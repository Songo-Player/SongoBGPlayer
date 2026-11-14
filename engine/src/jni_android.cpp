// JNI for the Android plugin's playback service (org.songo.harness.SongoNative).
//
// It shares the process's one engine with SongoPlayerHarness through
// songo_engine_acquire_shared, so the lock screen, notification and Songo all
// drive the same playback, and the service keeps it alive while Songo's
// activity is in the background. Commands and events are protocol lines
// (common/protocol.h), the same ones the daemon speaks.

#include "protocol.h"
#include "songo_engine.h"
#include "songo_shared.h"

#include <jni.h>
#include <poll.h>

#include <string>
#include <vector>

using namespace songo;

namespace {

struct Handle {
	SongoEngine *engine;
	SongoSubscription *subscription;
};

// JNI's own UTF-8 is "modified" (no 4 byte sequences), so paths with emoji
// would be mangled going through NewStringUTF/GetStringUTFChars. Convert
// properly through UTF-16 instead.
std::string to_utf8(JNIEnv *env, jstring string) {
	std::string out;
	if (!string) {
		return out;
	}
	const jsize length = env->GetStringLength(string);
	const jchar *chars = env->GetStringChars(string, nullptr);
	for (jsize i = 0; i < length; i++) {
		uint32_t c = chars[i];
		if (c >= 0xD800 && c <= 0xDBFF && i + 1 < length && chars[i + 1] >= 0xDC00 && chars[i + 1] <= 0xDFFF) {
			c = 0x10000 + ((c - 0xD800) << 10) + (chars[++i] - 0xDC00);
		}
		if (c < 0x80) {
			out += (char)c;
		} else if (c < 0x800) {
			out += (char)(0xC0 | (c >> 6));
			out += (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out += (char)(0xE0 | (c >> 12));
			out += (char)(0x80 | ((c >> 6) & 0x3F));
			out += (char)(0x80 | (c & 0x3F));
		} else {
			out += (char)(0xF0 | (c >> 18));
			out += (char)(0x80 | ((c >> 12) & 0x3F));
			out += (char)(0x80 | ((c >> 6) & 0x3F));
			out += (char)(0x80 | (c & 0x3F));
		}
	}
	env->ReleaseStringChars(string, chars);
	return out;
}

jstring to_java(JNIEnv *env, const std::string &utf8) {
	std::vector<jchar> out;
	out.reserve(utf8.size());
	for (size_t i = 0; i < utf8.size();) {
		unsigned char c = (unsigned char)utf8[i];
		uint32_t code;
		int extra;
		if (c < 0x80) {
			code = c;
			extra = 0;
		} else if ((c >> 5) == 0x6) {
			code = c & 0x1F;
			extra = 1;
		} else if ((c >> 4) == 0xE) {
			code = c & 0x0F;
			extra = 2;
		} else if ((c >> 3) == 0x1E) {
			code = c & 0x07;
			extra = 3;
		} else {
			// Not UTF-8, keep the byte as Latin-1
			code = c;
			extra = 0;
		}
		bool valid = i + extra < utf8.size();
		for (int k = 1; valid && k <= extra; k++) {
			valid = ((unsigned char)utf8[i + k] & 0xC0) == 0x80;
		}
		if (!valid) {
			// Cut short or not UTF-8 after all, keep the byte as Latin-1
			code = c;
			extra = 0;
		}
		for (int k = 1; k <= extra; k++) {
			code = (code << 6) | ((unsigned char)utf8[i + k] & 0x3F);
		}
		i += extra + 1;
		if (code >= 0x10000) {
			code -= 0x10000;
			out.push_back((jchar)(0xD800 + (code >> 10)));
			out.push_back((jchar)(0xDC00 + (code & 0x3FF)));
		} else {
			out.push_back((jchar)code);
		}
	}
	return env->NewString(out.data(), (jsize)out.size());
}

Handle *handle_of(jlong handle) {
	return reinterpret_cast<Handle *>(handle);
}

} // namespace

extern "C" {

// The shared engine and an event subscription on it, 0 if the engine
// couldn't start (no AAudio before Android 8.0)
JNIEXPORT jlong JNICALL Java_org_songo_harness_SongoNative_acquire(JNIEnv *, jclass) {
	SongoEngine *engine = songo_engine_acquire_shared(nullptr);
	if (!engine) {
		return 0;
	}
	return reinterpret_cast<jlong>(new Handle{ engine, songo_engine_subscribe(engine) });
}

JNIEXPORT void JNICALL Java_org_songo_harness_SongoNative_release(JNIEnv *, jclass, jlong handle) {
	Handle *h = handle_of(handle);
	if (!h) {
		return;
	}
	songo_subscription_close(h->subscription);
	songo_engine_release_shared(h->engine);
	delete h;
}

// One protocol command line, e.g. "pause" or "seek\t42.5"
JNIEXPORT void JNICALL Java_org_songo_harness_SongoNative_command(JNIEnv *env, jclass, jlong handle, jstring line) {
	Handle *h = handle_of(handle);
	if (h) {
		dispatch_command(h->engine, decode_line(to_utf8(env, line)));
	}
}

// The next event as a protocol line without its newline, waiting up to
// `timeout_ms` for one. Null if none came.
JNIEXPORT jstring JNICALL Java_org_songo_harness_SongoNative_nextEvent(JNIEnv *env, jclass, jlong handle, jint timeout_ms) {
	Handle *h = handle_of(handle);
	if (!h) {
		return nullptr;
	}
	SongoEvent event;
	if (!songo_subscription_poll(h->subscription, &event)) {
		pollfd fd = { songo_subscription_fd(h->subscription), POLLIN, 0 };
		if (fd.fd < 0 || poll(&fd, 1, timeout_ms) <= 0 || !songo_subscription_poll(h->subscription, &event)) {
			return nullptr;
		}
	}
	std::string line = encode_line(event_message(event));
	line.pop_back();
	return to_java(env, line);
}

// { state, queue index, position (s), duration (s), volume }, or null
JNIEXPORT jdoubleArray JNICALL Java_org_songo_harness_SongoNative_status(JNIEnv *env, jclass, jlong handle) {
	Handle *h = handle_of(handle);
	if (!h) {
		return nullptr;
	}
	size_t size = 0;
	const Shared *shared = static_cast<const Shared *>(songo_engine_shared(h->engine, &size));
	Status status;
	if (!shared_is_compatible(shared, size) || !read_status(shared, status)) {
		return nullptr;
	}
	jdouble values[5] = {
		(jdouble)status.state,
		(jdouble)status.index,
		status_position_at(status, monotonic_ns()),
		status.duration,
		(jdouble)status.volume,
	};
	jdoubleArray out = env->NewDoubleArray(5);
	env->SetDoubleArrayRegion(out, 0, 5, values);
	return out;
}

} // extern "C"
