package org.songo.harness;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * The foreground service that keeps Songo's process alive and playing while
 * music is on (paused included), and carries the media notification. Started
 * and stopped by SongoPlayback as the engine's state changes. The
 * notification's buttons come back here as commands for the engine.
 */
public class SongoPlaybackService extends Service {
	static final String ACTION_COMMAND = "org.songo.harness.COMMAND";
	static final String EXTRA_LINE = "line";
	private static final int NOTIFICATION_ID = 0x5047;

	@Override
	public void onCreate() {
		super.onCreate();
		SongoPlayback.get(this).attach(this);
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		SongoPlayback playback = SongoPlayback.get(this);
		// Has to happen within seconds of every start, before anything else
		Notification notification = playback.buildNotification();
		if (Build.VERSION.SDK_INT >= 29) {
			startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
		} else {
			startForeground(NOTIFICATION_ID, notification);
		}
		if (intent != null && ACTION_COMMAND.equals(intent.getAction())) {
			playback.command(intent.getStringExtra(EXTRA_LINE));
		} else if (playback.state == SongoPlayback.STATE_STOPPED) {
			// Music stopped between asking for the service and it starting
			stopSelf();
		}
		return START_NOT_STICKY;
	}

	void update(Notification notification) {
		getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, notification);
	}

	@Override
	public void onDestroy() {
		SongoPlayback.get(this).detach(this);
		stopForeground(STOP_FOREGROUND_REMOVE);
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}
}
