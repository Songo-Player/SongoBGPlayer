// WASAPI output (Windows), in shared mode so Godot's own sounds play alongside.
//
// Float stereo at the engine's rate, with Windows converting to the device's
// mix format (AUTOCONVERTPCM). Writes block until the buffer, sized from the
// latency, has room, which is what paces decoding, as on ALSA. COM is set up
// on the engine thread, the only one that calls in here.

#include "output.h"

#include "songo_shared.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace songo {

namespace {

const int CHANNELS = 2;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out rather than pulled from
// ksmedia.h, which wants the GUIDs instantiated in exactly one place
const GUID SUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

template <typename T>
void release(T *&p_object) {
	if (p_object) {
		p_object->Release();
		p_object = nullptr;
	}
}

std::wstring to_wide(const std::string &p_utf8) {
	if (p_utf8.empty()) {
		return std::wstring();
	}
	int length = MultiByteToWideChar(CP_UTF8, 0, p_utf8.c_str(), (int)p_utf8.size(), nullptr, 0);
	std::wstring out((size_t)length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_utf8.c_str(), (int)p_utf8.size(), &out[0], length);
	return out;
}

std::string hresult_string(HRESULT p_hr) {
	char buf[32];
	snprintf(buf, sizeof(buf), "0x%08lx", (unsigned long)p_hr);
	return buf;
}

class WasapiOutput : public Output {
public:
	WasapiOutput(const std::string &p_device, int p_sample_rate, int p_latency_ms) :
			device(p_device), sample_rate(p_sample_rate), latency_ms(p_latency_ms) {}

	~WasapiOutput() override { close(); }

	bool open(const std::function<bool()> &, std::string &r_error) override {
		if (client) {
			return true;
		}
		if (!com_ready) {
			HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			// RPC_E_CHANGED_MODE: COM is already up on this thread, as STA,
			// which still lets us use these objects
			com_owned = SUCCEEDED(hr);
			com_ready = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
			if (!com_ready) {
				r_error = "Can't initialize COM for WASAPI: " + hresult_string(hr);
				return false;
			}
		}
		return open_device(r_error);
	}

	void close() override {
		if (client) {
			client->Stop();
		}
		release(render);
		release(client);
		paused = false;
		if (com_owned) {
			CoUninitialize();
			com_owned = false;
			com_ready = false;
		}
	}

	bool is_open() const override { return client != nullptr; }

	void write(const float *frames, int count) override {
		if (!client || paused) {
			return;
		}
		while (count > 0) {
			UINT32 padding = 0;
			HRESULT hr = client->GetCurrentPadding(&padding);
			if (FAILED(hr)) {
				if (!recover(hr)) {
					return;
				}
				continue;
			}
			UINT32 room = buffer_frames - padding;
			if (room == 0) {
				// Wait for about a quarter of the buffer to play out
				Sleep((DWORD)std::max(1, latency_ms / 4));
				continue;
			}
			UINT32 n = std::min<UINT32>(room, (UINT32)count);
			BYTE *data = nullptr;
			hr = render->GetBuffer(n, &data);
			if (FAILED(hr)) {
				if (!recover(hr)) {
					return;
				}
				continue;
			}
			// The only place samples get clipped, like the ALSA output
			float *out = reinterpret_cast<float *>(data);
			for (UINT32 i = 0; i < n * CHANNELS; i++) {
				out[i] = std::clamp(frames[i], -1.0f, 1.0f);
			}
			render->ReleaseBuffer(n, 0);
			if (!started) {
				client->Start();
				started = true;
			}
			frames += (size_t)n * CHANNELS;
			count -= (int)n;
		}
	}

	int64_t delay_frames() override {
		if (!client) {
			return 0;
		}
		UINT32 padding = 0;
		if (FAILED(client->GetCurrentPadding(&padding))) {
			return 0;
		}
		return (int64_t)padding + stream_latency_frames;
	}

	void flush() override {
		if (!client) {
			return;
		}
		client->Stop();
		client->Reset();
		started = false;
		// Flushing also ends a pause, as on ALSA
		paused = false;
	}

	void drain() override {
		if (!client || paused) {
			return;
		}
		if (!started) {
			client->Start();
			started = true;
		}
		while (true) {
			UINT32 padding = 0;
			if (FAILED(client->GetCurrentPadding(&padding)) || padding == 0) {
				break;
			}
			Sleep(5);
		}
		client->Stop();
		client->Reset();
		started = false;
	}

	void set_paused(bool p_paused) override {
		if (!client || p_paused == paused) {
			return;
		}
		paused = p_paused;
		if (paused) {
			// Keeps what's queued, so resuming carries on from it
			client->Stop();
		} else if (started) {
			client->Start();
		}
	}

private:
	std::string device;
	int sample_rate;
	int latency_ms;
	bool com_ready = false;
	bool com_owned = false;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	UINT32 buffer_frames = 0;
	int64_t stream_latency_frames = 0;
	bool started = false;
	bool paused = false;

	bool open_device(std::string &r_error) {
		IMMDeviceEnumerator *enumerator = nullptr;
		IMMDevice *endpoint = nullptr;
		HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
				__uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
		if (SUCCEEDED(hr)) {
			// A device id from IMMDevice::GetId, or the default output
			hr = device.empty() ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)
								: enumerator->GetDevice(to_wide(device).c_str(), &endpoint);
		}
		if (SUCCEEDED(hr)) {
			hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
		}
		release(endpoint);
		release(enumerator);
		if (FAILED(hr)) {
			r_error = "Can't open the WASAPI output device" + (device.empty() ? std::string() : " " + device) +
					": " + hresult_string(hr);
			release(client);
			return false;
		}

		WAVEFORMATEXTENSIBLE format = {};
		format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		format.Format.nChannels = CHANNELS;
		format.Format.nSamplesPerSec = (DWORD)sample_rate;
		format.Format.wBitsPerSample = 32;
		format.Format.nBlockAlign = CHANNELS * 4;
		format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
		format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		format.Samples.wValidBitsPerSample = 32;
		format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
		format.SubFormat = SUBTYPE_IEEE_FLOAT;

		REFERENCE_TIME buffer_duration = (REFERENCE_TIME)latency_ms * 10000;
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
				buffer_duration, 0, reinterpret_cast<WAVEFORMATEX *>(&format), nullptr);
		if (SUCCEEDED(hr)) {
			hr = client->GetBufferSize(&buffer_frames);
		}
		if (SUCCEEDED(hr)) {
			hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render));
		}
		if (FAILED(hr)) {
			r_error = "Can't set up the WASAPI output: " + hresult_string(hr);
			release(render);
			release(client);
			return false;
		}
		REFERENCE_TIME latency = 0;
		stream_latency_frames = SUCCEEDED(client->GetStreamLatency(&latency)) ? (int64_t)latency * sample_rate / 10000000 : 0;
		started = false;
		paused = false;
		return true;
	}

	// The device went away (unplugged, default changed, exclusive grab):
	// reopen the default one. False if that fails, which drops this write.
	bool recover(HRESULT p_hr) {
		fprintf(stderr, "WASAPI write failed: %s\n", hresult_string(p_hr).c_str());
		if (p_hr != AUDCLNT_E_DEVICE_INVALIDATED && p_hr != AUDCLNT_E_SERVICE_NOT_RUNNING) {
			return false;
		}
		release(render);
		release(client);
		std::string error;
		device.clear();
		if (!open_device(error)) {
			fprintf(stderr, "%s\n", error.c_str());
			return false;
		}
		return true;
	}
};

} // namespace

std::unique_ptr<Output> create_wasapi_output(const std::string &device, int sample_rate, int latency_ms) {
	return std::make_unique<WasapiOutput>(device, sample_rate, latency_ms);
}

} // namespace songo
