#include "song.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unistd.h>

namespace songo {

namespace {

const int CHANNELS = 2;
// Frames the read-ahead thread decodes at a time
const int READER_CHUNK_FRAMES = 1024;
// FFmpeg already retried the connection per the reconnect options, so this
// many failed reads in a row means the stream is gone
const int MAX_READ_ERRORS = 10;

std::string av_error_string(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

bool is_valid_utf8(const std::string &s) {
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = (unsigned char)s[i];
		int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
		if (extra < 0 || i + extra >= s.size()) {
			return false;
		}
		for (int k = 1; k <= extra; k++) {
			if ((((unsigned char)s[i + k]) >> 6) != 0x2) {
				return false;
			}
		}
		i += extra + 1;
	}
	return true;
}

// ICY titles are usually UTF-8, but plenty of older servers send Latin-1
std::string icy_to_utf8(const std::string &s) {
	if (is_valid_utf8(s)) {
		return s;
	}
	std::string out;
	for (unsigned char c : s) {
		if (c < 0x80) {
			out += (char)c;
		} else {
			out += (char)(0xC0 | (c >> 6));
			out += (char)(0x80 | (c & 0x3F));
		}
	}
	return out;
}

std::string trim(const std::string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

} // namespace

Song::~Song() {
	stop_reader();
	av_frame_free(&frame);
	av_packet_free(&packet);
	swr_free(&swr);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&format_ctx);
}

bool Song::open(const std::string &p_path, double seek_sec, const SongOptions &options, std::string &r_error) {
	path = p_path;
	sample_rate = options.sample_rate;

	format_ctx = avformat_alloc_context();
	if (!format_ctx) {
		r_error = "Out of memory";
		return false;
	}
	host_interrupt = options.interrupt;
	format_ctx->interrupt_callback = { interrupt_callback, this };

	// Protocol options; the ones that don't apply to a local file are ignored
	AVDictionary *dict = nullptr;
	av_dict_set_int(&dict, "rw_timeout", (int64_t)(options.network_timeout * 1000000.0), 0);
	av_dict_set(&dict, "reconnect", "1", 0);
	av_dict_set(&dict, "reconnect_streamed", "1", 0);
	av_dict_set(&dict, "reconnect_on_network_error", "1", 0);
	av_dict_set(&dict, "reconnect_delay_max", "5", 0);
	av_dict_set(&dict, "icy", "1", 0);
	if (!options.user_agent.empty()) {
		av_dict_set(&dict, "user_agent", options.user_agent.c_str(), 0);
	}

	int ret = avformat_open_input(&format_ctx, path.c_str(), nullptr, &dict);
	av_dict_free(&dict);
	if (ret < 0) {
		// avformat_open_input frees the context on failure
		format_ctx = nullptr;
		r_error = "Failed to open " + path + ": " + av_error_string(ret);
		return false;
	}
	if ((ret = avformat_find_stream_info(format_ctx, nullptr)) < 0) {
		r_error = "Failed to read stream info for " + path + ": " + av_error_string(ret);
		return false;
	}

	const AVCodec *codec = nullptr;
	stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if (stream_index < 0 || !codec) {
		r_error = "No audio stream in " + path;
		return false;
	}
	// Skips cover art, and stops HLS downloading renditions we don't play
	for (unsigned i = 0; i < format_ctx->nb_streams; i++) {
		if ((int)i != stream_index) {
			format_ctx->streams[i]->discard = AVDISCARD_ALL;
		}
	}

	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		r_error = "Out of memory";
		return false;
	}
	avcodec_parameters_to_context(codec_ctx, format_ctx->streams[stream_index]->codecpar);
	if ((ret = avcodec_open2(codec_ctx, codec, nullptr)) < 0) {
		r_error = "Failed to open decoder for " + path + ": " + av_error_string(ret);
		return false;
	}

	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, CHANNELS);
	ret = swr_alloc_set_opts2(&swr,
			&out_layout, AV_SAMPLE_FMT_FLT, sample_rate,
			&codec_ctx->ch_layout, codec_ctx->sample_fmt, codec_ctx->sample_rate,
			0, nullptr);
	if (ret < 0 || swr_init(swr) < 0) {
		r_error = "Failed to set up the resampler for " + path;
		return false;
	}

	// Live radio (Icecast/Shoutcast, live HLS) has no duration and can't seek
	live = format_ctx->duration == AV_NOPTS_VALUE;
	if (!live) {
		duration = (double)format_ctx->duration / AV_TIME_BASE;
	}

	packet = av_packet_alloc();
	frame = av_frame_alloc();
	if (!packet || !frame) {
		r_error = "Out of memory";
		return false;
	}
	if (seek_sec > 0.0 && !live) {
		seek(seek_sec);
	}

	bool network = path.find("://") != std::string::npos && path.rfind("file:", 0) != 0;
	if (network && options.read_ahead_sec > 0.0) {
		ring_target = std::max<size_t>((size_t)(options.read_ahead_sec * sample_rate) * CHANNELS, READER_CHUNK_FRAMES * CHANNELS);
		ring.resize(ring_target + READER_CHUNK_FRAMES * CHANNELS);
		threaded = true;
		start_reader();
		// Fill up before playing, interruptible by the engine like the open
		std::unique_lock<std::mutex> lock(ring_mutex);
		while (ring_count < ring_target && !reader_done) {
			ring_cv.wait_for(lock, std::chrono::milliseconds(20));
			if (host_interrupted()) {
				r_error = "Interrupted";
				return false;
			}
		}
		if (reader_done && ring_count == 0) {
			r_error = "Nothing came from " + path + (error.empty() ? "" : ": " + error);
			return false;
		}
	}
	opening = false;
	return true;
}

int Song::interrupt_callback(void *opaque) {
	Song *song = static_cast<Song *>(opaque);
	if (song->stopping) {
		return 1;
	}
	// Reads on the engine thread can always be cut short by the engine. The
	// reader thread's only while opening, after that nothing waits on it.
	bool engine_reads = !song->threaded || song->opening;
	return engine_reads && song->host_interrupted() ? 1 : 0;
}

bool Song::host_interrupted() const {
	return host_interrupt.callback && host_interrupt.callback(host_interrupt.opaque);
}

bool Song::is_finished() {
	if (!threaded) {
		return finished && fifo_pos >= fifo.size();
	}
	std::lock_guard<std::mutex> lock(ring_mutex);
	return reader_done && ring_count == 0;
}

int Song::read(float *out, int frames) {
	if (!threaded) {
		int filled = read_direct(out, frames);
		frames_read += (uint64_t)filled;
		return filled;
	}

	std::unique_lock<std::mutex> lock(ring_mutex);
	if (ring_count == 0 && !reader_done) {
		// Give a late chunk a moment before falling back on silence
		ring_cv.wait_for(lock, std::chrono::milliseconds(100), [this]() { return ring_count > 0 || reader_done; });
	}
	int n = std::min((int)(ring_count / CHANNELS), frames);
	for (int i = 0; i < n * CHANNELS; i++) {
		out[i] = ring[(ring_head + (size_t)i) % ring.size()];
	}
	ring_head = (ring_head + (size_t)n * CHANNELS) % ring.size();
	ring_count -= (size_t)n * CHANNELS;
	bool done = reader_done;
	lock.unlock();
	ring_cv.notify_all();
	frames_read += (uint64_t)n;

	if (n < frames && !done) {
		// The connection stalled: play silence and carry on once data comes
		std::fill(out + (size_t)n * CHANNELS, out + (size_t)frames * CHANNELS, 0.0f);
		return frames;
	}
	return n;
}

// Decodes straight into `out` on the calling thread
int Song::read_direct(float *out, int frames) {
	int filled = 0;
	while (filled < frames) {
		size_t available = (fifo.size() - fifo_pos) / CHANNELS;
		if (available == 0) {
			fifo.clear();
			fifo_pos = 0;
			if (finished || !decode_more()) {
				break;
			}
			continue;
		}
		int n = std::min((int)available, frames - filled);
		memcpy(out + (size_t)filled * CHANNELS, fifo.data() + fifo_pos, (size_t)n * CHANNELS * sizeof(float));
		fifo_pos += (size_t)n * CHANNELS;
		filled += n;
	}
	return filled;
}

bool Song::seek(double seconds) {
	if (live || !format_ctx) {
		return false;
	}
	seconds = std::max(0.0, seconds);
	// The reader owns the decoder while it runs
	stop_reader();
	{
		std::lock_guard<std::mutex> lock(ring_mutex);
		ring_head = 0;
		ring_count = 0;
		reader_done = false;
	}
	if (av_seek_frame(format_ctx, -1, (int64_t)(seconds * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) {
		// Carry on from where it was
		start_reader();
		return false;
	}
	avcodec_flush_buffers(codec_ctx);
	// Re-initialising drops whatever the resampler was holding
	swr_init(swr);
	fifo.clear();
	fifo_pos = 0;
	demux_done = false;
	finished = false;
	consecutive_errors = 0;
	first_frame = true;
	position_base = seconds;
	frames_read = 0;
	start_reader();
	return true;
}

// Decodes until at least one frame lands in the fifo. False once the song (or
// the decoder) is done, or when interrupted.
bool Song::decode_more() {
	while (true) {
		int ret = avcodec_receive_frame(codec_ctx, frame);
		if (ret == 0) {
			// A backwards seek lands a little before the target, so take the
			// position from the first frame rather than the request
			if (first_frame) {
				first_frame = false;
				int64_t pts = frame->best_effort_timestamp;
				if (pts != AV_NOPTS_VALUE && !live) {
					position_base = pts * av_q2d(format_ctx->streams[stream_index]->time_base);
					if (!threaded) {
						frames_read = 0;
					}
				}
			}
			append_converted(frame->extended_data, frame->nb_samples);
			av_frame_unref(frame);
			if (fifo_pos < fifo.size()) {
				return true;
			}
			continue;
		}
		if (ret == AVERROR_EOF) {
			// Whatever the resampler is still holding
			append_converted(nullptr, 0);
			finished = true;
			return fifo_pos < fifo.size();
		}
		if (ret != AVERROR(EAGAIN)) {
			error = "Decoding failed: " + av_error_string(ret);
			finished = true;
			return false;
		}

		if (demux_done) {
			// Shouldn't happen after a flush, but don't spin if it does
			finished = true;
			return false;
		}
		ret = av_read_frame(format_ctx, packet);
		if (ret == AVERROR_EXIT) {
			// The engine interrupted us. Not finished, it'll be thrown away.
			return false;
		}
		if (ret == AVERROR(EAGAIN)) {
			usleep(10000);
			continue;
		}
		if (ret < 0 && ret != AVERROR_EOF && !(format_ctx->pb && avio_feof(format_ctx->pb))) {
			if (++consecutive_errors < MAX_READ_ERRORS) {
				usleep(100000);
				continue;
			}
			error = "Stream read failed: " + av_error_string(ret);
		}
		if (ret < 0) {
			demux_done = true;
			avcodec_send_packet(codec_ctx, nullptr);
			continue;
		}
		consecutive_errors = 0;
		if (packet->stream_index == stream_index) {
			avcodec_send_packet(codec_ctx, packet);
		}
		av_packet_unref(packet);
	}
}

void Song::append_converted(uint8_t **in, int in_samples) {
	int max_out = swr_get_out_samples(swr, in_samples);
	if (max_out <= 0) {
		return;
	}
	size_t start = fifo.size();
	fifo.resize(start + (size_t)max_out * CHANNELS);
	uint8_t *out = (uint8_t *)(fifo.data() + start);
	int got = swr_convert(swr, &out, max_out, (const uint8_t **)in, in_samples);
	fifo.resize(start + (size_t)std::max(got, 0) * CHANNELS);
}

bool Song::poll_title(std::string &r_title) {
	if (!threaded) {
		return fetch_title(r_title);
	}
	std::lock_guard<std::mutex> lock(ring_mutex);
	if (!title_changed) {
		return false;
	}
	title_changed = false;
	r_title = title;
	return true;
}

// Reads the Icecast/Shoutcast title, on the thread doing the reading
bool Song::fetch_title(std::string &r_title) {
	if (!live || !format_ctx || !format_ctx->pb) {
		return false;
	}
	int64_t now = av_gettime_relative();
	if (now - last_title_poll_us < 500000) {
		return false;
	}
	last_title_poll_us = now;

	uint8_t *meta_packet = nullptr;
	if (av_opt_get(format_ctx->pb, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &meta_packet) < 0 || !meta_packet) {
		return false;
	}
	std::string meta((const char *)meta_packet);
	av_free(meta_packet);

	// StreamTitle='Artist - Title';StreamUrl='...';
	static const std::string key = "StreamTitle='";
	size_t start = meta.find(key);
	if (start == std::string::npos) {
		return false;
	}
	start += key.size();
	size_t end = meta.find("';", start);
	if (end == std::string::npos) {
		end = meta.rfind('\'');
		if (end == std::string::npos || end < start) {
			end = meta.size();
		}
	}

	std::string new_title = trim(icy_to_utf8(meta.substr(start, end - start)));
	std::lock_guard<std::mutex> lock(ring_mutex);
	if (new_title == title) {
		return false;
	}
	title = new_title;
	r_title = title;
	return true;
}

void Song::start_reader() {
	if (!threaded) {
		return;
	}
	stopping = false;
	reader = std::thread(&Song::reader_main, this);
}

void Song::stop_reader() {
	if (!reader.joinable()) {
		return;
	}
	stopping = true;
	ring_cv.notify_all();
	reader.join();
	stopping = false;
}

// Keeps the ring topped up until the song ends or we're stopped
void Song::reader_main() {
	std::vector<float> chunk((size_t)READER_CHUNK_FRAMES * CHANNELS);
	while (!stopping) {
		{
			std::unique_lock<std::mutex> lock(ring_mutex);
			ring_cv.wait(lock, [this]() { return stopping || ring_count + (size_t)READER_CHUNK_FRAMES * CHANNELS <= ring.size(); });
		}
		if (stopping) {
			break;
		}
		std::string new_title;
		bool got_title = fetch_title(new_title);
		int n = read_direct(chunk.data(), READER_CHUNK_FRAMES);
		bool ended = n < READER_CHUNK_FRAMES && finished && fifo_pos >= fifo.size();
		{
			std::lock_guard<std::mutex> lock(ring_mutex);
			for (int i = 0; i < n * CHANNELS; i++) {
				ring[(ring_head + ring_count + (size_t)i) % ring.size()] = chunk[(size_t)i];
			}
			ring_count += (size_t)n * CHANNELS;
			if (got_title) {
				title_changed = true;
			}
			if (ended) {
				reader_done = true;
			}
		}
		ring_cv.notify_all();
		if (ended) {
			break;
		}
		if (n < READER_CHUNK_FRAMES && !stopping) {
			// Interrupted while opening, the song is about to be thrown away
			std::lock_guard<std::mutex> lock(ring_mutex);
			reader_done = true;
			ring_cv.notify_all();
			break;
		}
	}
}

} // namespace songo
