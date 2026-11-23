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
 * Computes the 3x3 Rec.709 CTM matrix incorporating exclusively saturation (0..400)
 * in DRM S31.32 fixed-point using strictly 64-bit integer arithmetic (zero FPU).
 */
void chroma_compute_ctm(struct drm_color_ctm *ctm, int sat_val)
{
	s64 sat = (s64)sat_val;
	s64 one_minus_s = 100LL - sat;
	s64 weights[3] = { REC709_RW, REC709_GW, REC709_BW };
	s64 den = 100LL * 10000LL; /* 100 (sat) * 10000 (Rec.709 weights) = 1,000,000 */
	int r, c;

	if (!ctm)
		return;

	for (r = 0; r < 3; ++r) {
		for (c = 0; c < 3; ++c) {
			s64 w = weights[c];
			s64 val;

			if (r == c)
				val = one_minus_s * w + sat * 10000LL;
			else
				val = one_minus_s * w;

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

/*
 * Computes a 1D DRM GAMMA_LUT incorporating contrast (10..300) with a pivot
 * at mid-gray (0.5) and brightness (-100..100) as a linear offset limited to +/-50%
 * using strictly 64-bit integer arithmetic (zero FPU).
 */
void chroma_compute_gamma_lut(struct drm_color_lut *lut, size_t size, int con_val, int bri_val)
{
	s64 D;
	s64 den;
	size_t i;

	if (!lut || size < 2)
		return;

	D = (s64)(size - 1);
	den = 200LL * D;

	for (i = 0; i < size; ++i) {
		s64 num = (2LL * (s64)i - D) * (s64)con_val + (100LL + (s64)bri_val) * D;
		u16 val;

		if (num <= 0) {
			val = 0;
		} else if (num >= den) {
			val = 65535;
		} else {
			u64 tmp = (u64)num * 65535ULL + (u64)den / 2ULL;
			val = (u16)div64_u64(tmp, (u64)den);
		}

		lut[i].red = val;
		lut[i].green = val;
		lut[i].blue = val;
		lut[i].reserved = 0;
	}
}

/*
 * Validates whether a given GAMMA_LUT is an identity ramp (neutral curve).
 * Allows +/-1 tolerance per color channel to accommodate integer rounding
 * differences across drivers and compositors. Returns false if any channel
 * deviates by >1 (e.g. Night Light, wlsunset, ICC curves).
 */
bool chroma_is_identity_gamma_lut(const struct drm_color_lut *lut, size_t count)
{
	s64 D;
	size_t i;

	if (!lut || count < 2)
		return true;

	D = (s64)(count - 1);

	for (i = 0; i < count; ++i) {
		u64 tmp = (u64)i * 65535ULL + (u64)D / 2ULL;
		u16 expected = (u16)div64_u64(tmp, (u64)D);
		int diff_r = (int)lut[i].red - (int)expected;
		int diff_g = (int)lut[i].green - (int)expected;
		int diff_b = (int)lut[i].blue - (int)expected;

		if (diff_r < -1 || diff_r > 1 ||
		    diff_g < -1 || diff_g > 1 ||
		    diff_b < -1 || diff_b > 1)
			return false;
	}

	return true;
}
