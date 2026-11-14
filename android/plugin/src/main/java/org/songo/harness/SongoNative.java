package org.songo.harness;

/**
 * JNI into libsongo_engine (engine/src/jni_android.cpp), which this AAR
 * bundles. The engine is the one Songodot's SongoPlayerHarness plays through,
 * shared within the process.
 */
final class SongoNative {
	/** False if the engine library isn't in the APK, so the plugin stays out of the way. */
	static final boolean AVAILABLE;

	static {
		boolean loaded;
		try {
			System.loadLibrary("songo_engine.android.arm64");
			loaded = true;
		} catch (UnsatisfiedLinkError e) {
			loaded = false;
		}
		AVAILABLE = loaded;
	}

	private SongoNative() {}

	/** The shared engine plus an event subscription on it, 0 if it couldn't start. */
	static native long acquire();

	static native void release(long handle);

	/** One protocol command line, e.g. "pause" or "seek\t42.5" (common/protocol.h). */
	static native void command(long handle, String line);

	/** The next event as a protocol line, waiting up to timeoutMs. Null if none came. */
	static native String nextEvent(long handle, int timeoutMs);

	/** { state, queue index, position (s), duration (s), volume }, or null. */
	static native double[] status(long handle);
}
