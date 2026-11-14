// A spectrum analyzer that measures the same way as Godot's
// AudioEffectSpectrumAnalyzer at its default FFT_SIZE_1024, so Songo's
// visualizers keep their tuning (they map magnitudes from -60dB to 0dB).
//
// Like Godot's, it analyses back to back blocks of 2 * FFT_SIZE frames with
// no overlap, windows them with -0.5 * cos(2pi * i / FFT_SIZE) + 0.5 (which
// runs over the block twice, as Godot's does), and stores |X[k]| / FFT_SIZE
// for the first FFT_SIZE bins of each channel. The FFT is a plain radix-2
// one rather than Godot's smbFft, and both channels go through it at once as
// the real and imaginary parts of one complex signal.

#ifndef SONGO_SPECTRUM_ANALYZER_H
#define SONGO_SPECTRUM_ANALYZER_H

#include "songo_shared.h"

#include <complex>

namespace songo {

class SpectrumAnalyzer {
public:
	static constexpr int BLOCK_FRAMES = FFT_SIZE * 2;

	SpectrumAnalyzer();

	// Drops the partly filled block, e.g. after a skip
	void reset() { fill = 0; }

	// Feeds `count` interleaved stereo frames. Returns how many were taken,
	// stopping early when a block completes; magnitude() then holds its
	// result until the next call.
	int feed(const float *frames, int count, bool &r_block_done);

	const float (&magnitude() const)[2][FFT_SIZE] { return result; }

private:
	float window[BLOCK_FRAMES];
	int bit_reverse[BLOCK_FRAMES];
	std::complex<float> twiddle[BLOCK_FRAMES / 2];

	std::complex<float> buffer[BLOCK_FRAMES];
	int fill = 0;
	float result[2][FFT_SIZE];

	void analyse();
};

} // namespace songo

#endif // SONGO_SPECTRUM_ANALYZER_H
