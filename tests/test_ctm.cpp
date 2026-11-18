#include "drm_ctm.hpp"
#include <iostream>
#include <cassert>
#include <cmath>
#include <iomanip>

using namespace chroma;

void print_matrix(const char* label, const drm_color_ctm& ctm) {
    std::cout << "\n=== " << label << " ===\n";
    for (int r = 0; r < 3; ++r) {
        std::cout << "  ";
        for (int c = 0; c < 3; ++c) {
            uint64_t raw = ctm.matrix[r * 3 + c];
            bool neg = (raw & DRM_CTM_SIGN_BIT) != 0;
            double val = s31_32_to_double(raw);
            std::cout << std::setw(10) << std::fixed << std::setprecision(4) << val
                      << " (raw: " << (neg ? "-" : "+") << "0x"
                      << std::hex << (raw & ~DRM_CTM_SIGN_BIT) << std::dec << ")  ";
        }
        std::cout << "\n";
    }
}

int main() {
    std::cout << "[test_ctm] Iniciando suíte de testes do módulo DRM CTM...\n";

    // Teste 1: Conversão S31.32 básica
    {
        uint64_t one = double_to_s31_32(1.0);
        assert(one == (1ULL << 32) && "1.0 deve ser exatamente 1ULL << 32");

        uint64_t zero = double_to_s31_32(0.0);
        assert(zero == 0ULL && "0.0 deve ser 0ULL");

        uint64_t minus_one = double_to_s31_32(-1.0);
        assert(minus_one == ((1ULL << 63) | (1ULL << 32)) && "-1.0 deve ativar bit 63 com magnitude 1ULL << 32");

        uint64_t neg_val = double_to_s31_32(-0.7152);
        assert((neg_val & DRM_CTM_SIGN_BIT) != 0 && "Valores negativos devem ativar o bit 63");

        // Round-trip precision
        double rt_pos = s31_32_to_double(double_to_s31_32(1.234567));
        assert(std::abs(rt_pos - 1.234567) < 1e-7 && "Precisão do round-trip positivo");

        double rt_neg = s31_32_to_double(double_to_s31_32(-0.456789));
        assert(std::abs(rt_neg - (-0.456789)) < 1e-7 && "Precisão do round-trip negativo");

        std::cout << "  [PASS] Teste 1: Conversão ponto fixo S31.32 e round-trip\n";
    }

    // Teste 2: Saturation = 1.0 (Identidade perfeita)
    {
        drm_color_ctm ctm = calculate_ctm(1.0, 1.0);
        print_matrix("Saturation = 1.0, Contrast = 1.0 (Identidade)", ctm);

        // Diagonal principal deve ser exatamente 1ULL << 32
        assert(ctm.matrix[0] == (1ULL << 32) && "matrix[0] deve ser exatamente 1ULL << 32");
        assert(ctm.matrix[4] == (1ULL << 32) && "matrix[4] deve ser exatamente 1ULL << 32");
        assert(ctm.matrix[8] == (1ULL << 32) && "matrix[8] deve ser exatamente 1ULL << 32");

        // Fora da diagonal deve ser exatamente 0ULL
        assert(ctm.matrix[1] == 0ULL && "matrix[1] deve ser 0");
        assert(ctm.matrix[2] == 0ULL && "matrix[2] deve ser 0");
        assert(ctm.matrix[3] == 0ULL && "matrix[3] deve ser 0");
        assert(ctm.matrix[5] == 0ULL && "matrix[5] deve ser 0");
        assert(ctm.matrix[6] == 0ULL && "matrix[6] deve ser 0");
        assert(ctm.matrix[7] == 0ULL && "matrix[7] deve ser 0");

        std::cout << "  [PASS] Teste 2: Matriz identidade garantida com 1ULL << 32 na diagonal e 0 fora\n";
    }

    // Teste 3: Saturation = 2.0 (Digital Vibrance) - valores negativos ativam bit 63
    {
        drm_color_ctm ctm = calculate_ctm(2.0, 1.0);
        print_matrix("Saturation = 2.0, Contrast = 1.0 (Digital Vibrance 200%)", ctm);

        // Com s = 2.0, (1 - s) = -1.0. Coeficientes fora da diagonal são negativos!
        // M01 = -0.7152, M02 = -0.0722, M10 = -0.2126, M12 = -0.0722, M20 = -0.2126, M21 = -0.7152
        for (int idx : {1, 2, 3, 5, 6, 7}) {
            uint64_t val = ctm.matrix[idx];
            assert((val & DRM_CTM_SIGN_BIT) != 0 && "Elemento fora da diagonal deve ter o bit 63 (sinal) ativado!");
            double d = s31_32_to_double(val);
            assert(d < 0.0 && "Valor convertido deve ser negativo");
        }

        // Elementos da diagonal devem ser positivos e > 1.0
        for (int idx : {0, 4, 8}) {
            uint64_t val = ctm.matrix[idx];
            assert((val & DRM_CTM_SIGN_BIT) == 0 && "Diagonal deve ser positiva");
            assert(val > (1ULL << 32) && "Diagonal deve ser maior que 1.0");
        }

        std::cout << "  [PASS] Teste 3: Saturation = 2.0 ativa corretamente o bit de sinal 1ULL << 63\n";
    }

    // Teste 4: Saturation = 0.0 (Monocromático Rec.709)
    {
        drm_color_ctm ctm = calculate_ctm(0.0, 1.0);
        print_matrix("Saturation = 0.0, Contrast = 1.0 (Escala de cinza Rec.709)", ctm);

        // Todas as 3 linhas devem ser iguais aos pesos Rec.709
        for (int r = 0; r < 3; ++r) {
            double r_w = s31_32_to_double(ctm.matrix[r * 3 + 0]);
            double g_w = s31_32_to_double(ctm.matrix[r * 3 + 1]);
            double b_w = s31_32_to_double(ctm.matrix[r * 3 + 2]);

            assert(std::abs(r_w - LUMA_REC709_R) < 1e-4 && "Peso R deve ser 0.2126");
            assert(std::abs(g_w - LUMA_REC709_G) < 1e-4 && "Peso G deve ser 0.7152");
            assert(std::abs(b_w - LUMA_REC709_B) < 1e-4 && "Peso B deve ser 0.0722");
        }

        std::cout << "  [PASS] Teste 4: Saturation = 0.0 produz luminância pura Rec.709\n";
    }

    // Teste 5: Contraste escalando a matriz
    {
        drm_color_ctm ctm = calculate_ctm(1.0, 1.5);
        print_matrix("Saturation = 1.0, Contrast = 1.5 (Escala de Contraste)", ctm);

        uint64_t expected_diag = double_to_s31_32(1.5);
        assert(ctm.matrix[0] == expected_diag && "matrix[0] deve ser 1.5");
        assert(ctm.matrix[4] == expected_diag && "matrix[4] deve ser 1.5");
        assert(ctm.matrix[8] == expected_diag && "matrix[8] deve ser 1.5");

        assert(ctm.matrix[1] == 0ULL);
        assert(ctm.matrix[2] == 0ULL);
        assert(ctm.matrix[3] == 0ULL);

        std::cout << "  [PASS] Teste 5: Fator de contraste escala a matriz corretamente\n";
    }

    // Teste 6: GAMMA_LUT para brilho e contraste com hardware clipping
    {
        const size_t lut_size = 256;
        // Padrão linear
        auto lut_normal = calculate_gamma_lut(lut_size, 1.0, 0.0);
        assert(lut_normal.size() == lut_size);
        assert(lut_normal[0].red == 0 && "lut[0] deve ser 0 em ramp linear");
        assert(lut_normal[lut_size - 1].red == 65535 && "lut[max] deve ser 65535 em ramp linear");

        // Brilho positivo (+10%)
        auto lut_bright = calculate_gamma_lut(lut_size, 1.0, 0.1);
        assert(lut_bright[0].red > 0 && "Brilho positivo eleva o preto");
        assert(lut_bright[lut_size - 1].red == 65535 && "Brilho positivo sofre hardware clip em 65535");

        // Brilho negativo (-10%)
        auto lut_dark = calculate_gamma_lut(lut_size, 1.0, -0.1);
        assert(lut_dark[0].red == 0 && "Brilho negativo sofre hardware clip em 0");
        assert(lut_dark[lut_size - 1].red < 65535 && "Brilho negativo reduz o branco");

        // Verificação de clipping estrito em valores extremos
        auto lut_extreme = calculate_gamma_lut(lut_size, 3.0, 0.5);
        for (const auto& entry : lut_extreme) {
            assert(entry.red <= 65535 && "Nenhum canal pode exceder 16 bits (65535)");
            assert(entry.green <= 65535);
            assert(entry.blue <= 65535);
        }

        std::cout << "  [PASS] Teste 6: GAMMA_LUT com suporte a contraste, brilho e clipping de hardware\n";
    }

    std::cout << "\n>>> Todos os testes do módulo CTM passaram com 100% de sucesso! <<<\n";
    return 0;
}
