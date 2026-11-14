#include "output.h"

namespace songo {

std::unique_ptr<Output> create_null_output(int sample_rate, int latency_ms);
#ifdef SONGO_HAS_ALSA
std::unique_ptr<Output> create_alsa_output(const std::string &device, int sample_rate, int latency_ms);
#endif
#ifdef __ANDROID__
std::unique_ptr<Output> create_aaudio_output(int sample_rate, int latency_ms);
#endif
#ifdef _WIN32
std::unique_ptr<Output> create_wasapi_output(const std::string &device, int sample_rate, int latency_ms);
#endif

std::unique_ptr<Output> create_output(const std::string &backend, const std::string &device,
		int sample_rate, int latency_ms) {
	if (backend == "null") {
		return create_null_output(sample_rate, latency_ms);
	}
#ifdef SONGO_HAS_ALSA
	if (backend.empty() || backend == "alsa") {
		return create_alsa_output(device, sample_rate, latency_ms);
	}
#endif
#ifdef __ANDROID__
	if (backend.empty() || backend == "aaudio") {
		// Null before Android 8.0, which has no AAudio
		return create_aaudio_output(sample_rate, latency_ms);
	}
#endif
#ifdef _WIN32
	if (backend.empty() || backend == "wasapi") {
		return create_wasapi_output(device, sample_rate, latency_ms);
	}
#endif
	return nullptr;
}

} // namespace songo
