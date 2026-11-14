// ALSA output, as songo_bgplayer has always played: interleaved S16 through
// the "default" device, whose plug layer converts to whatever the hardware
// wants.

#include "output.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>
#include <vector>

#include "songo_shared.h"

namespace songo {

namespace {

// Songo starts the daemon while it's still shutting down. Without a sound
// server it holds the device until it exits, so keep retrying for a while.
const double OPEN_TIMEOUT_SEC = 5.0;
const int CHANNELS = 2;

class AlsaOutput : public Output {
public:
	AlsaOutput(const std::string &p_device, int p_sample_rate, int p_latency_ms) :
			device(p_device.empty() ? "default" : p_device), sample_rate(p_sample_rate), latency_ms(p_latency_ms) {}

	~AlsaOutput() override { close(); }

	bool open(const std::function<bool()> &cancelled, std::string &r_error) override {
		if (pcm) {
			return true;
		}
		int err = 0;
		int64_t give_up_at = monotonic_ns() + (int64_t)(OPEN_TIMEOUT_SEC * 1e9);
		while (true) {
			err = snd_pcm_open(&pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
			if (err >= 0 || monotonic_ns() >= give_up_at || cancelled()) {
				break;
			}
			usleep(100000);
		}
		if (err < 0) {
			pcm = nullptr;
			r_error = std::string("Can't open ALSA device ") + device + ": " + snd_strerror(err);
			return false;
		}
		// Non-blocking was only for the open, writes should block to pace decoding
		snd_pcm_nonblock(pcm, 0);
		err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				CHANNELS, sample_rate, 1, (unsigned)latency_ms * 1000);
		if (err < 0) {
			r_error = std::string("Can't set up ALSA device ") + device + ": " + snd_strerror(err);
			snd_pcm_close(pcm);
			pcm = nullptr;
			return false;
		}

		snd_pcm_hw_params_t *params;
		snd_pcm_hw_params_alloca(&params);
		can_pause = snd_pcm_hw_params_current(pcm, params) == 0 && snd_pcm_hw_params_can_pause(params);
		paused = false;
		return true;
	}

	void close() override {
		if (pcm) {
			snd_pcm_close(pcm);
			pcm = nullptr;
		}
	}

	bool is_open() const override { return pcm != nullptr; }

	void write(const float *frames, int count) override {
		if (!pcm || paused) {
			return;
		}
		samples.resize((size_t)count * CHANNELS);
		for (size_t i = 0; i < samples.size(); i++) {
			// The only place samples get clipped, so EQ boosts and blends can
			// go past full scale on the way, like they can on Godot's float buses
			samples[i] = (int16_t)std::lround(std::clamp(frames[i] * 32768.0f, -32768.0f, 32767.0f));
		}
		const int16_t *cursor = samples.data();
		while (count > 0) {
			snd_pcm_sframes_t written = snd_pcm_writei(pcm, cursor, count);
			if (written < 0) {
				// Underruns and suspend/resume
				written = snd_pcm_recover(pcm, (int)written, 1);
				if (written < 0) {
					fprintf(stderr, "ALSA write failed: %s\n", snd_strerror((int)written));
					return;
				}
				continue;
			}
			cursor += written * CHANNELS;
			count -= (int)written;
		}
	}

	int64_t delay_frames() override {
		if (!pcm) {
			return 0;
		}
		snd_pcm_sframes_t delay = 0;
		if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0) {
			return 0;
		}
		return delay;
	}

	void flush() override {
		if (!pcm) {
			return;
		}
		snd_pcm_drop(pcm);
		snd_pcm_prepare(pcm);
		// Dropping also ends a pause
		paused = false;
	}

	void drain() override {
		if (pcm && !paused) {
			snd_pcm_drain(pcm);
			snd_pcm_prepare(pcm);
		}
	}

	void set_paused(bool p_paused) override {
		if (!pcm || p_paused == paused) {
			return;
		}
		if (can_pause && snd_pcm_pause(pcm, p_paused ? 1 : 0) == 0) {
			paused = p_paused;
			return;
		}
		// Devices that can't pause lose what was queued (up to the latency)
		if (p_paused) {
			snd_pcm_drop(pcm);
		}
		snd_pcm_prepare(pcm);
		paused = p_paused;
	}

private:
	std::string device;
	int sample_rate;
	int latency_ms;
	snd_pcm_t *pcm = nullptr;
	bool can_pause = false;
	bool paused = false;
	std::vector<int16_t> samples;
};

} // namespace

std::unique_ptr<Output> create_alsa_output(const std::string &device, int sample_rate, int latency_ms) {
	return std::make_unique<AlsaOutput>(device, sample_rate, latency_ms);
}

} // namespace songo
