// Songo's 10 band graphic equalizer.
//
// Each band is a peaking filter (RBJ Audio EQ Cookbook) at an octave centre,
// chained in series. Neighbouring bands overlap, so the filter gains are
// solved for together such that the response at every band's centre
// frequency is exactly the gain asked for. A band left at 0 dB with flat
// neighbours is skipped outright, which makes the EQ with every band at 0
// (and no output gain) bit-identical to having it off.
//
// After the bands comes one output level: auto preamp (on by default) takes
// off the largest boost so boosts don't clip, plus a manual output gain. The
// filters shift phase, so a song already at full scale can still peak a
// fraction of a dB over; the output clips that.

#ifndef SONGO_EQUALIZER_H
#define SONGO_EQUALIZER_H

#include <algorithm>
#include <cmath>

class Equalizer10 {
public:
	static const int BAND_COUNT = 10;

	explicit Equalizer10(double p_mix_rate) :
			mix_rate(p_mix_rate) {
		static const double freqs[BAND_COUNT] = { 31.25, 62.5, 125, 250, 500, 1e3, 2e3, 4e3, 8e3, 16e3 };
		for (int i = 0; i < BAND_COUNT; i++) {
			band[i].freq = freqs[i];
			// Bands at or past Nyquist can't be represented at this rate
			band[i].usable = freqs[i] < mix_rate * 0.5;
		}
	}

	// Sets every band at once, in dB; missing bands are 0
	void set_band_gains_db(const float *p_gains_db, int p_count) {
		double target[BAND_COUNT];
		for (int i = 0; i < BAND_COUNT; i++) {
			target[i] = (i < p_count && band[i].usable) ? p_gains_db[i] : 0.0;
		}
		solve(target);
	}

	// The level after the bands. Auto preamp lowers everything by the largest
	// boost the bands make, so boosting doesn't push music past full scale; the
	// output gain in dB is added on top, so it can trade that headroom back
	// for loudness (or turn things down further).
	void set_output(bool p_auto_preamp, float p_output_gain_db) {
		auto_preamp = p_auto_preamp;
		output_gain_db = p_output_gain_db;
		update_output_gain();
	}

	// The output level in dB the current settings work out to
	double get_output_gain_db() const {
		return (auto_preamp ? -peak_boost_db : 0.0) + output_gain_db;
	}

	// In-place over interleaved stereo float frames
	void process(float *p_frames, int p_frame_count) {
		for (int j = 0; j < BAND_COUNT; j++) {
			Band &b = band[j];
			if (!b.active) {
				continue;
			}
			for (int i = 0; i < p_frame_count; i++) {
				for (int ch = 0; ch < 2; ch++) {
					float &sample = p_frames[i * 2 + ch];
					// Transposed direct form II, in double so the low bands stay stable
					double x = sample;
					double y = b.b0 * x + b.z1[ch];
					b.z1[ch] = b.b1 * x - b.a1 * y + b.z2[ch];
					b.z2[ch] = b.b2 * x - b.a2 * y;
					sample = (float)y;
				}
			}
		}

		// Ramped across the chunk when it changes, so adjusting it doesn't click
		if (p_frame_count > 0 && (target_gain != 1.0f || applied_gain != 1.0f)) {
			for (int i = 0; i < p_frame_count; i++) {
				float gain = applied_gain + (target_gain - applied_gain) * (float)(i + 1) / (float)p_frame_count;
				p_frames[i * 2] *= gain;
				p_frames[i * 2 + 1] *= gain;
			}
			applied_gain = target_gain;
		}
	}

private:
	struct Band {
		double freq = 0.0;
		bool usable = true;
		bool active = false;
		double gain_db = 0.0;
		double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
		double z1[2] = {};
		double z2[2] = {};
	};

	struct Coeffs {
		double b0, b1, b2, a1, a2;
	};

	double mix_rate;
	Band band[BAND_COUNT];

	bool auto_preamp = true;
	double output_gain_db = 0.0;
	double peak_boost_db = 0.0;
	float target_gain = 1.0f;
	float applied_gain = 1.0f;

	// Peaking EQ. Q 0.7 is wide enough that bands set alike blend smoothly
	// between centres (all +12 stays within 10.4 to 12.4 dB), while a lone
	// band only nudges its neighbours by under 1 dB
	Coeffs peaking(double p_freq, double p_gain_db) const {
		const double q = 0.7;
		double a = std::pow(10.0, p_gain_db / 40.0);
		double w0 = 6.283185307179586 * p_freq / mix_rate;
		double alpha = std::sin(w0) / (2.0 * q);
		double cos_w0 = std::cos(w0);
		double a0 = 1.0 + alpha / a;
		return {
			(1.0 + alpha * a) / a0,
			(-2.0 * cos_w0) / a0,
			(1.0 - alpha * a) / a0,
			(-2.0 * cos_w0) / a0,
			(1.0 - alpha / a) / a0,
		};
	}

	// A filter's gain in dB at p_freq
	double response_db(const Coeffs &c, double p_freq) const {
		double w = 6.283185307179586 * p_freq / mix_rate;
		double cw = std::cos(w), sw = std::sin(w), c2w = std::cos(2 * w), s2w = std::sin(2 * w);
		double nr = c.b0 + c.b1 * cw + c.b2 * c2w, ni = -(c.b1 * sw + c.b2 * s2w);
		double dr = 1.0 + c.a1 * cw + c.a2 * c2w, di = -(c.a1 * sw + c.a2 * s2w);
		return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
	}

	// Response in dB of band p_band, set to p_gain_db, at band p_at's centre
	double band_at(int p_band, double p_gain_db, int p_at) const {
		if (p_gain_db == 0.0) {
			return 0.0;
		}
		return response_db(peaking(band[p_band].freq, p_gain_db), band[p_at].freq);
	}

	// Combined response at every centre, and how far each misses p_target
	double centre_error(const double *p_gain, const double *p_target, double *r_error) const {
		double worst = 0.0;
		for (int i = 0; i < BAND_COUNT; i++) {
			r_error[i] = 0.0;
			if (!band[i].usable) {
				continue;
			}
			double got = 0.0;
			for (int j = 0; j < BAND_COUNT; j++) {
				if (band[j].usable) {
					got += band_at(j, p_gain[j], i);
				}
			}
			r_error[i] = p_target[i] - got;
			worst = std::max(worst, std::fabs(r_error[i]));
		}
		return worst;
	}

	// Solves p_m * r_x = p_b in place (Gaussian elimination, partial pivoting)
	static bool linear_solve(double p_m[BAND_COUNT][BAND_COUNT], double *p_b, double *r_x) {
		for (int col = 0; col < BAND_COUNT; col++) {
			int pivot = col;
			for (int row = col + 1; row < BAND_COUNT; row++) {
				if (std::fabs(p_m[row][col]) > std::fabs(p_m[pivot][col])) {
					pivot = row;
				}
			}
			if (std::fabs(p_m[pivot][col]) < 1e-12) {
				return false;
			}
			std::swap(p_m[col], p_m[pivot]);
			std::swap(p_b[col], p_b[pivot]);
			for (int row = col + 1; row < BAND_COUNT; row++) {
				double f = p_m[row][col] / p_m[col][col];
				for (int k = col; k < BAND_COUNT; k++) {
					p_m[row][k] -= f * p_m[col][k];
				}
				p_b[row] -= f * p_b[col];
			}
		}
		for (int row = BAND_COUNT - 1; row >= 0; row--) {
			double sum = p_b[row];
			for (int k = row + 1; k < BAND_COUNT; k++) {
				sum -= p_m[row][k] * r_x[k];
			}
			r_x[row] = sum / p_m[row][row];
		}
		return true;
	}

	// Finds the filter gains whose combined response hits p_target at every
	// centre, with Newton's method over all bands at once since each filter
	// reaches into its neighbours' centres
	void solve(const double *p_target) {
		const double MIN_GAIN = -60.0, MAX_GAIN = 30.0;
		double gain[BAND_COUNT];
		bool flat = true;
		for (int i = 0; i < BAND_COUNT; i++) {
			gain[i] = p_target[i];
			flat = flat && p_target[i] == 0.0;
		}

		if (!flat) {
			double error[BAND_COUNT];
			double worst = centre_error(gain, p_target, error);
			for (int pass = 0; pass < 30 && worst > 0.001; pass++) {
				// How each band's gain moves every centre, measured around the current gains
				double jacobian[BAND_COUNT][BAND_COUNT];
				for (int j = 0; j < BAND_COUNT; j++) {
					const double h = 0.01;
					for (int i = 0; i < BAND_COUNT; i++) {
						if (!band[i].usable || !band[j].usable) {
							jacobian[i][j] = i == j ? 1.0 : 0.0;
						} else {
							jacobian[i][j] = (band_at(j, gain[j] + h, i) - band_at(j, gain[j] - h, i)) / (2 * h);
						}
					}
				}
				double step[BAND_COUNT];
				double rhs[BAND_COUNT];
				std::copy(error, error + BAND_COUNT, rhs);
				if (!linear_solve(jacobian, rhs, step)) {
					break;
				}

				// Halves the step until it actually helps
				bool improved = false;
				for (double scale = 1.0; scale > 1e-3; scale *= 0.5) {
					double trial[BAND_COUNT];
					double trial_error[BAND_COUNT];
					for (int j = 0; j < BAND_COUNT; j++) {
						trial[j] = band[j].usable ? std::clamp(gain[j] + step[j] * scale, MIN_GAIN, MAX_GAIN) : 0.0;
					}
					double trial_worst = centre_error(trial, p_target, trial_error);
					if (trial_worst < worst) {
						std::copy(trial, trial + BAND_COUNT, gain);
						std::copy(trial_error, trial_error + BAND_COUNT, error);
						worst = trial_worst;
						improved = true;
						break;
					}
				}
				if (!improved) {
					// The closest these filters can get to an extreme setting
					break;
				}
			}
		}

		for (int j = 0; j < BAND_COUNT; j++) {
			Band &b = band[j];
			bool active = b.usable && gain[j] != 0.0;
			if (!active) {
				b.active = false;
				b.gain_db = 0.0;
				b.z1[0] = b.z1[1] = b.z2[0] = b.z2[1] = 0.0;
				continue;
			}
			if (b.active && b.gain_db == gain[j]) {
				continue;
			}
			// Keeps the filter state so a change mid song doesn't click
			Coeffs c = peaking(b.freq, gain[j]);
			b.b0 = c.b0;
			b.b1 = c.b1;
			b.b2 = c.b2;
			b.a1 = c.a1;
			b.a2 = c.a2;
			b.gain_db = gain[j];
			b.active = true;
		}

		peak_boost_db = measure_peak_boost();
		update_output_gain();
	}

	// The most the active bands lift any frequency, in dB (0 when nothing is
	// boosted). Read off a fine sweep, since the curve can rise a little past
	// a band's own setting between centres.
	double measure_peak_boost() const {
		bool any = false;
		for (int j = 0; j < BAND_COUNT; j++) {
			any = any || band[j].active;
		}
		if (!any) {
			return 0.0;
		}

		Coeffs coeffs[BAND_COUNT];
		for (int j = 0; j < BAND_COUNT; j++) {
			if (band[j].active) {
				coeffs[j] = { band[j].b0, band[j].b1, band[j].b2, band[j].a1, band[j].a2 };
			}
		}
		const double low = 20.0;
		const double high = std::min(20000.0, mix_rate * 0.49);
		const int steps = 400;
		double peak = 0.0;
		for (int s = 0; s <= steps; s++) {
			double freq = low * std::pow(high / low, (double)s / steps);
			double total = 0.0;
			for (int j = 0; j < BAND_COUNT; j++) {
				if (band[j].active) {
					total += response_db(coeffs[j], freq);
				}
			}
			peak = std::max(peak, total);
		}
		return peak;
	}

	void update_output_gain() {
		double gain_db = (auto_preamp ? -peak_boost_db : 0.0) + output_gain_db;
		// Exactly 1 at 0 dB, so a flat EQ leaves samples untouched
		target_gain = gain_db == 0.0 ? 1.0f : (float)std::pow(10.0, gain_db / 20.0);
	}
};

#endif // SONGO_EQUALIZER_H
