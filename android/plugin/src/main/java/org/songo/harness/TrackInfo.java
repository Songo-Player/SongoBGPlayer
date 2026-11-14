package org.songo.harness;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;

/**
 * What the notification and lock screen show for a song. Read from the file's
 * own tags, since Songo's library isn't reachable while it's in the
 * background and the engine moves on by itself.
 */
final class TrackInfo {
	private static final int ART_SIZE = 512;

	String path = "";
	boolean live;
	long durationMs = -1;
	String title = "";
	String artist = "";
	String album = "";
	Bitmap art;

	/** Reads tags and cover art, which takes a while, so not on the main thread. */
	static TrackInfo read(String path, double durationSec, int flags) {
		TrackInfo info = new TrackInfo();
		info.path = path;
		info.live = (flags & 1) != 0;
		info.durationMs = durationSec > 0 ? (long) (durationSec * 1000.0) : -1;
		info.title = fileTitle(path);
		if (path.contains("://")) {
			return info;
		}
		MediaMetadataRetriever retriever = new MediaMetadataRetriever();
		try {
			retriever.setDataSource(path);
			info.title = orElse(retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_TITLE), info.title);
			info.artist = orElse(retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ARTIST), "");
			info.album = orElse(retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ALBUM), "");
			byte[] picture = retriever.getEmbeddedPicture();
			if (picture != null) {
				info.art = decodeArt(picture);
			}
		} catch (RuntimeException e) {
			// A format Android can't read tags from, the file name will do
		} finally {
			try {
				retriever.release();
			} catch (Exception e) {
				// Nothing to do about it
			}
		}
		return info;
	}

	private static String orElse(String value, String fallback) {
		return value == null || value.trim().isEmpty() ? fallback : value.trim();
	}

	private static String fileTitle(String path) {
		String name = path.substring(path.lastIndexOf('/') + 1);
		int dot = name.lastIndexOf('.');
		return dot > 0 && !path.contains("://") ? name.substring(0, dot) : name;
	}

	// Scaled down while decoding, so a huge embedded cover doesn't cost much
	private static Bitmap decodeArt(byte[] data) {
		BitmapFactory.Options bounds = new BitmapFactory.Options();
		bounds.inJustDecodeBounds = true;
		BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
		BitmapFactory.Options options = new BitmapFactory.Options();
		options.inSampleSize = 1;
		while (bounds.outWidth / (options.inSampleSize * 2) >= ART_SIZE && bounds.outHeight / (options.inSampleSize * 2) >= ART_SIZE) {
			options.inSampleSize *= 2;
		}
		return BitmapFactory.decodeByteArray(data, 0, data.length, options);
	}
}
