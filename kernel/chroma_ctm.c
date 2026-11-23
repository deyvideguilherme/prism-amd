// SPDX-License-Identifier: GPL-2.0
#include "chroma_internal.h"

/* Rec.709 integer luminance weights scaled by 10000 */
#define REC709_RW 2126LL
#define REC709_GW 7152LL
#define REC709_BW 722LL

u64 to_s31_32(s64 num, u64 den)
{
	u64 sign = 0;
	u64 abs_num;
	u64 mag;

	if (num < 0) {
		sign = 1ULL << 63;
		abs_num = (u64)(-num);
	} else {
		abs_num = (u64)num;
	}

	/* Scale magnitude by 2^32 with safe intermediate 128-bit division (no FPU) */
	mag = mul_u64_u64_div_u64(abs_num, 1ULL << 32, den);

	/* Safe clamp to S31.32 maximum magnitude (bits 0..62) */
	if (mag > ((1ULL << 63) - 1))
		mag = (1ULL << 63) - 1;

	return sign | mag;
}

/*
 * Computes the 3x3 Rec.709 CTM matrix incorporating saturation, contrast,
 * and linear brightness gain using strictly 64-bit integer arithmetic.
 *
 * Brightness (-100 to 100) is applied as a proportional linear gain factor:
 * - brightness = -100 -> gain_q16 = 16384  (0.25x)
 * - brightness =    0 -> gain_q16 = 65536  (1.00x)
 * - brightness = +100 -> gain_q16 = 114688 (1.75x)
 * Formula: gain_q16 = 65536 + div_s64(brightness * 49152, 100)
 */
void chroma_compute_ctm(struct drm_color_ctm *ctm, int sat_val, int con_val, int bri_val)
{
	s64 sat = (s64)sat_val;
	s64 contrast = (s64)con_val;
	s64 brightness = (s64)bri_val;
	s64 one_minus_s = 100LL - sat;
	s64 weights[3] = { REC709_RW, REC709_GW, REC709_BW };
	s64 gain_q16;
	s64 den = 100000000LL * 65536LL; /* 100 (sat) * 10000 (luma) * 100 (contrast) * 65536 (gain_q16) */
	int r, c;

	gain_q16 = 65536LL + div_s64(brightness * 49152LL, 100LL);
	if (gain_q16 < 0)
		gain_q16 = 0;

	for (r = 0; r < 3; ++r) {
		for (c = 0; c < 3; ++c) {
			s64 w = weights[c];
			s64 val;

			if (r == c)
				val = (one_minus_s * w + sat * 10000LL) * contrast;
			else
				val = (one_minus_s * w) * contrast;

			val = val * gain_q16;

			ctm->matrix[r * 3 + c] = to_s31_32(val, den);
		}
	}
}

bool chroma_is_identity_ctm(const struct drm_color_ctm *ctm)
{
	int i;

	if (!ctm)
		return true;

	for (i = 0; i < 9; ++i) {
		u64 val = ctm->matrix[i];

		if (i == 0 || i == 4 || i == 8) {
			if (val != (1ULL << 32))
				return false;
		} else {
			if ((val & ~(1ULL << 63)) != 0ULL)
				return false;
		}
	}
	return true;
}
