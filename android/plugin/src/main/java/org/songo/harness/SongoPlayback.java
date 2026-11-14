package org.songo.harness;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.drawable.Icon;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

/**
 * Follows the engine's playback and does what Android needs of a music player
 * around it: while music is on, a foreground service with a media
 * notification (so the process survives the screen going off or another app
 * coming forward), a media session for the lock screen, headsets and
 * Bluetooth, audio focus, pausing when headphones are unplugged, and wake
 * locks. The engine itself carries on playing and moving through the queue on
 * its own thread, so none of this needs Songo's activity to be running.
 *
 * Everything but the event thread runs on the main thread.
 */
final class SongoPlayback {
	private static final String TAG = "SongoHarness";
	private static final String CHANNEL_ID = "songo_playback";

	// SongoState, engine/include/songo_engine.h
	static final int STATE_STOPPED = 0;
	static final int STATE_LOADING = 1;
	static final int STATE_PLAYING = 2;
	static final int STATE_PAUSED = 3;

	private static SongoPlayback instance;

	static synchronized SongoPlayback get(Context context) {
		if (instance == null) {
			instance = new SongoPlayback(context.getApplicationContext());
		}
		return instance;
	}

	private final Context context;
	private final Handler main = new Handler(Looper.getMainLooper());

	private long handle;
	private Thread eventThread;
	private volatile boolean running;

	private MediaSession session;
	private AudioManager audio;
	private AudioFocusRequest focusRequest;
	private boolean hasFocus;
	// Paused for a call or another app's audio, so it resumes afterwards
	private boolean pausedByFocusLoss;
	private PowerManager.WakeLock wakeLock;
	private WifiManager.WifiLock wifiLock;
	private boolean noisyRegistered;
	private boolean serviceStarted;
	private SongoPlaybackService service;

	int state = STATE_STOPPED;
	private TrackInfo track = new TrackInfo();
	private String streamTitle = "";
	// The position last given to the session, to spot seeks made elsewhere
	private long reportedPositionMs;
	private long reportedAtMs;

	private SongoPlayback(Context context) {
		this.context = context;
	}

	/** False if the engine isn't usable here (pre Android 8.0, or not in the APK). */
	boolean start() {
		if (handle != 0) {
			return true;
		}
		if (Build.VERSION.SDK_INT < 26 || !SongoNative.AVAILABLE) {
			return false;
		}
		handle = SongoNative.acquire();
		if (handle == 0) {
			Log.w(TAG, "The music engine didn't start, background playback is off");
			return false;
		}
		audio = context.getSystemService(AudioManager.class);
		focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
				.setAudioAttributes(new AudioAttributes.Builder()
						.setUsage(AudioAttributes.USAGE_MEDIA)
						.setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
						.build())
				.setOnAudioFocusChangeListener(this::onAudioFocusChange, main)
				.build();

		PowerManager power = context.getSystemService(PowerManager.class);
		wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Songo:playback");
		wakeLock.setReferenceCounted(false);
		WifiManager wifi = context.getSystemService(WifiManager.class);
		if (wifi != null) {
			wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "Songo:stream");
			wifiLock.setReferenceCounted(false);
		}

		session = new MediaSession(context, "Songo");
		session.setCallback(new SessionCallback(), main);
		Intent launch = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
		if (launch != null) {
			session.setSessionActivity(PendingIntent.getActivity(context, 0, launch, PendingIntent.FLAG_IMMUTABLE));
		}
		updatePlaybackState();

		running = true;
		eventThread = new Thread(this::eventLoop, "SongoEvents");
		eventThread.start();
		main.postDelayed(ticker, 1000);
		return true;
	}

	/** Songo's activity is going away, and the process with it. */
	void shutdown() {
		if (handle == 0) {
			return;
		}
		running = false;
		try {
			eventThread.join(1000);
		} catch (InterruptedException e) {
			Thread.currentThread().interrupt();
		}
		main.removeCallbacks(ticker);
		onState(STATE_STOPPED);
		session.release();
		SongoNative.release(handle);
		handle = 0;
	}

	void command(String line) {
		if (handle != 0 && line != null) {
			SongoNative.command(handle, line);
		}
	}

	void attach(SongoPlaybackService service) {
		this.service = service;
	}

	void detach(SongoPlaybackService service) {
		if (this.service == service) {
			this.service = null;
		}
	}

	// ---- Engine events ----

	private void eventLoop() {
		while (running) {
			String line = SongoNative.nextEvent(handle, 500);
			if (line == null) {
				continue;
			}
			String[] fields = Protocol.split(line);
			switch (fields[0]) {
				case "state": {
					int newState = Protocol.intField(fields, 1);
					main.post(() -> onState(newState));
				} break;
				case "track": {
					// Tags are read here, off the main thread
					TrackInfo info = TrackInfo.read(Protocol.field(fields, 4), Protocol.doubleField(fields, 3), Protocol.intField(fields, 2));
					main.post(() -> onTrack(info));
				} break;
				case "title": {
					String title = Protocol.field(fields, 1);
					main.post(() -> onStreamTitle(title));
				} break;
				default:
					break;
			}
		}
	}

	private void onState(int newState) {
		state = newState;
		boolean active = newState == STATE_PLAYING || newState == STATE_LOADING;
		if (newState == STATE_PLAYING) {
			pausedByFocusLoss = false;
		}
		if (active) {
			requestFocus();
			wakeLock.acquire();
			if (wifiLock != null && track.path.contains("://")) {
				wifiLock.acquire();
			}
			registerNoisy();
		} else {
			releaseLocks();
			unregisterNoisy();
		}

		if (newState == STATE_STOPPED) {
			abandonFocus();
			stopService();
			session.setActive(false);
		} else {
			session.setActive(true);
			startService();
		}
		updatePlaybackState();
		updateNotification();
	}

	private void onTrack(TrackInfo info) {
		track = info;
		streamTitle = "";
		if (wifiLock != null && (state == STATE_PLAYING || state == STATE_LOADING)) {
			if (track.path.contains("://")) {
				wifiLock.acquire();
			} else if (wifiLock.isHeld()) {
				wifiLock.release();
			}
		}
		updateMetadata();
		updatePlaybackState();
		updateNotification();
	}

	private void onStreamTitle(String title) {
		streamTitle = title;
		updateMetadata();
		updateNotification();
	}

	// ---- Session and notification ----

	// Live radio shows "now playing" from the stream as the title
	private String displayTitle() {
		return !streamTitle.isEmpty() ? streamTitle : track.title;
	}

	private String displaySubtitle() {
		return !streamTitle.isEmpty() ? track.title : track.artist;
	}

	private void updateMetadata() {
		MediaMetadata.Builder metadata = new MediaMetadata.Builder()
				.putString(MediaMetadata.METADATA_KEY_TITLE, displayTitle())
				.putString(MediaMetadata.METADATA_KEY_ARTIST, displaySubtitle())
				.putString(MediaMetadata.METADATA_KEY_ALBUM, track.album);
		if (track.durationMs > 0) {
			metadata.putLong(MediaMetadata.METADATA_KEY_DURATION, track.durationMs);
		}
		if (track.art != null) {
			metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, track.art);
		}
		session.setMetadata(metadata.build());
	}

	private void updatePlaybackState() {
		double[] status = handle != 0 ? SongoNative.status(handle) : null;
		long positionMs = status != null ? (long) (status[2] * 1000.0) : 0;
		int sessionState;
		switch (state) {
			case STATE_PLAYING:
				sessionState = PlaybackState.STATE_PLAYING;
				break;
			case STATE_PAUSED:
				sessionState = PlaybackState.STATE_PAUSED;
				break;
			case STATE_LOADING:
				sessionState = PlaybackState.STATE_BUFFERING;
				break;
			default:
				sessionState = PlaybackState.STATE_STOPPED;
		}
		long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE | PlaybackState.ACTION_PLAY_PAUSE
				| PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS | PlaybackState.ACTION_STOP;
		if (!track.live) {
			actions |= PlaybackState.ACTION_SEEK_TO;
		}
		reportedPositionMs = positionMs;
		reportedAtMs = SystemClock.elapsedRealtime();
		session.setPlaybackState(new PlaybackState.Builder()
				.setActions(actions)
				.setState(sessionState, positionMs, state == STATE_PLAYING ? 1.0f : 0.0f, reportedAtMs)
				.build());
	}

	// The session extrapolates the position itself. This catches the times
	// it's wrong: a seek from Songo, or the engine moving on.
	private final Runnable ticker = new Runnable() {
		@Override
		public void run() {
			if (handle == 0) {
				return;
			}
			if (state != STATE_STOPPED) {
				double[] status = SongoNative.status(handle);
				if (status != null) {
					long expected = reportedPositionMs + (state == STATE_PLAYING ? SystemClock.elapsedRealtime() - reportedAtMs : 0);
					if (Math.abs((long) (status[2] * 1000.0) - expected) > 750) {
						updatePlaybackState();
					}
				}
			}
			main.postDelayed(this, 1000);
		}
	};

	Notification buildNotification() {
		NotificationManager notifications = context.getSystemService(NotificationManager.class);
		if (notifications.getNotificationChannel(CHANNEL_ID) == null) {
			NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "Playback", NotificationManager.IMPORTANCE_LOW);
			channel.setShowBadge(false);
			notifications.createNotificationChannel(channel);
		}
		boolean playing = state == STATE_PLAYING || state == STATE_LOADING;
		Notification.Builder builder = new Notification.Builder(context, CHANNEL_ID)
				.setSmallIcon(R.drawable.songo_notification)
				.setContentTitle(displayTitle())
				.setContentText(displaySubtitle())
				.setLargeIcon(track.art)
				.setShowWhen(false)
				.setOngoing(true)
				.setVisibility(Notification.VISIBILITY_PUBLIC)
				.setCategory(Notification.CATEGORY_TRANSPORT)
				.setContentIntent(session.getController().getSessionActivity())
				.addAction(action(android.R.drawable.ic_media_previous, "Previous", "previous", 1))
				.addAction(playing
						? action(android.R.drawable.ic_media_pause, "Pause", "pause", 2)
						: action(android.R.drawable.ic_media_play, "Play", "resume", 3))
				.addAction(action(android.R.drawable.ic_media_next, "Next", "next", 4))
				.setStyle(new Notification.MediaStyle()
						.setMediaSession(session.getSessionToken())
						.setShowActionsInCompactView(0, 1, 2));
		if (Build.VERSION.SDK_INT >= 31) {
			builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
		}
		return builder.build();
	}

	private Notification.Action action(int icon, String label, String command, int requestCode) {
		Intent intent = new Intent(context, SongoPlaybackService.class)
				.setAction(SongoPlaybackService.ACTION_COMMAND)
				.putExtra(SongoPlaybackService.EXTRA_LINE, command);
		PendingIntent pending = PendingIntent.getService(context, requestCode, intent,
				PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
		return new Notification.Action.Builder(Icon.createWithResource("android", icon), label, pending).build();
	}

	private void updateNotification() {
		if (service != null && state != STATE_STOPPED) {
			service.update(buildNotification());
		}
	}

	private void startService() {
		if (serviceStarted) {
			return;
		}
		try {
			context.startForegroundService(new Intent(context, SongoPlaybackService.class));
			serviceStarted = true;
		} catch (RuntimeException e) {
			// Android 12+ won't start one from the background. Playback starts
			// from Songo or the notification, so this shouldn't happen.
			Log.w(TAG, "Couldn't start the playback service", e);
		}
	}

	private void stopService() {
		if (!serviceStarted) {
			return;
		}
		serviceStarted = false;
		context.stopService(new Intent(context, SongoPlaybackService.class));
	}

	// ---- Audio focus, headphones, wake locks ----

	private void requestFocus() {
		if (hasFocus) {
			return;
		}
		int result = audio.requestAudioFocus(focusRequest);
		hasFocus = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
		if (result == AudioManager.AUDIOFOCUS_REQUEST_FAILED) {
			// Something more important has the audio, like a call
			command("pause");
		}
	}

	private void abandonFocus() {
		if (hasFocus) {
			audio.abandonAudioFocusRequest(focusRequest);
			hasFocus = false;
		}
		pausedByFocusLoss = false;
	}

	// Ducking for notifications and navigation is left to the system, which
	// does it automatically from Android 8.0
	private void onAudioFocusChange(int change) {
		boolean active = state == STATE_PLAYING || state == STATE_LOADING;
		switch (change) {
			case AudioManager.AUDIOFOCUS_LOSS:
				hasFocus = false;
				pausedByFocusLoss = false;
				if (active) {
					command("pause");
				}
				break;
			case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
				if (active) {
					pausedByFocusLoss = true;
					command("pause");
				}
				break;
			case AudioManager.AUDIOFOCUS_GAIN:
				hasFocus = true;
				if (pausedByFocusLoss) {
					pausedByFocusLoss = false;
					command("resume");
				}
				break;
			default:
				break;
		}
	}

	private final BroadcastReceiver noisyReceiver = new BroadcastReceiver() {
		@Override
		public void onReceive(Context context, Intent intent) {
			// Headphones unplugged or Bluetooth gone: don't carry on out of the speaker
			if (AudioManager.ACTION_AUDIO_BECOMING_NOISY.equals(intent.getAction())) {
				command("pause");
			}
		}
	};

	private void registerNoisy() {
		if (!noisyRegistered) {
			context.registerReceiver(noisyReceiver, new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));
			noisyRegistered = true;
		}
	}

	private void unregisterNoisy() {
		if (noisyRegistered) {
			context.unregisterReceiver(noisyReceiver);
			noisyRegistered = false;
		}
	}

	private void releaseLocks() {
		if (wakeLock.isHeld()) {
			wakeLock.release();
		}
		if (wifiLock != null && wifiLock.isHeld()) {
			wifiLock.release();
		}
	}

	// Lock screen, headset buttons, Bluetooth, Android Auto and the like
	private final class SessionCallback extends MediaSession.Callback {
		@Override
		public void onPlay() {
			command("resume");
		}

		@Override
		public void onPause() {
			command("pause");
		}

		@Override
		public void onSkipToNext() {
			command("next");
		}

		@Override
		public void onSkipToPrevious() {
			command("previous");
		}

		@Override
		public void onStop() {
			command("stop");
		}

		@Override
		public void onSeekTo(long positionMs) {
			command("seek\t" + (positionMs / 1000.0));
		}
	}
}
