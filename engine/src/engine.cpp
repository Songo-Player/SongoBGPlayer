#include "songo_engine.h"
#include "songo_shared.h"

#include "godot_eq.h"
#include "output.h"
#include "song.h"
#include "spectrum_analyzer.h"

extern "C" {
#include <libavformat/avformat.h>
}

#ifdef _WIN32
// No daemon to share memory with, nor eventfd: the shared block is plain
// memory and subscriptions have no fd (songo_subscription_fd returns -1)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace songo;

namespace {

const int DEFAULT_SAMPLE_RATE = 44100;
const int DEFAULT_LATENCY_MS = 200;
const int CHANNELS = 2;
// Frames mixed and written per pass, ~23ms
const int CHUNK_FRAMES = 1024;
// Previous within this far into a song goes back a song instead of restarting
const double RESTART_THRESHOLD_SEC = 3.0;
// The spectrum and waveform stop being worked out this long after a reader
// last looked at them
const int64_t READER_TIMEOUT_NS = 2000000000;

struct Command {
	enum Type {
		SET_QUEUE,
		REPLACE_QUEUE,
		PLAY_INDEX,
		RESUME,
		PAUSE,
		STOP,
		SEEK,
		NEXT,
		PREVIOUS,
		VOLUME,
		EQ,
		BLEND,
		REPEAT_ONE,
		LOOP_QUEUE,
		NETWORK_TIMEOUT,
		USER_AGENT,
		SKIP_FAILED,
		LATENCY,
		STREAM_BUFFER,
	};

	Type type;
	std::vector<std::string> paths;
	int index = 0;
	double value = 0.0;
	bool flag = false;
	std::string text;
	std::vector<float> gains;

	// Commands that make whatever song is being opened or read pointless,
	// so a dead stream can't hold them up
	bool preempts() const {
		return type == SET_QUEUE || type == PLAY_INDEX || type == STOP || type == NEXT || type == PREVIOUS;
	}
};

struct Event {
	int type = 0;
	int index = -1;
	int flags = 0;
	double value = 0.0;
	std::string text;
	std::vector<std::string> paths;
};

std::vector<std::string> copy_paths(const char *const *paths, int count) {
	std::vector<std::string> out;
	for (int i = 0; paths && i < count; i++) {
		out.push_back(paths[i] ? paths[i] : "");
	}
	return out;
}

// Scales samples by a gain ramping from `from` to `to` across the buffer, so a
// volume change doesn't click
void apply_volume(float *samples, int frames, float from, float to) {
	if (frames <= 0 || (from == 1.0f && to == 1.0f)) {
		return;
	}
	for (int f = 0; f < frames; f++) {
		float gain = from + (to - from) * (float)(f + 1) / (float)frames;
		for (int c = 0; c < CHANNELS; c++) {
			samples[f * CHANNELS + c] *= gain;
		}
	}
}

} // namespace

struct SongoSubscription {
	SongoEngine *engine = nullptr;
	std::mutex mutex;
	std::deque<Event> events;
	int fd = -1;
	// Backs the pointers in the SongoEvent handed out last
	Event current;
	std::vector<const char *> current_paths;

	void push(const Event &event) {
		std::lock_guard<std::mutex> lock(mutex);
		events.push_back(event);
#ifndef _WIN32
		if (fd >= 0) {
			uint64_t one = 1;
			ssize_t ignored = write(fd, &one, sizeof(one));
			(void)ignored;
		}
#endif
	}
};

struct SongoEngine {
	explicit SongoEngine(const SongoEngineConfig *config);
	~SongoEngine();

	bool init();
	void push(Command command);
	SongoSubscription *subscribe();
	void unsubscribe(SongoSubscription *subscription);

	Shared *shared = nullptr;
	size_t shared_size = 0;
	int shared_fd = -1;

private:
	std::string backend;
	std::string device;
	int sample_rate;
	int latency_ms;

	std::thread thread;
	std::mutex command_mutex;
	std::condition_variable command_cv;
	std::vector<Command> commands;
	std::atomic<bool> quitting = false;
	std::atomic<bool> interrupt = false;

	std::mutex subscriber_mutex;
	std::vector<SongoSubscription *> subscribers;
	// Replayed to new subscribers
	Event last_queue;
	Event last_track;
	bool has_track = false;
	Event last_title;
	Event last_state;

	// ---- Engine thread only from here ----

	std::unique_ptr<Output> output;
	std::vector<std::string> queue;
	int index = -1;
	SongoState state = SONGO_STATE_STOPPED;

	std::unique_ptr<Song> current;
	std::unique_ptr<Song> incoming;
	int incoming_index = -1;
	bool blend_tried = false;
	int64_t blend_frames = 0;
	int64_t blend_done = 0;
	uint64_t track_serial = 0;
	std::string title;

	// The song coming out of the speaker, which is what events and the
	// status report. When one song runs into the next it trails the decoder
	// by the output latency, and the outgoing song is still heard until then.
	struct Heard {
		int index = -1;
		double duration = -1.0;
		int flags = 0;
		std::string path;
	};
	Heard heard;
	bool heard_pending = false;
	Heard next_heard;
	int64_t next_heard_at_ns = 0;
	double outgoing_position = 0.0;
	int64_t outgoing_time_ns = 0;

	// Set by commands, carried out by the loop
	struct {
		bool active = false;
		int index = 0;
		double position = 0.0;
	} pending;
	// Where a song cued with autoplay off starts
	double cued_position = 0.0;
	bool pause_after_load = false;

	float volume = 1.0f;
	// What the last chunk ended on, so a change ramps from there
	float applied_volume = 1.0f;
	bool eq_enabled = false;
	float eq_gains_db[EQ_BANDS] = {};
	std::unique_ptr<GodotEQ10> eq;
	double blend_sec = 0.0;
	bool repeat_one = false;
	bool loop_queue = false;
	bool skip_failed = true;
	// Seconds network streams decode ahead (Song's read-ahead)
	double stream_buffer_sec = 1.0;
	double network_timeout = 10.0;
	std::string user_agent;

	SpectrumAnalyzer analyzer;
	std::vector<float> a, b, mix;

	enum OpenResult {
		OPENED,
		CANCELLED,
		// Ran off the end of the queue with looping off
		REACHED_END,
		NOTHING_PLAYABLE,
		// It wouldn't open and skip_failed is off
		FAILED,
	};

	static int interrupt_callback(void *opaque);
	bool cancelled() const { return quitting || interrupt; }

	void run();
	void apply(Command &command);
	void start_pending();
	void mix_chunk();
	std::unique_ptr<Song> open_from(int &r_index, double seek_sec, OpenResult &r_result, bool quiet = false);
	bool ensure_output();
	void flush_output();
	void stop_playback();
	void finish_queue(bool drain, bool reached_end);
	int next_index(int from) const;
	bool can_blend() const;
	void cancel_blend();
	void track_started(const Song *outgoing);
	void set_latency(int ms);
	void hear(Heard info);
	void hear_pending_now();
	void set_title(const std::string &new_title);
	void set_state(SongoState new_state);
	void publish_status();
	void publish_audio(const float *frames, int count, int64_t first_audible_ns);
	void emit(Event event);
	void emit_queue();
	void emit_error(int at, const std::string &message);
};

SongoEngine::SongoEngine(const SongoEngineConfig *config) {
	backend = config && config->backend ? config->backend : "";
	device = config && config->device ? config->device : "";
	sample_rate = config && config->sample_rate > 0 ? config->sample_rate : DEFAULT_SAMPLE_RATE;
	latency_ms = config && config->latency_ms > 0 ? config->latency_ms : DEFAULT_LATENCY_MS;
	a.resize(CHUNK_FRAMES * CHANNELS);
	b.resize(CHUNK_FRAMES * CHANNELS);
	mix.resize(CHUNK_FRAMES * CHANNELS);
	last_state.type = SONGO_EVENT_STATE;
	last_state.index = SONGO_STATE_STOPPED;
	last_queue.type = SONGO_EVENT_QUEUE;
	last_title.type = SONGO_EVENT_TITLE;
}

bool SongoEngine::init() {
	output = create_output(backend, device, sample_rate, latency_ms);
	if (!output) {
		fprintf(stderr, "Unknown audio backend \"%s\"\n", backend.c_str());
		return false;
	}

#ifdef _WIN32
	shared_size = sizeof(Shared);
	// Zeroed, like a fresh mapping
	void *memory = VirtualAlloc(nullptr, shared_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!memory) {
		fprintf(stderr, "Can't allocate the engine's shared memory\n");
		return false;
	}
#else
	// A memfd when we can get one, so the daemon can hand it to Songo
	long page = sysconf(_SC_PAGESIZE);
	shared_size = (sizeof(Shared) + (size_t)page - 1) / (size_t)page * (size_t)page;
	void *memory = MAP_FAILED;
#ifdef SYS_memfd_create
	shared_fd = (int)syscall(SYS_memfd_create, "songo_engine", 1u /* MFD_CLOEXEC */);
	if (shared_fd >= 0) {
		if (ftruncate(shared_fd, (off_t)shared_size) == 0) {
			memory = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, shared_fd, 0);
		}
		if (memory == MAP_FAILED) {
			close(shared_fd);
			shared_fd = -1;
		}
	}
#endif
	if (memory == MAP_FAILED) {
		memory = mmap(nullptr, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	}
	if (memory == MAP_FAILED) {
		fprintf(stderr, "Can't allocate the engine's shared memory\n");
		return false;
	}
#endif

	// Fresh mappings are zeroed, which is a valid starting state for every field
	shared = static_cast<Shared *>(memory);
	shared->version = SHARED_VERSION;
	shared->size = sizeof(Shared);
	shared->sample_rate = (uint32_t)sample_rate;
	shared->fft_size = FFT_SIZE;
	shared->spectrum_frames = SPECTRUM_FRAMES;
	shared->wave_frames = WAVE_FRAMES;
	shared->status.index = -1;
	shared->status.volume = 1.0f;
	std::atomic_thread_fence(std::memory_order_release);
	shared->magic = SHARED_MAGIC;

	avformat_network_init();
	thread = std::thread(&SongoEngine::run, this);
	return true;
}

SongoEngine::~SongoEngine() {
	if (thread.joinable()) {
		{
			std::lock_guard<std::mutex> lock(command_mutex);
			quitting = true;
			interrupt = true;
		}
		command_cv.notify_all();
		thread.join();
		avformat_network_deinit();
	}
	current.reset();
	incoming.reset();
	if (output) {
		output->flush();
		output->close();
	}
	{
		std::lock_guard<std::mutex> lock(subscriber_mutex);
		for (SongoSubscription *subscription : subscribers) {
			subscription->engine = nullptr;
		}
		subscribers.clear();
	}
#ifdef _WIN32
	if (shared) {
		VirtualFree(shared, 0, MEM_RELEASE);
	}
#else
	if (shared) {
		munmap(shared, shared_size);
	}
	if (shared_fd >= 0) {
		close(shared_fd);
	}
#endif
}

void SongoEngine::push(Command command) {
	{
		std::lock_guard<std::mutex> lock(command_mutex);
		if (command.preempts()) {
			interrupt = true;
		}
		commands.push_back(std::move(command));
	}
	command_cv.notify_all();
}

SongoSubscription *SongoEngine::subscribe() {
	SongoSubscription *subscription = new SongoSubscription;
	subscription->engine = this;
#ifndef _WIN32
	subscription->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#endif
	std::lock_guard<std::mutex> lock(subscriber_mutex);
	// Catch the new subscriber up before it gets anything newer
	subscription->push(last_queue);
	if (has_track) {
		subscription->push(last_track);
	}
	if (!last_title.text.empty()) {
		subscription->push(last_title);
	}
	subscription->push(last_state);
	subscribers.push_back(subscription);
	return subscription;
}

void SongoEngine::unsubscribe(SongoSubscription *subscription) {
	std::lock_guard<std::mutex> lock(subscriber_mutex);
	subscribers.erase(std::remove(subscribers.begin(), subscribers.end(), subscription), subscribers.end());
}

void SongoEngine::emit(Event event) {
	std::lock_guard<std::mutex> lock(subscriber_mutex);
	switch (event.type) {
		case SONGO_EVENT_QUEUE:
			last_queue = event;
			break;
		case SONGO_EVENT_TRACK:
			last_track = event;
			has_track = true;
			break;
		case SONGO_EVENT_TITLE:
			last_title = event;
			break;
		case SONGO_EVENT_STATE:
			last_state = event;
			break;
	}
	for (SongoSubscription *subscription : subscribers) {
		subscription->push(event);
	}
}

void SongoEngine::emit_queue() {
	Event event;
	event.type = SONGO_EVENT_QUEUE;
	event.index = index;
	event.paths = queue;
	emit(std::move(event));
}

void SongoEngine::emit_error(int at, const std::string &message) {
	fprintf(stderr, "%s\n", message.c_str());
	Event event;
	event.type = SONGO_EVENT_ERROR;
	event.index = at;
	event.text = message;
	emit(std::move(event));
}

int SongoEngine::interrupt_callback(void *opaque) {
	return static_cast<SongoEngine *>(opaque)->cancelled() ? 1 : 0;
}

// ---- Engine thread ----

void SongoEngine::run() {
	publish_status();
	while (!quitting) {
		std::vector<Command> batch;
		{
			std::unique_lock<std::mutex> lock(command_mutex);
			bool busy = pending.active || state == SONGO_STATE_PLAYING;
			if (commands.empty() && !busy) {
				command_cv.wait_for(lock, std::chrono::milliseconds(250));
			}
			batch.swap(commands);
			interrupt = false;
		}
		if (quitting) {
			break;
		}
		for (Command &command : batch) {
			apply(command);
		}
		if (!batch.empty()) {
			publish_status();
		}
		if (pending.active) {
			start_pending();
		} else if (state == SONGO_STATE_PLAYING && current) {
			mix_chunk();
		}
	}
}

void SongoEngine::apply(Command &command) {
	int count = (int)queue.size();
	switch (command.type) {
		case Command::SET_QUEUE: {
			stop_playback();
			queue = std::move(command.paths);
			count = (int)queue.size();
			index = count > 0 ? std::clamp(command.index, 0, count - 1) : -1;
			pause_after_load = false;
			if (command.flag && index >= 0) {
				pending = { true, index, command.value };
			} else {
				cued_position = command.value;
				set_state(SONGO_STATE_STOPPED);
				flush_output();
				if (output) {
					output->close();
				}
			}
			emit_queue();
		} break;

		case Command::REPLACE_QUEUE: {
			queue = std::move(command.paths);
			count = (int)queue.size();
			if (count == 0) {
				stop_playback();
				index = -1;
				set_state(SONGO_STATE_STOPPED);
			} else {
				index = std::clamp(command.index, 0, count - 1);
				hear_pending_now();
				heard.index = index;
				// The song fading in was picked from the old queue
				cancel_blend();
			}
			emit_queue();
		} break;

		case Command::PLAY_INDEX:
			if (command.index >= 0 && command.index < count) {
				pending = { true, command.index, command.value };
				pause_after_load = false;
			}
			break;

		case Command::RESUME:
			pause_after_load = false;
			if (state == SONGO_STATE_PAUSED && current) {
				if (current->is_live()) {
					// The broadcast carried on (or dropped us) while paused,
					// so rejoin it live rather than play stale audio
					pending = { true, index, 0.0 };
				} else {
					output->set_paused(false);
					set_state(SONGO_STATE_PLAYING);
				}
			} else if (state == SONGO_STATE_STOPPED && !pending.active && count > 0) {
				pending = { true, std::clamp(index, 0, count - 1), cued_position };
				cued_position = 0.0;
			}
			break;

		case Command::PAUSE:
			if (state == SONGO_STATE_PLAYING) {
				hear_pending_now();
				output->set_paused(true);
				// What's queued in the spectrum was stamped for a speaker
				// that's now stopped
				shared->generation.fetch_add(1, std::memory_order_release);
				analyzer.reset();
				set_state(SONGO_STATE_PAUSED);
			} else if (state == SONGO_STATE_LOADING || pending.active) {
				pause_after_load = true;
			}
			break;

		case Command::STOP:
			pending.active = false;
			pause_after_load = false;
			cued_position = 0.0;
			stop_playback();
			set_state(SONGO_STATE_STOPPED);
			if (output) {
				output->close();
			}
			break;

		case Command::SEEK:
			if (current && current->is_seekable()) {
				// The end of the old song is thrown away with the rest
				hear_pending_now();
				cancel_blend();
				flush_output();
				current->seek(command.value);
				blend_tried = false;
				publish_status();
			} else if (state == SONGO_STATE_STOPPED && !pending.active) {
				cued_position = std::max(0.0, command.value);
			}
			break;

		case Command::NEXT:
		case Command::PREVIOUS: {
			if (count == 0) {
				break;
			}
			// Mid-blend the incoming song is the one being heard, so move from it
			int from = incoming ? incoming_index : std::clamp(index, 0, count - 1);
			int target;
			if (repeat_one) {
				// Like Songo's own skips, which stay on a repeating song
				target = from;
			} else if (command.type == Command::NEXT) {
				target = from + 1 < count ? from + 1 : loop_queue ? 0 : -1;
				if (target < 0) {
					finish_queue(false, true);
					break;
				}
			} else {
				Song *heard = incoming ? incoming.get() : current.get();
				if (heard && heard->get_position() >= RESTART_THRESHOLD_SEC) {
					target = from;
				} else {
					target = from > 0 ? from - 1 : loop_queue ? count - 1 : 0;
				}
			}
			pending = { true, target, 0.0 };
			pause_after_load = false;
		} break;

		case Command::VOLUME:
			volume = std::clamp((float)command.value, 0.0f, 1.0f);
			publish_status();
			break;

		case Command::EQ:
			eq_enabled = command.flag;
			for (int band = 0; band < EQ_BANDS; band++) {
				eq_gains_db[band] = band < (int)command.gains.size() ? command.gains[band] : 0.0f;
			}
			if (eq_enabled) {
				if (!eq) {
					eq = std::make_unique<GodotEQ10>(sample_rate);
				}
				for (int band = 0; band < EQ_BANDS; band++) {
					eq->set_band_gain_db(band, eq_gains_db[band]);
				}
			} else {
				// Skipped entirely when off, like Songo bypassing the effect
				eq.reset();
			}
			publish_status();
			break;

		case Command::BLEND:
			blend_sec = std::max(0.0, command.value);
			publish_status();
			break;

		case Command::REPEAT_ONE:
			repeat_one = command.flag;
			publish_status();
			break;

		case Command::LOOP_QUEUE:
			loop_queue = command.flag;
			publish_status();
			break;

		case Command::NETWORK_TIMEOUT:
			network_timeout = std::max(1.0, command.value);
			break;

		case Command::USER_AGENT:
			user_agent = command.text;
			break;

		case Command::SKIP_FAILED:
			skip_failed = command.flag;
			break;

		case Command::LATENCY:
			set_latency(std::clamp(command.index, 20, 2000));
			break;

		case Command::STREAM_BUFFER:
			// Streams opened from here on
			stream_buffer_sec = std::clamp(command.value, 0.0, 30.0);
			break;
	}
}

void SongoEngine::start_pending() {
	int target = pending.index;
	double position = pending.position;
	pending.active = false;

	stop_playback();
	set_state(SONGO_STATE_LOADING);

	if (!ensure_output()) {
		if (cancelled()) {
			pending = { true, target, position };
		} else {
			set_state(SONGO_STATE_STOPPED);
		}
		return;
	}

	OpenResult result;
	std::unique_ptr<Song> song = open_from(target, position, result);
	if (result == CANCELLED) {
		// Whatever cancelled it decides what happens next. If it turns out to
		// change nothing (say a next on an empty queue), carry on with this.
		pending = { true, target, position };
		return;
	}
	if (!song) {
		if (result == NOTHING_PLAYABLE) {
			emit_error(-1, "Nothing in the queue is playable");
		}
		finish_queue(false, result == REACHED_END);
		return;
	}

	current = std::move(song);
	index = target;
	track_started(nullptr);
	if (pause_after_load) {
		pause_after_load = false;
		output->set_paused(true);
		set_state(SONGO_STATE_PAUSED);
	} else {
		set_state(SONGO_STATE_PLAYING);
	}
}

// Opens the song at `r_index`, moving on past ones that won't open unless
// skip_failed is off. `quiet` leaves failures unreported (a blend trying the
// next song early, which reports it once it's actually due).
std::unique_ptr<Song> SongoEngine::open_from(int &r_index, double seek_sec, OpenResult &r_result, bool quiet) {
	int count = (int)queue.size();
	SongOptions options;
	options.sample_rate = sample_rate;
	options.network_timeout = network_timeout;
	options.user_agent = user_agent;
	options.read_ahead_sec = stream_buffer_sec;
	options.interrupt = { interrupt_callback, this };

	for (int tries = 0; tries < count; tries++) {
		if (cancelled()) {
			r_result = CANCELLED;
			return nullptr;
		}
		auto song = std::make_unique<Song>();
		std::string error;
		if (song->open(queue[r_index], seek_sec, options, error)) {
			fprintf(stderr, "Playing %s\n", queue[r_index].c_str());
			r_result = OPENED;
			return song;
		}
		if (cancelled()) {
			// It failed because we interrupted it
			r_result = CANCELLED;
			return nullptr;
		}
		if (!quiet) {
			emit_error(r_index, error);
		}
		if (!skip_failed) {
			r_result = FAILED;
			return nullptr;
		}
		seek_sec = 0.0;
		// Even on repeat-one, a song that won't open is skipped
		if (r_index + 1 < count) {
			r_index++;
		} else if (loop_queue) {
			r_index = 0;
		} else {
			r_result = REACHED_END;
			return nullptr;
		}
	}
	r_result = NOTHING_PLAYABLE;
	return nullptr;
}

bool SongoEngine::ensure_output() {
	if (output->is_open()) {
		return true;
	}
	std::string error;
	if (output->open([this]() { return cancelled(); }, error)) {
		analyzer.reset();
		return true;
	}
	if (!cancelled()) {
		emit_error(-1, error);
	}
	return false;
}

void SongoEngine::flush_output() {
	if (output && output->is_open()) {
		output->flush();
	}
	shared->generation.fetch_add(1, std::memory_order_release);
	analyzer.reset();
}

// Drops the songs and what's queued of them. Leaves the state to the caller.
void SongoEngine::stop_playback() {
	current.reset();
	incoming.reset();
	heard_pending = false;
	incoming_index = -1;
	blend_tried = false;
	flush_output();
	set_title("");
}

// Playback has nowhere to go: optionally let what's queued play out, then
// let go of the device
void SongoEngine::finish_queue(bool drain, bool reached_end) {
	if (drain && output->is_open() && state == SONGO_STATE_PLAYING) {
		output->drain();
	}
	stop_playback();
	output->close();
	set_state(SONGO_STATE_STOPPED);
	if (reached_end) {
		Event event;
		event.type = SONGO_EVENT_QUEUE_END;
		emit(std::move(event));
	}
}

// Where playback goes after `from` ends on its own. -1 at the end of the queue
// with looping off.
int SongoEngine::next_index(int from) const {
	if (repeat_one) {
		return from;
	}
	if (from + 1 < (int)queue.size()) {
		return from + 1;
	}
	return loop_queue && !queue.empty() ? 0 : -1;
}

bool SongoEngine::can_blend() const {
	// Songo doesn't blend a lone song into itself unless it's repeating
	return blend_sec > 0.0 && (queue.size() > 1 || repeat_one) && next_index(index) >= 0;
}

void SongoEngine::cancel_blend() {
	incoming.reset();
	incoming_index = -1;
	blend_tried = false;
}

// The output's buffer size can only be set as it opens, so a change reopens
// it. What's queued is thrown away and the song picks up from where the
// speaker was, so it's only a blip.
void SongoEngine::set_latency(int ms) {
	if (ms == latency_ms) {
		return;
	}
	latency_ms = ms;
	bool was_open = output && output->is_open();
	double resume_at = -1.0;
	if (was_open && current && current->is_seekable()) {
		resume_at = std::max(0.0, current->get_position() - (double)output->delay_frames() / sample_rate);
	}
	hear_pending_now();
	cancel_blend();
	flush_output();
	if (output) {
		output->close();
	}
	std::unique_ptr<Output> replacement = create_output(backend, device, sample_rate, latency_ms);
	if (replacement) {
		output = std::move(replacement);
	}
	if (!was_open) {
		return;
	}
	if (!ensure_output()) {
		stop_playback();
		set_state(SONGO_STATE_STOPPED);
		return;
	}
	if (state == SONGO_STATE_PAUSED) {
		output->set_paused(true);
	}
	if (resume_at >= 0.0) {
		current->seek(resume_at);
	}
}

// `current` just took over from `outgoing` (null when nothing was playing)
void SongoEngine::track_started(const Song *outgoing) {
	set_title("");
	applied_volume = volume;
	Heard info;
	info.index = index;
	info.duration = current->get_duration();
	info.flags = (current->is_live() ? SONGO_TRACK_LIVE : 0) | (current->is_seekable() ? SONGO_TRACK_SEEKABLE : 0);
	info.path = current->get_path();

	int64_t delay = output->is_open() ? output->delay_frames() : 0;
	if (outgoing && delay > 0 && state == SONGO_STATE_PLAYING) {
		// The end of the outgoing song is still queued, so it's announced once
		// that's played out
		int64_t now = monotonic_ns();
		if (!heard_pending) {
			outgoing_position = std::max(0.0, outgoing->get_position() - (double)delay / sample_rate);
			outgoing_time_ns = now;
		}
		next_heard = std::move(info);
		next_heard_at_ns = now + delay * 1000000000 / sample_rate;
		heard_pending = true;
	} else {
		heard_pending = false;
		hear(std::move(info));
	}
	publish_status();
}

void SongoEngine::hear(Heard info) {
	heard = std::move(info);
	track_serial++;
	Event event;
	event.type = SONGO_EVENT_TRACK;
	event.index = heard.index;
	event.value = heard.duration;
	event.flags = heard.flags;
	event.text = heard.path;
	emit(std::move(event));
}

void SongoEngine::hear_pending_now() {
	if (heard_pending) {
		heard_pending = false;
		hear(std::move(next_heard));
	}
}

void SongoEngine::set_title(const std::string &new_title) {
	if (new_title == title) {
		return;
	}
	title = new_title;
	Event event;
	event.type = SONGO_EVENT_TITLE;
	event.text = title;
	emit(std::move(event));
}

void SongoEngine::set_state(SongoState new_state) {
	if (new_state == state) {
		return;
	}
	state = new_state;
	Event event;
	event.type = SONGO_EVENT_STATE;
	event.index = state;
	emit(std::move(event));
	publish_status();
}

// Mixes the outgoing and incoming songs with linear gain ramps during a
// blend, matching Songo's volume tweens, then applies the music volume and
// Songo's EQ. Mixing is in float from -1 to 1 like Godot's audio frames, and
// only the output converts it to the device's format.
void SongoEngine::mix_chunk() {
	// Same trigger as Songo: within the blend time of the end, capped at half
	// the song so short songs aren't mostly crossfade
	double duration = current->get_duration();
	if (can_blend() && !incoming && !blend_tried && duration > 0.0) {
		double remaining = duration - current->get_position();
		if (remaining <= std::min(blend_sec, duration * 0.5)) {
			blend_tried = true;
			incoming_index = next_index(index);
			OpenResult result;
			incoming = open_from(incoming_index, 0.0, result, !skip_failed);
			if (result == CANCELLED) {
				blend_tried = false;
				return;
			}
			if (incoming) {
				blend_frames = std::max<int64_t>(1, (int64_t)(std::min(blend_sec, std::max(remaining, 0.01)) * sample_rate));
				blend_done = 0;
			}
		}
	}

	int got_a = current->read(a.data(), CHUNK_FRAMES);
	int frames = got_a;
	if (incoming) {
		int got_b = incoming->read(b.data(), CHUNK_FRAMES);
		std::fill(a.begin() + got_a * CHANNELS, a.end(), 0.0f);
		std::fill(b.begin() + got_b * CHANNELS, b.end(), 0.0f);
		frames = std::max(got_a, got_b);
		for (int f = 0; f < frames; f++) {
			float t = std::min(1.0f, (float)(blend_done + f) / (float)blend_frames);
			for (int c = 0; c < CHANNELS; c++) {
				int i = f * CHANNELS + c;
				mix[i] = a[i] * (1.0f - t) + b[i] * t;
			}
		}
		blend_done += frames;
	} else {
		std::copy(a.begin(), a.begin() + got_a * CHANNELS, mix.begin());
	}
	if (interrupt) {
		// A command is about to replace all of this
		return;
	}

	apply_volume(mix.data(), frames, applied_volume, volume);
	applied_volume = volume;

	// Analysed here, after the volume and before the EQ, which is where
	// Songo's spectrum analyzer sits on its Visualizer bus
	if (frames > 0) {
		int64_t now = monotonic_ns();
		int64_t first_audible = now + output->delay_frames() * 1000000000 / sample_rate;
		publish_audio(mix.data(), frames, first_audible);
	}

	if (eq) {
		eq->process(mix.data(), frames);
	}
	output->write(mix.data(), frames);

	std::string new_title;
	if (current->poll_title(new_title)) {
		set_title(new_title);
	}

	if (incoming) {
		// The incoming song takes over once the fade is done or the old one runs out
		if (blend_done >= blend_frames || current->is_finished()) {
			std::unique_ptr<Song> outgoing = std::move(current);
			current = std::move(incoming);
			index = incoming_index;
			incoming_index = -1;
			blend_tried = false;
			track_started(outgoing.get());
		}
		publish_status();
		return;
	}

	if (got_a < CHUNK_FRAMES && current->is_finished()) {
		if (!current->get_error().empty()) {
			emit_error(index, current->get_error() + " (" + current->get_path() + ")");
			if (!skip_failed) {
				// The host decides what to do about a stream that died
				finish_queue(false, false);
				return;
			}
		}
		int target = next_index(index);
		if (target < 0) {
			finish_queue(true, true);
			return;
		}
		OpenResult result;
		std::unique_ptr<Song> song = open_from(target, 0.0, result);
		if (result == CANCELLED) {
			// Let the command take it from here, as if it came mid song
			current.reset();
			pending = { true, target, 0.0 };
			return;
		}
		if (!song) {
			if (result == NOTHING_PLAYABLE) {
				emit_error(-1, "Nothing in the queue is playable");
			}
			finish_queue(true, result == REACHED_END);
			return;
		}
		std::unique_ptr<Song> outgoing = std::move(current);
		current = std::move(song);
		index = target;
		blend_tried = false;
		track_started(outgoing.get());
	}
	publish_status();
}

void SongoEngine::publish_status() {
	int64_t now = monotonic_ns();
	if (heard_pending && now >= next_heard_at_ns) {
		hear_pending_now();
	}

	Status status = {};
	status.state = state;
	status.index = current ? heard.index : index;
	status.queue_length = (int32_t)queue.size();
	status.track_serial = track_serial;
	status.volume = volume;
	status.blend_seconds = blend_sec;
	for (int band = 0; band < EQ_BANDS; band++) {
		status.eq_gains_db[band] = eq_gains_db[band];
	}
	status.flags = (repeat_one ? STATUS_REPEAT_ONE : 0) | (loop_queue ? STATUS_LOOP_QUEUE : 0) |
			(eq_enabled ? STATUS_EQ_ENABLED : 0);
	status.duration = -1.0;
	status.position_time_ns = now;
	if (current) {
		status.flags |= (heard.flags & SONGO_TRACK_LIVE ? STATUS_LIVE : 0) | (heard.flags & SONGO_TRACK_SEEKABLE ? STATUS_SEEKABLE : 0);
		status.duration = heard.duration;
		if (heard_pending) {
			status.position = outgoing_position + (double)(now - outgoing_time_ns) / 1e9;
		} else {
			// What's queued in the output hasn't been heard yet
			double queued = output && output->is_open() ? (double)output->delay_frames() / sample_rate : 0.0;
			status.position = std::max(0.0, current->get_position() - queued);
		}
	} else if (state == SONGO_STATE_STOPPED) {
		status.position = cued_position;
	}

	seq_write_begin(shared->status_seq);
	shared->status = status;
	seq_write_end(shared->status_seq);
}

void SongoEngine::publish_audio(const float *frames, int count, int64_t first_audible_ns) {
	if (first_audible_ns - shared->reader_heartbeat_ns.load(std::memory_order_relaxed) > READER_TIMEOUT_NS + (int64_t)latency_ms * 1000000) {
		// Nobody's looking, don't bother
		analyzer.reset();
		return;
	}
	uint32_t generation = shared->generation.load(std::memory_order_relaxed);

	// Waveform: the samples, then where they land in time, then the count
	// that makes them visible
	uint64_t start = shared->wave_count.load(std::memory_order_relaxed);
	for (int i = 0; i < count; i++) {
		float *dst = shared->wave[(start + (uint64_t)i) % WAVE_FRAMES];
		dst[0] = frames[i * 2];
		dst[1] = frames[i * 2 + 1];
	}
	seq_write_begin(shared->wave_anchor_seq);
	shared->wave_anchor_generation = generation;
	shared->wave_anchor_frame = start;
	shared->wave_anchor_time_ns = first_audible_ns;
	seq_write_end(shared->wave_anchor_seq);
	shared->wave_count.store(start + (uint64_t)count, std::memory_order_release);

	// Spectrum: one frame per completed block, stamped with when its last
	// sample is heard
	int offset = 0;
	while (offset < count) {
		bool block_done = false;
		offset += analyzer.feed(frames + offset * 2, count - offset, block_done);
		if (!block_done) {
			continue;
		}
		uint64_t n = shared->spectrum_count.load(std::memory_order_relaxed);
		SpectrumFrame &frame = shared->spectrum[n % SPECTRUM_FRAMES];
		seq_write_begin(frame.seq);
		frame.generation = generation;
		frame.audible_time_ns = first_audible_ns + (int64_t)(offset - 1) * 1000000000 / sample_rate;
		memcpy(frame.magnitude, analyzer.magnitude(), sizeof(frame.magnitude));
		seq_write_end(frame.seq);
		shared->spectrum_count.store(n + 1, std::memory_order_release);
	}
}

// ---- C API ----

namespace {

std::mutex shared_engine_mutex;
SongoEngine *shared_engine = nullptr;
int shared_engine_refs = 0;

void send(SongoEngine *engine, Command command) {
	if (engine) {
		engine->push(std::move(command));
	}
}

Command make(Command::Type type) {
	Command command;
	command.type = type;
	return command;
}

} // namespace

extern "C" {

int songo_engine_api_version(void) {
	return SONGO_ENGINE_API_VERSION;
}

SongoEngine *songo_engine_create(const SongoEngineConfig *config) {
	SongoEngine *engine = new (std::nothrow) SongoEngine(config);
	if (engine && !engine->init()) {
		delete engine;
		return nullptr;
	}
	return engine;
}

void songo_engine_destroy(SongoEngine *engine) {
	delete engine;
}

SongoEngine *songo_engine_acquire_shared(const SongoEngineConfig *config) {
	std::lock_guard<std::mutex> lock(shared_engine_mutex);
	if (!shared_engine) {
		shared_engine = songo_engine_create(config);
		if (!shared_engine) {
			return nullptr;
		}
	}
	shared_engine_refs++;
	return shared_engine;
}

void songo_engine_release_shared(SongoEngine *engine) {
	std::lock_guard<std::mutex> lock(shared_engine_mutex);
	if (!engine || engine != shared_engine) {
		return;
	}
	if (--shared_engine_refs == 0) {
		delete shared_engine;
		shared_engine = nullptr;
	}
}

void songo_engine_set_queue(SongoEngine *engine, const char *const *paths, int count,
		int start_index, double start_position, int autoplay) {
	Command command = make(Command::SET_QUEUE);
	command.paths = copy_paths(paths, count);
	command.index = start_index;
	command.value = start_position;
	command.flag = autoplay != 0;
	send(engine, std::move(command));
}

void songo_engine_replace_queue(SongoEngine *engine, const char *const *paths, int count, int current_index) {
	Command command = make(Command::REPLACE_QUEUE);
	command.paths = copy_paths(paths, count);
	command.index = current_index;
	send(engine, std::move(command));
}

void songo_engine_play_index(SongoEngine *engine, int index, double position) {
	Command command = make(Command::PLAY_INDEX);
	command.index = index;
	command.value = position;
	send(engine, std::move(command));
}

void songo_engine_resume(SongoEngine *engine) {
	send(engine, make(Command::RESUME));
}

void songo_engine_pause(SongoEngine *engine) {
	send(engine, make(Command::PAUSE));
}

void songo_engine_stop(SongoEngine *engine) {
	send(engine, make(Command::STOP));
}

void songo_engine_seek(SongoEngine *engine, double seconds) {
	Command command = make(Command::SEEK);
	command.value = seconds;
	send(engine, std::move(command));
}

void songo_engine_next(SongoEngine *engine) {
	send(engine, make(Command::NEXT));
}

void songo_engine_previous(SongoEngine *engine) {
	send(engine, make(Command::PREVIOUS));
}

void songo_engine_set_volume(SongoEngine *engine, float volume) {
	Command command = make(Command::VOLUME);
	command.value = volume;
	send(engine, std::move(command));
}

void songo_engine_set_eq(SongoEngine *engine, int enabled, const float *gains_db, int count) {
	Command command = make(Command::EQ);
	command.flag = enabled != 0;
	for (int i = 0; gains_db && i < count && i < EQ_BANDS; i++) {
		command.gains.push_back(gains_db[i]);
	}
	send(engine, std::move(command));
}

void songo_engine_set_blend(SongoEngine *engine, double seconds) {
	Command command = make(Command::BLEND);
	command.value = seconds;
	send(engine, std::move(command));
}

void songo_engine_set_repeat_one(SongoEngine *engine, int enabled) {
	Command command = make(Command::REPEAT_ONE);
	command.flag = enabled != 0;
	send(engine, std::move(command));
}

void songo_engine_set_loop_queue(SongoEngine *engine, int enabled) {
	Command command = make(Command::LOOP_QUEUE);
	command.flag = enabled != 0;
	send(engine, std::move(command));
}

void songo_engine_set_network_timeout(SongoEngine *engine, double seconds) {
	Command command = make(Command::NETWORK_TIMEOUT);
	command.value = seconds;
	send(engine, std::move(command));
}

void songo_engine_set_user_agent(SongoEngine *engine, const char *user_agent) {
	Command command = make(Command::USER_AGENT);
	command.text = user_agent ? user_agent : "";
	send(engine, std::move(command));
}

void songo_engine_set_latency(SongoEngine *engine, int milliseconds) {
	Command command = make(Command::LATENCY);
	command.index = milliseconds;
	send(engine, std::move(command));
}

void songo_engine_set_stream_buffer(SongoEngine *engine, double seconds) {
	Command command = make(Command::STREAM_BUFFER);
	command.value = seconds;
	send(engine, std::move(command));
}

void songo_engine_set_skip_failed(SongoEngine *engine, int enabled) {
	Command command = make(Command::SKIP_FAILED);
	command.flag = enabled != 0;
	send(engine, std::move(command));
}

SongoSubscription *songo_engine_subscribe(SongoEngine *engine) {
	return engine ? engine->subscribe() : nullptr;
}

void songo_subscription_close(SongoSubscription *subscription) {
	if (!subscription) {
		return;
	}
	// The shared engine lock keeps the engine from being destroyed under us
	std::lock_guard<std::mutex> lock(shared_engine_mutex);
	if (subscription->engine) {
		subscription->engine->unsubscribe(subscription);
	}
#ifndef _WIN32
	if (subscription->fd >= 0) {
		close(subscription->fd);
	}
#endif
	delete subscription;
}

int songo_subscription_poll(SongoSubscription *subscription, SongoEvent *r_event) {
	if (!subscription || !r_event) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(subscription->mutex);
	if (subscription->events.empty()) {
		// Only cleared with the queue empty and locked, so an event pushed
		// after this always leaves the fd readable
#ifndef _WIN32
		uint64_t ignored;
		while (subscription->fd >= 0 && read(subscription->fd, &ignored, sizeof(ignored)) > 0) {
		}
#endif
		return 0;
	}
	subscription->current = std::move(subscription->events.front());
	subscription->events.pop_front();
	subscription->current_paths.clear();
	for (const std::string &path : subscription->current.paths) {
		subscription->current_paths.push_back(path.c_str());
	}

	const Event &event = subscription->current;
	r_event->type = event.type;
	r_event->index = event.index;
	r_event->flags = event.flags;
	r_event->value = event.value;
	r_event->text = event.text.c_str();
	r_event->paths = subscription->current_paths.data();
	r_event->path_count = (int)subscription->current_paths.size();
	return 1;
}

int songo_subscription_fd(SongoSubscription *subscription) {
	return subscription ? subscription->fd : -1;
}

void *songo_engine_shared(SongoEngine *engine, size_t *r_size) {
	if (!engine) {
		return nullptr;
	}
	if (r_size) {
		*r_size = engine->shared_size;
	}
	return engine->shared;
}

int songo_engine_shared_fd(SongoEngine *engine) {
	return engine ? engine->shared_fd : -1;
}

} // extern "C"
