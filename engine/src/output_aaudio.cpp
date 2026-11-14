// Android output through AAudio, in its power saving mode with a large
// buffer, which is the deep buffer path meant for music with the screen off.
//
// AAudio arrived in Android 8.0 (API 26) while Godot supports API 21, so it's
// loaded at runtime. create_aaudio_output() returns null where it's missing,
// and the host falls back to its own player.

#include "output.h"

#include "songo_shared.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <dlfcn.h>

#include <algorithm>
#include <vector>

namespace songo {

namespace {

const int CHANNELS = 2;
const int64_t WRITE_TIMEOUT_NS = 500000000;
const int64_t STATE_TIMEOUT_NS = 200000000;

#define SONGO_LOG(...) __android_log_print(ANDROID_LOG_INFO, "songo_engine", __VA_ARGS__)

// Spelled out rather than decltype'd: below API 26 the NDK marks the real
// declarations unavailable, which rules out naming them at all
struct AAudioApi {
	aaudio_result_t (*AAudio_createStreamBuilder)(AAudioStreamBuilder **) = nullptr;
	const char *(*AAudio_convertResultToText)(aaudio_result_t) = nullptr;
	void (*AAudioStreamBuilder_setSampleRate)(AAudioStreamBuilder *, int32_t) = nullptr;
	void (*AAudioStreamBuilder_setChannelCount)(AAudioStreamBuilder *, int32_t) = nullptr;
	void (*AAudioStreamBuilder_setFormat)(AAudioStreamBuilder *, aaudio_format_t) = nullptr;
	void (*AAudioStreamBuilder_setSharingMode)(AAudioStreamBuilder *, aaudio_sharing_mode_t) = nullptr;
	void (*AAudioStreamBuilder_setPerformanceMode)(AAudioStreamBuilder *, aaudio_performance_mode_t) = nullptr;
	void (*AAudioStreamBuilder_setDirection)(AAudioStreamBuilder *, aaudio_direction_t) = nullptr;
	aaudio_result_t (*AAudioStreamBuilder_openStream)(AAudioStreamBuilder *, AAudioStream **) = nullptr;
	aaudio_result_t (*AAudioStreamBuilder_delete)(AAudioStreamBuilder *) = nullptr;
	aaudio_result_t (*AAudioStream_close)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_requestStart)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_requestPause)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_requestFlush)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_requestStop)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_waitForStateChange)(AAudioStream *, aaudio_stream_state_t, aaudio_stream_state_t *, int64_t) = nullptr;
	aaudio_result_t (*AAudioStream_write)(AAudioStream *, const void *, int32_t, int64_t) = nullptr;
	int32_t (*AAudioStream_getSampleRate)(AAudioStream *) = nullptr;
	int32_t (*AAudioStream_getBufferCapacityInFrames)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_setBufferSizeInFrames)(AAudioStream *, int32_t) = nullptr;
	int64_t (*AAudioStream_getFramesWritten)(AAudioStream *) = nullptr;
	int64_t (*AAudioStream_getFramesRead)(AAudioStream *) = nullptr;
	aaudio_result_t (*AAudioStream_getTimestamp)(AAudioStream *, clockid_t, int64_t *, int64_t *) = nullptr;

	bool load() {
		void *lib = dlopen("libaaudio.so", RTLD_NOW);
		if (!lib) {
			return false;
		}
		bool ok = true;
#define SONGO_AAUDIO_LOAD(name)                                     \
	name = reinterpret_cast<decltype(name)>(dlsym(lib, #name)); \
	ok = ok && name != nullptr;
		SONGO_AAUDIO_LOAD(AAudio_createStreamBuilder)
		SONGO_AAUDIO_LOAD(AAudio_convertResultToText)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setSampleRate)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setChannelCount)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setFormat)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setSharingMode)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setPerformanceMode)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_setDirection)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_openStream)
		SONGO_AAUDIO_LOAD(AAudioStreamBuilder_delete)
		SONGO_AAUDIO_LOAD(AAudioStream_close)
		SONGO_AAUDIO_LOAD(AAudioStream_requestStart)
		SONGO_AAUDIO_LOAD(AAudioStream_requestPause)
		SONGO_AAUDIO_LOAD(AAudioStream_requestFlush)
		SONGO_AAUDIO_LOAD(AAudioStream_requestStop)
		SONGO_AAUDIO_LOAD(AAudioStream_waitForStateChange)
		SONGO_AAUDIO_LOAD(AAudioStream_write)
		SONGO_AAUDIO_LOAD(AAudioStream_getSampleRate)
		SONGO_AAUDIO_LOAD(AAudioStream_getBufferCapacityInFrames)
		SONGO_AAUDIO_LOAD(AAudioStream_setBufferSizeInFrames)
		SONGO_AAUDIO_LOAD(AAudioStream_getFramesWritten)
		SONGO_AAUDIO_LOAD(AAudioStream_getFramesRead)
		SONGO_AAUDIO_LOAD(AAudioStream_getTimestamp)
#undef SONGO_AAUDIO_LOAD
		return ok;
	}
};

AAudioApi api;

class AAudioOutput : public Output {
public:
	AAudioOutput(int p_sample_rate, int p_latency_ms) :
			sample_rate(p_sample_rate), latency_frames((int32_t)((int64_t)p_sample_rate * p_latency_ms / 1000)) {}

	~AAudioOutput() override { close(); }

	bool open(const std::function<bool()> &, std::string &r_error) override {
		if (stream) {
			return true;
		}
		AAudioStreamBuilder *builder = nullptr;
		aaudio_result_t result = api.AAudio_createStreamBuilder(&builder);
		if (result != AAUDIO_OK) {
			r_error = std::string("Can't set up AAudio: ") + api.AAudio_convertResultToText(result);
			return false;
		}
		api.AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
		api.AAudioStreamBuilder_setSampleRate(builder, sample_rate);
		api.AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
		api.AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
		api.AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
		api.AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
		result = api.AAudioStreamBuilder_openStream(builder, &stream);
		api.AAudioStreamBuilder_delete(builder);
		if (result != AAUDIO_OK) {
			stream = nullptr;
			r_error = std::string("Can't open an AAudio stream: ") + api.AAudio_convertResultToText(result);
			return false;
		}
		int32_t actual_rate = api.AAudioStream_getSampleRate(stream);
		if (actual_rate != sample_rate) {
			r_error = "AAudio wants " + std::to_string(actual_rate) + "Hz rather than " + std::to_string(sample_rate) + "Hz";
			close();
			return false;
		}
		// As much as the latency asks for, the screen off path likes it large
		int32_t capacity = api.AAudioStream_getBufferCapacityInFrames(stream);
		api.AAudioStream_setBufferSizeInFrames(stream, std::min(capacity, std::max(latency_frames, 1)));
		paused = false;
		started = false;
		SONGO_LOG("AAudio stream open, %d Hz, buffer capacity %d frames", actual_rate, capacity);
		return true;
	}

	void close() override {
		if (stream) {
			api.AAudioStream_requestStop(stream);
			api.AAudioStream_close(stream);
			stream = nullptr;
		}
	}

	bool is_open() const override { return stream != nullptr; }

	void write(const float *frames, int count) override {
		if (!stream || paused) {
			return;
		}
		// The only place samples get clipped, so EQ boosts and blends can go
		// past full scale on the way, like they can on Godot's float buses
		samples.resize((size_t)count * CHANNELS);
		for (size_t i = 0; i < samples.size(); i++) {
			samples[i] = std::clamp(frames[i], -1.0f, 1.0f);
		}
		if (!started) {
			api.AAudioStream_requestStart(stream);
			started = true;
		}
		const float *cursor = samples.data();
		while (count > 0) {
			aaudio_result_t written = api.AAudioStream_write(stream, cursor, count, WRITE_TIMEOUT_NS);
			if (written == AAUDIO_ERROR_DISCONNECTED) {
				// The output device went (headphones unplugged, Bluetooth
				// dropped). Carry on through whatever's the default now.
				SONGO_LOG("AAudio stream disconnected, reopening");
				close();
				std::string error;
				if (!open([]() { return false; }, error)) {
					SONGO_LOG("%s", error.c_str());
				}
				return;
			}
			if (written < 0) {
				SONGO_LOG("AAudio write failed: %s", api.AAudio_convertResultToText(written));
				return;
			}
			cursor += (size_t)written * CHANNELS;
			count -= written;
		}
	}

	int64_t delay_frames() override {
		if (!stream) {
			return 0;
		}
		int64_t written = api.AAudioStream_getFramesWritten(stream);
		int64_t position = 0;
		int64_t time_ns = 0;
		if (!paused && api.AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &position, &time_ns) == AAUDIO_OK) {
			// Where the speaker is now, from where it was at time_ns
			int64_t presented = position + (monotonic_ns() - time_ns) * sample_rate / 1000000000;
			return std::clamp<int64_t>(written - presented, 0, written);
		}
		return std::max<int64_t>(written - api.AAudioStream_getFramesRead(stream), 0);
	}

	void flush() override {
		if (!stream) {
			return;
		}
		// Only a paused stream can be flushed
		api.AAudioStream_requestPause(stream);
		wait_for_state(AAUDIO_STREAM_STATE_PAUSING);
		api.AAudioStream_requestFlush(stream);
		wait_for_state(AAUDIO_STREAM_STATE_FLUSHING);
		// Starts again with the next write, as after a pause
		started = false;
		paused = false;
	}

	void drain() override {
		if (!stream || paused || !started) {
			return;
		}
		// Stopping an output stream plays out what's queued first
		api.AAudioStream_requestStop(stream);
		aaudio_stream_state_t state = AAUDIO_STREAM_STATE_STOPPING;
		for (int i = 0; i < 20 && state == AAUDIO_STREAM_STATE_STOPPING; i++) {
			api.AAudioStream_waitForStateChange(stream, AAUDIO_STREAM_STATE_STOPPING, &state, STATE_TIMEOUT_NS);
		}
		started = false;
	}

	void set_paused(bool p_paused) override {
		if (!stream || p_paused == paused) {
			return;
		}
		if (p_paused) {
			api.AAudioStream_requestPause(stream);
		} else if (started) {
			api.AAudioStream_requestStart(stream);
		}
		paused = p_paused;
	}

private:
	int sample_rate;
	int32_t latency_frames;
	AAudioStream *stream = nullptr;
	bool paused = false;
	// Started on the first write, so a song opening doesn't start a silent stream
	bool started = false;
	std::vector<float> samples;

	void wait_for_state(aaudio_stream_state_t transitional) {
		aaudio_stream_state_t state = transitional;
		api.AAudioStream_waitForStateChange(stream, transitional, &state, STATE_TIMEOUT_NS);
	}
};

} // namespace

std::unique_ptr<Output> create_aaudio_output(int sample_rate, int latency_ms) {
	static bool loaded = api.load();
	if (!loaded) {
		return nullptr;
	}
	return std::make_unique<AAudioOutput>(sample_rate, latency_ms);
}

} // namespace songo
