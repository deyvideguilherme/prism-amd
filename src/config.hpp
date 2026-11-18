#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <sys/stat.h>

struct ChromaPushConstants {
    float saturation{1.0f};  // 1.0 = normal
    float contrast{1.0f};    // 1.0 = normal
    float brightness{0.0f};  // 0.0 = normal
};

class ConfigManager {
public:
    static ConfigManager& instance() {
        static ConfigManager s_instance;
        return s_instance;
    }

    ChromaPushConstants get() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    bool check_and_reload() {
        const char* path = "/tmp/chroma.conf";
        struct stat st{};
        if (stat(path, &st) != 0) {
            // Se o arquivo não existir, cria um template inicial comentado
            if (!m_created_default) {
                m_created_default = true;
                std::ofstream out(path);
                if (out.is_open()) {
                    out << "# =========================================\n"
                        << "# Chroma - Configuração em Tempo Real\n"
                        << "# O arquivo é monitorado via stat/mtime.\n"
                        << "# Edite os valores e salve para atualizar instantaneamente!\n"
                        << "# =========================================\n\n"
                        << "# Saturação (Digital Vibrance): 1.0 = normal, 1.5 = +50%, 2.0 = 2x, 0.0 = P&B\n"
                        << "saturation = 1.0\n\n"
                        << "# Contraste: 1.0 = normal, 1.1 = +10% contraste, 0.9 = -10% contraste\n"
                        << "contrast = 1.0\n\n"
                        << "# Brilho: 0.0 = normal, 0.05 = +5% brilho, -0.05 = -5% brilho\n"
                        << "brightness = 0.0\n";
                    std::cerr << "[chroma] Arquivo de configuração criado em " << path << std::endl;
                }
                if (stat(path, &st) == 0) {
                    m_last_mtime = st.st_mtime;
                    load_from_file(path);
                    return true;
                }
            }
            return false;
        }

        // Verifica se a data de modificação mudou
        if (st.st_mtime == m_last_mtime) {
            return false;
        }

        m_last_mtime = st.st_mtime;
        load_from_file(path);
        return true;
    }

    time_t last_mtime() const {
        return m_last_mtime;
    }

private:
    ConfigManager() = default;

    void load_from_file(const char* path) {
        std::ifstream in(path);
        if (!in.is_open()) return;

        ChromaPushConstants new_cfg = m_config;
        std::string line;

        while (std::getline(in, line)) {
            // Ignora comentários e espaços vazios
            size_t comment = line.find('#');
            if (comment != std::string::npos) {
                line = line.substr(0, comment);
            }
            auto trim = [](std::string& s) {
                while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
                while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            };
            trim(line);
            if (line.empty()) continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string val = line.substr(eq + 1);
            trim(key);
            trim(val);

            try {
                float num = std::stof(val);
                if (key == "saturation") {
                    new_cfg.saturation = num;
                } else if (key == "contrast") {
                    new_cfg.contrast = num;
                } else if (key == "brightness") {
                    new_cfg.brightness = num;
                }
            } catch (...) {
                // Linha com formato inválido ignorada
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_config = new_cfg;
        }

        std::cerr << "[chroma] Config recarregada: saturation=" << new_cfg.saturation
                  << ", contrast=" << new_cfg.contrast
                  << ", brightness=" << new_cfg.brightness << std::endl;
    }

    std::mutex m_mutex;
    ChromaPushConstants m_config;
    time_t m_last_mtime{0};
    bool m_created_default{false};
};
