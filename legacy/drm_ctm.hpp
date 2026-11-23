#pragma once

#include <cstdint>
#include <vector>
#include <xf86drmMode.h>
#include <drm/drm_mode.h>

namespace chroma {

// Coeficientes de Luminância Rec.709
constexpr double LUMA_REC709_R = 0.2126;
constexpr double LUMA_REC709_G = 0.7152;
constexpr double LUMA_REC709_B = 0.0722;

// Máscara do bit de sinal no formato S31.32 sign-magnitude do kernel DRM
constexpr uint64_t DRM_CTM_SIGN_BIT = (1ULL << 63);

/**
 * Converte um valor em ponto flutuante double para o formato S31.32 sign-magnitude
 * exigido pela struct drm_color_ctm do kernel Linux:
 * - Bit 63: bit de sinal (1 = negativo, 0 = positivo/zero)
 * - Bits 0-62: magnitude absoluta multiplicada por 2^32
 */
uint64_t double_to_s31_32(double val);

/**
 * Converte um valor do formato S31.32 sign-magnitude de volta para double.
 */
double s31_32_to_double(uint64_t val);

/**
 * Calcula a Matriz de Transformação de Cor 3x3 compatível com struct drm_color_ctm:
 * - saturation: Fator de saturação Rec.709 (1.0 = normal/identidade, 0.0 = monocromático, >1.0 = vibrance)
 * - contrast: Fator de escala linear aplicado à matriz (1.0 = padrão)
 */
drm_color_ctm calculate_ctm(double saturation, double contrast = 1.0);

/**
 * Calcula a tabela GAMMA_LUT (struct drm_color_lut) correspondente a brilho e contraste
 * com hardware clipping garantido no intervalo dinâmico padrão [0, 65535].
 * - lut_size: Quantidade de entradas da LUT exposta pelo CRTC (GAMMA_LUT_SIZE)
 * - contrast: Contraste centrado em 0.5 (1.0 = padrão)
 * - brightness: Deslocamento de brilho linear (0.0 = padrão)
 */
std::vector<drm_color_lut> calculate_gamma_lut(size_t lut_size, double contrast = 1.0, double brightness = 0.0);

} // namespace chroma
