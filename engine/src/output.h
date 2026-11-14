// Where the engine's mixed audio goes. The engine thread is the only caller,
// and write() blocking until there's room is what paces decoding.

#ifndef SONGO_OUTPUT_H
#define SONGO_OUTPUT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace songo {

class Output {
public:
	virtual ~Output() = default;

	// `cancelled` is polled while waiting on a busy device, so a stop or
	// quit isn't held up by it
	virtual bool open(const std::function<bool()> &cancelled, std::string &r_error) = 0;
	virtual void close() = 0;
	virtual bool is_open() const = 0;

	// Interleaved stereo floats, clipped here if they go past full scale
	virtual void write(const float *frames, int count) = 0;
	// Frames written that haven't reached the speaker yet
	virtual int64_t delay_frames() = 0;
	// Throws away everything not yet heard, so a skip is heard straight away
	virtual void flush() = 0;
	// Lets everything written play out
	virtual void drain() = 0;
	virtual void set_paused(bool paused) = 0;
};

// "alsa" (Linux), "aaudio" (Android 8.0+), "wasapi" (Windows) or "null",
// empty for the platform default. Null if unknown or unavailable here.
std::unique_ptr<Output> create_output(const std::string &backend, const std::string &device,
		int sample_rate, int latency_ms);

} // namespace songo

#endif // SONGO_OUTPUT_H
