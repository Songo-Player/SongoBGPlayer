#include "spectrum_analyzer.h"

#include <cmath>

namespace songo {

SpectrumAnalyzer::SpectrumAnalyzer() {
	const double tau = 6.2831853071795864769252867666;
	for (int i = 0; i < BLOCK_FRAMES; i++) {
		// Godot's window, worked out in double and stored as float like it does
		window[i] = (float)(-0.5 * std::cos(tau / (double)FFT_SIZE * (double)i) + 0.5);
	}

	int bits = 0;
	while ((1 << bits) < BLOCK_FRAMES) {
		bits++;
	}
	for (int i = 0; i < BLOCK_FRAMES; i++) {
		int reversed = 0;
		for (int b = 0; b < bits; b++) {
			if (i & (1 << b)) {
				reversed |= 1 << (bits - 1 - b);
			}
		}
		bit_reverse[i] = reversed;
	}

	for (int k = 0; k < BLOCK_FRAMES / 2; k++) {
		double angle = -tau * (double)k / (double)BLOCK_FRAMES;
		twiddle[k] = std::complex<float>((float)std::cos(angle), (float)std::sin(angle));
	}

	for (int c = 0; c < 2; c++) {
		for (int k = 0; k < FFT_SIZE; k++) {
			result[c][k] = 0.0f;
		}
	}
}

int SpectrumAnalyzer::feed(const float *frames, int count, bool &r_block_done) {
	r_block_done = false;
	int taken = 0;
	while (taken < count) {
		float w = window[fill];
		buffer[bit_reverse[fill]] = std::complex<float>(w * frames[taken * 2], w * frames[taken * 2 + 1]);
		taken++;
		if (++fill == BLOCK_FRAMES) {
			analyse();
			fill = 0;
			r_block_done = true;
			break;
		}
	}
	return taken;
}

void SpectrumAnalyzer::analyse() {
	// The buffer was filled in bit reversed order, so this is the in-place
	// iterative Cooley-Tukey FFT from there
	for (int size = 2; size <= BLOCK_FRAMES; size <<= 1) {
		int half = size >> 1;
		int step = BLOCK_FRAMES / size;
		for (int start = 0; start < BLOCK_FRAMES; start += size) {
			for (int k = 0; k < half; k++) {
				std::complex<float> t = twiddle[k * step] * buffer[start + k + half];
				std::complex<float> u = buffer[start + k];
				buffer[start + k] = u + t;
				buffer[start + k + half] = u - t;
			}
		}
	}

	// Left went in as the real part and right as the imaginary part. With
	// Z = FFT(L + iR), FFT(L)[k] = (Z[k] + conj(Z[N-k])) / 2 and
	// FFT(R)[k] = (Z[k] - conj(Z[N-k])) / 2i.
	const float scale = 1.0f / (2.0f * (float)FFT_SIZE);
	for (int k = 0; k < FFT_SIZE; k++) {
		std::complex<float> z = buffer[k];
		std::complex<float> mirror = std::conj(buffer[(BLOCK_FRAMES - k) % BLOCK_FRAMES]);
		result[0][k] = std::abs(z + mirror) * scale;
		result[1][k] = std::abs(z - mirror) * scale;
	}
}

} // namespace songo
