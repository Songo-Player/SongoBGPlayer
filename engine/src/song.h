// One song being decoded to interleaved stereo floats at the engine's rate.
// Pulled from by the engine thread, so two can run at once during a blend.
// Network inputs can decode ahead on a thread of their own (read_ahead_sec),
// in which case a stall plays as silence rather than holding the engine up.

#ifndef SONGO_SONG_H
#define SONGO_SONG_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

struct SwrContext;

namespace songo {

struct SongOptions {
	int sample_rate = 44100;
	double network_timeout = 10.0;
	std::string user_agent;
	// Lets the engine abort a blocking open or read (a dead stream, a
	// stalled connection) when a command makes it pointless
	AVIOInterruptCB interrupt = { nullptr, nullptr };
	// Seconds of audio to keep decoded ahead for network inputs, on a thread
	// of their own, so a stalled connection doesn't stall the output. 0 reads
	// them on the engine thread like local files.
	double read_ahead_sec = 0.0;
};

class Song {
public:
	~Song();

	bool open(const std::string &path, double seek_sec, const SongOptions &options, std::string &r_error);

	// Fills up to `frames` frames. Fewer means the song has ended, or the
	// engine interrupted it (is_finished() tells which).
	int read(float *out, int frames);
	bool is_finished();

	bool seek(double seconds);

	// True and sets `r_title` when the Icecast/Shoutcast title changed since
	// the last call. Only checks twice a second.
	bool poll_title(std::string &r_title);

	const std::string &get_path() const { return path; }
	// <= 0 for live streams
	double get_duration() const { return duration; }
	// Where reading has got to, which is ahead of what's being heard
	double get_position() const { return position_base.load() + (double)frames_read / sample_rate; }
	bool is_live() const { return live; }
	bool is_seekable() const { return !live; }
	// Set when reading failed rather than reaching the end, once finished
	const std::string &get_error() const { return error; }

private:
	std::string path;
	int sample_rate = 44100;
	AVFormatContext *format_ctx = nullptr;
	AVCodecContext *codec_ctx = nullptr;
	SwrContext *swr = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	int stream_index = -1;

	std::vector<float> fifo;
	size_t fifo_pos = 0;
	bool demux_done = false;
	bool finished = false;
	bool first_frame = true;
	int consecutive_errors = 0;
	std::string error;

	bool live = false;
	double duration = -1.0;
	// Set by the first decoded frame, which may be on the reader thread
	std::atomic<double> position_base = 0.0;
	uint64_t frames_read = 0;

	std::string title;
	int64_t last_title_poll_us = 0;

	// ---- Read-ahead (network inputs) ----
	AVIOInterruptCB host_interrupt = { nullptr, nullptr };
	bool threaded = false;
	// The host's interrupt only applies while opening and filling up the
	// first time. After that the reader is only stopped by us.
	std::atomic<bool> opening = true;
	std::atomic<bool> stopping = false;
	std::thread reader;
	std::mutex ring_mutex;
	std::condition_variable ring_cv;
	std::vector<float> ring;
	size_t ring_head = 0;
	size_t ring_count = 0;
	// Samples to have decoded before playing starts
	size_t ring_target = 0;
	bool reader_done = false;
	bool title_changed = false;

	static int interrupt_callback(void *opaque);
	bool host_interrupted() const;
	bool decode_more();
	int read_direct(float *out, int frames);
	void append_converted(uint8_t **in, int in_samples);
	bool fetch_title(std::string &r_title);
	void start_reader();
	void stop_reader();
	void reader_main();
};

} // namespace songo

#endif // SONGO_SONG_H
