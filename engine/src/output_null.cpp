// Plays nothing, but takes audio at the speed a real device would, with the
// same latency. Lets the daemon and SongoPlayerHarness run headless, in tests
// and wherever the sound device is busy.

#include "output.h"

#include "songo_shared.h"

#include <algorithm>
#include <ctime>

namespace songo {

namespace {

class NullOutput : public Output {
public:
	NullOutput(int p_sample_rate, int p_latency_ms) :
			sample_rate(p_sample_rate), latency_frames((int64_t)p_sample_rate * p_latency_ms / 1000) {}

	bool open(const std::function<bool()> &, std::string &) override {
		opened = true;
		reset_clock();
		return true;
	}

	void close() override { opened = false; }
	bool is_open() const override { return opened; }

	void write(const float *, int count) override {
		written += count;
		// Block until no more than the latency is queued, as a device would
		while (!paused && delay_frames() > latency_frames) {
			int64_t excess = delay_frames() - latency_frames;
			int64_t wait_ns = std::max<int64_t>(excess * 1000000000 / sample_rate, 1000000);
			timespec ts = { (time_t)(wait_ns / 1000000000), (long)(wait_ns % 1000000000) };
			nanosleep(&ts, nullptr);
		}
	}

	int64_t delay_frames() override { return std::max<int64_t>(written - played(), 0); }

	void flush() override {
		written = played();
	}

	void drain() override {
		while (!paused && delay_frames() > 0) {
			timespec ts = { 0, 5000000 };
			nanosleep(&ts, nullptr);
		}
	}

	void set_paused(bool p_paused) override {
		if (p_paused == paused) {
			return;
		}
		if (p_paused) {
			played_before_pause = played();
		} else {
			// Carry on from where the virtual speaker stopped
			start_ns = monotonic_ns();
			played_base = played_before_pause;
		}
		paused = p_paused;
	}

private:
	int sample_rate;
	int64_t latency_frames;
	bool opened = false;
	bool paused = false;
	int64_t written = 0;
	int64_t start_ns = 0;
	int64_t played_base = 0;
	int64_t played_before_pause = 0;

	void reset_clock() {
		written = 0;
		played_base = 0;
		start_ns = monotonic_ns();
	}

	// Frames the virtual speaker has played, which never runs ahead of what
	// was written (that would be an underrun)
	int64_t played() {
		if (paused) {
			return played_before_pause;
		}
		int64_t elapsed = (monotonic_ns() - start_ns) * sample_rate / 1000000000;
		int64_t now = played_base + elapsed;
		if (now > written) {
			// Underrun: restart the clock from here so it doesn't bank time
			played_base = written;
			start_ns = monotonic_ns();
			return written;
		}
		return now;
	}
};

} // namespace

std::unique_ptr<Output> create_null_output(int sample_rate, int latency_ms) {
	return std::make_unique<NullOutput>(sample_rate, latency_ms);
}

} // namespace songo
