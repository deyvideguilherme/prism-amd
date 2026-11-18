#include "drm_ctm.hpp"
#include <cmath>
#include <algorithm>

namespace chroma {

uint64_t double_to_s31_32(double val) {
    if (std::isnan(val)) {
        return 0ULL;
    }
    uint64_t sign_bit = 0ULL;
    double abs_val = val;
    if (val < 0.0) {
        sign_bit = DRM_CTM_SIGN_BIT;
        abs_val = -val;
    }

    constexpr double scale = 4294967296.0; // 2^32
    double scaled = std::round(abs_val * scale);

    constexpr uint64_t max_mag = 0x7FFFFFFFFFFFFFFFULL;
    uint64_t magnitude = 0;
    if (scaled >= static_cast<double>(max_mag)) {
        magnitude = max_mag;
    } else {
        magnitude = static_cast<uint64_t>(scaled);
    }
    return sign_bit | (magnitude & max_mag);
}

double s31_32_to_double(uint64_t val) {
    bool is_neg = (val & DRM_CTM_SIGN_BIT) != 0;
    uint64_t mag = val & (~DRM_CTM_SIGN_BIT);
    double d = static_cast<double>(mag) / 4294967296.0;
    return is_neg ? -d : d;
}

drm_color_ctm calculate_ctm(double saturation, double contrast) {
    // Matriz de Saturação Rec.709:
    // luminância Y = 0.2126*R + 0.7152*G + 0.0722*B
    // cor de saída = (1 - s) * Y + s * cor_original
    double one_minus_s = 1.0 - saturation;

    double m[9];
    // Linha 0 (R_out)
    m[0] = (one_minus_s * LUMA_REC709_R + saturation) * contrast;
    m[1] = (one_minus_s * LUMA_REC709_G) * contrast;
    m[2] = (one_minus_s * LUMA_REC709_B) * contrast;

    // Linha 1 (G_out)
    m[3] = (one_minus_s * LUMA_REC709_R) * contrast;
    m[4] = (one_minus_s * LUMA_REC709_G + saturation) * contrast;
    m[5] = (one_minus_s * LUMA_REC709_B) * contrast;

    // Linha 2 (B_out)
    m[6] = (one_minus_s * LUMA_REC709_R) * contrast;
    m[7] = (one_minus_s * LUMA_REC709_G) * contrast;
    m[8] = (one_minus_s * LUMA_REC709_B + saturation) * contrast;

    drm_color_ctm ctm{};
    for (int i = 0; i < 9; ++i) {
        ctm.matrix[i] = double_to_s31_32(m[i]);
    }
    return ctm;
}

std::vector<drm_color_lut> calculate_gamma_lut(size_t lut_size, double contrast, double brightness) {
    if (lut_size == 0) {
        return {};
    }
    std::vector<drm_color_lut> lut(lut_size);
    for (size_t i = 0; i < lut_size; ++i) {
        double x = (lut_size > 1) ? (static_cast<double>(i) / static_cast<double>(lut_size - 1)) : 0.5;
        // Ajuste de contraste centralizado em 0.5 e deslocamento de brilho
        double y = (x - 0.5) * contrast + 0.5 + brightness;
        // Clipping de hardware estrito [0.0, 1.0]
        if (y < 0.0) y = 0.0;
        if (y > 1.0) y = 1.0;
        uint16_t val = static_cast<uint16_t>(std::clamp(std::round(y * 65535.0), 0.0, 65535.0));
        lut[i].red = val;
        lut[i].green = val;
        lut[i].blue = val;
        lut[i].reserved = 0;
    }
    return lut;
}

} // namespace chroma
