#if __has_include(<drm/drm_mode.h>)
#include <drm/drm_mode.h>
#else
struct drm_color_ctm {
    uint64_t matrix[9];
};

struct drm_color_lut {
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    uint16_t reserved;
};
#endif

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

/* Rec.709 integer luminance weights scaled by 10000 */
constexpr int64_t REC709_RW = 2126LL;
constexpr int64_t REC709_GW = 7152LL;
constexpr int64_t REC709_BW = 722LL;

inline uint64_t mul_u64_u64_div_u64(uint64_t a, uint64_t b, uint64_t c) {
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b)) / c);
}

inline uint64_t div64_u64(uint64_t a, uint64_t b) {
    return a / b;
}

uint64_t to_s31_32(int64_t num, uint64_t den) {
    uint64_t sign = 0;
    uint64_t abs_num;
    uint64_t mag;

    if (num < 0) {
        sign = 1ULL << 63;
        abs_num = static_cast<uint64_t>(-num);
    } else {
        abs_num = static_cast<uint64_t>(num);
    }

    mag = mul_u64_u64_div_u64(abs_num, 1ULL << 32, den);

    if (mag > ((1ULL << 63) - 1))
        mag = (1ULL << 63) - 1;

    return sign | mag;
}

void chroma_compute_ctm(struct drm_color_ctm* ctm, int sat_val) {
    int64_t sat = static_cast<int64_t>(sat_val);
    int64_t one_minus_s = 100LL - sat;
    int64_t weights[3] = { REC709_RW, REC709_GW, REC709_BW };
    int64_t den = 100LL * 10000LL;

    if (!ctm)
        return;

    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            int64_t w = weights[c];
            int64_t val;

            if (r == c)
                val = one_minus_s * w + sat * 10000LL;
            else
                val = one_minus_s * w;

            ctm->matrix[r * 3 + c] = to_s31_32(val, den);
        }
    }
}

bool chroma_is_identity_ctm(const struct drm_color_ctm* ctm) {
    if (!ctm)
        return true;

    for (int i = 0; i < 9; ++i) {
        uint64_t val = ctm->matrix[i];

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

void chroma_compute_gamma_lut(struct drm_color_lut* lut, size_t size, int con_val, int bri_val) {
    if (!lut || size < 2)
        return;

    int64_t D = static_cast<int64_t>(size - 1);
    int64_t den = 200LL * D;

    for (size_t i = 0; i < size; ++i) {
        int64_t num = (2LL * static_cast<int64_t>(i) - D) * static_cast<int64_t>(con_val) +
                      (100LL + static_cast<int64_t>(bri_val)) * D;
        uint16_t val;

        if (num <= 0) {
            val = 0;
        } else if (num >= den) {
            val = 65535;
        } else {
            uint64_t tmp = static_cast<uint64_t>(num) * 65535ULL + static_cast<uint64_t>(den) / 2ULL;
            val = static_cast<uint16_t>(div64_u64(tmp, static_cast<uint64_t>(den)));
        }

        lut[i].red = val;
        lut[i].green = val;
        lut[i].blue = val;
        lut[i].reserved = 0;
    }
}

bool chroma_is_identity_gamma_lut(const struct drm_color_lut* lut, size_t count) {
    if (!lut || count < 2)
        return true;

    int64_t D = static_cast<int64_t>(count - 1);

    for (size_t i = 0; i < count; ++i) {
        uint64_t tmp = static_cast<uint64_t>(i) * 65535ULL + static_cast<uint64_t>(D) / 2ULL;
        uint16_t expected = static_cast<uint16_t>(div64_u64(tmp, static_cast<uint64_t>(D)));
        int diff_r = static_cast<int>(lut[i].red) - static_cast<int>(expected);
        int diff_g = static_cast<int>(lut[i].green) - static_cast<int>(expected);
        int diff_b = static_cast<int>(lut[i].blue) - static_cast<int>(expected);

        if (diff_r < -1 || diff_r > 1 ||
            diff_g < -1 || diff_g > 1 ||
            diff_b < -1 || diff_b > 1)
            return false;
    }

    return true;
}

} // namespace

int main() {
    std::cout << "[test_color_math] Starting color math verification suite...\n";

    // 1. Fixed point S31.32 conversion
    {
        uint64_t one = to_s31_32(100, 100);
        assert(one == (1ULL << 32) && "to_s31_32(100, 100) must equal 1.0 (1ULL << 32)");

        uint64_t zero = to_s31_32(0, 100);
        assert(zero == 0 && "to_s31_32(0, 100) must equal 0");

        uint64_t neg_one = to_s31_32(-100, 100);
        assert(neg_one == ((1ULL << 63) | (1ULL << 32)) && "to_s31_32(-100, 100) must have sign bit 63 set");

        std::cout << "  [PASS] Fixed point S31.32 conversion\n";
    }

    // 2. CTM Saturation formula
    {
        struct drm_color_ctm ctm{};

        // Saturation 100 (neutral identity)
        chroma_compute_ctm(&ctm, 100);
        assert(chroma_is_identity_ctm(&ctm) && "Saturation 100 must produce identity CTM");
        assert(ctm.matrix[0] == (1ULL << 32));
        assert(ctm.matrix[4] == (1ULL << 32));
        assert(ctm.matrix[8] == (1ULL << 32));
        assert(ctm.matrix[1] == 0);
        assert(ctm.matrix[2] == 0);

        // Saturation 0 (complete grayscale Rec.709)
        chroma_compute_ctm(&ctm, 0);
        assert(!chroma_is_identity_ctm(&ctm) && "Saturation 0 is not identity");
        for (int r = 0; r < 3; ++r) {
            uint64_t r_coeff = (static_cast<uint64_t>(REC709_RW) << 32) / 10000ULL;
            uint64_t g_coeff = (static_cast<uint64_t>(REC709_GW) << 32) / 10000ULL;
            uint64_t b_coeff = (static_cast<uint64_t>(REC709_BW) << 32) / 10000ULL;
            assert(ctm.matrix[r * 3 + 0] == r_coeff);
            assert(ctm.matrix[r * 3 + 1] == g_coeff);
            assert(ctm.matrix[r * 3 + 2] == b_coeff);
        }

        // Saturation 200
        chroma_compute_ctm(&ctm, 200);
        assert(!chroma_is_identity_ctm(&ctm));
        // Off-diagonals must be negative
        assert((ctm.matrix[1] & (1ULL << 63)) != 0 && "Off-diagonal elements must be negative for sat > 100");

        std::cout << "  [PASS] CTM Saturation Rec.709 math\n";
    }

    // 3. GAMMA_LUT identity ramp, contrast pivot, and brightness offset (256 and 4096)
    for (size_t size : {256UL, 4096UL}) {
        std::vector<struct drm_color_lut> lut(size);
        size_t D = size - 1;

        // 3.1 Neutral identity ramp (con = 100, bri = 0)
        chroma_compute_gamma_lut(lut.data(), size, 100, 0);
        assert(lut[0].red == 0 && lut[0].green == 0 && lut[0].blue == 0);
        assert(lut[D].red == 65535 && lut[D].green == 65535 && lut[D].blue == 65535);
        for (size_t i = 1; i < size; ++i) {
            assert(lut[i].red >= lut[i - 1].red && "Identity LUT must be strictly non-decreasing");
            assert(lut[i].red == lut[i].green && lut[i].green == lut[i].blue && "Channels must match");
            assert(lut[i].reserved == 0 && "Reserved must be 0");
        }
        assert(chroma_is_identity_gamma_lut(lut.data(), size) && "Identity LUT must pass chroma_is_identity_gamma_lut");

        // 3.2 Contrast pivot around mid-gray (0.5)
        for (int con : {50, 100, 150, 200, 300}) {
            chroma_compute_gamma_lut(lut.data(), size, con, 0);

            // Verify monotonicity
            for (size_t i = 1; i < size; ++i) {
                assert(lut[i].red >= lut[i - 1].red && "Contrast LUT must be monotonic");
            }

            // Pivot check around mid-gray: for odd D, index mid_lo (D/2) and mid_hi (mid_lo + 1)
            // straddle normalized 0.5 symmetrically. Their average must be 32767/32768 for any contrast.
            size_t mid_lo = D / 2;
            size_t mid_hi = mid_lo + 1;
            int mid_avg = (static_cast<int>(lut[mid_lo].red) + static_cast<int>(lut[mid_hi].red)) / 2;
            assert(std::abs(mid_avg - 32767) <= 1 && "Contrast pivot must average exactly to mid-gray 0.5");
            assert(lut[mid_lo].red <= 32768 && lut[mid_hi].red >= 32767 && "Mid-gray must lie between mid_lo and mid_hi");

            if (con > 100) {
                // Higher contrast has steeper slope: reaches limits earlier
                assert(lut[0].red == 0);
                assert(lut[D].red == 65535);
            } else if (con < 100) {
                // Lower contrast has shallower slope: compressed towards mid-gray
                assert(lut[0].red > 0);
                assert(lut[D].red < 65535);
            }
        }

        // 3.3 Brightness offset limited to +/-50%
        // Max brightness (+100) -> +50% offset
        chroma_compute_gamma_lut(lut.data(), size, 100, 100);
        assert(lut[0].red == 32768 && "Max brightness (+100) must lift black to 50% (32768)");
        assert(lut[D].red == 65535 && "Clamped to 65535 at top");

        // Min brightness (-100) -> -50% offset
        chroma_compute_gamma_lut(lut.data(), size, 100, -100);
        assert(lut[0].red == 0 && "Clamped to 0 at bottom");
        assert((lut[D].red == 32767 || lut[D].red == 32768) && "Min brightness (-100) must lower white to 50%");

        std::cout << "  [PASS] GAMMA_LUT size " << size << " (identity ramp, contrast pivot, brightness offset)\n";
    }

    // 4. Tolerance (+/-1) and rejection of deviating curves
    {
        constexpr size_t size = 256;
        std::vector<struct drm_color_lut> lut(size);
        chroma_compute_gamma_lut(lut.data(), size, 100, 0);

        // Baseline is identity
        assert(chroma_is_identity_gamma_lut(lut.data(), size));

        // Perturb within +/-1 tolerance
        lut[50].red += 1;
        assert(chroma_is_identity_gamma_lut(lut.data(), size) && "Deviation of +1 must be tolerated");
        lut[50].red -= 1; // restore

        lut[100].green -= 1;
        assert(chroma_is_identity_gamma_lut(lut.data(), size) && "Deviation of -1 must be tolerated");
        lut[100].green += 1; // restore

        lut[150].blue += 1;
        assert(chroma_is_identity_gamma_lut(lut.data(), size) && "Deviation of +1 must be tolerated");
        lut[150].blue -= 1; // restore

        // Perturb beyond tolerance (> 1)
        lut[50].red += 2;
        assert(!chroma_is_identity_gamma_lut(lut.data(), size) && "Deviation of +2 must be rejected");
        lut[50].red -= 2;

        lut[100].blue -= 2;
        assert(!chroma_is_identity_gamma_lut(lut.data(), size) && "Deviation of -2 must be rejected");
        lut[100].blue += 2;

        // 5. Night Light curve rejection (attenuated blue channel)
        for (size_t i = 0; i < size; ++i) {
            lut[i].blue = static_cast<uint16_t>((static_cast<uint32_t>(lut[i].blue) * 80) / 100);
        }
        assert(!chroma_is_identity_gamma_lut(lut.data(), size) && "Night Light curve must be rejected as non-identity");

        std::cout << "  [PASS] GAMMA_LUT tolerance (+/-1) and Night Light curve rejection\n";
    }

    std::cout << "[test_color_math] ALL TESTS PASSED.\n";
    return 0;
}
