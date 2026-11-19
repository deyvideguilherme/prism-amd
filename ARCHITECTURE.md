# Arquitetura e Mapa do Sistema (prisma)

O **Prisma Control Center** (`chroma-layer` / `prism-amd`) é uma suíte integrada de controle de cor e exibição em hardware projetada para GPUs AMD Radeon (`amdgpu`) no Linux, com foco no ambiente **Noctalia Shell (Niri / Wayland)**.

Este documento detalha as três camadas do sistema, o inventário completo de arquivos do repositório, o contrato oficial de integração JSON e o roteiro de evolução técnica.

---

## 1. Camadas do Sistema

```
┌────────────────────────────────────────────────────────────────────────┐
│                        Camada 3: UI (Noctalia Shell)                   │
│                       Plugin: deyvideguilherme/prisma                  │
│   ┌─────────────────────┬───────────────────────┬──────────────────┐   │
│   │ prisma_widget.luau  │   prisma_panel.luau   │prisma_service.luau│  │
│   │  Indicador na Barra │  Painel Desempenho /  │ Polling assíncrono│  │
│   │  (carga / temp)     │  Tela & Cor / Perfis  │ e despacho de cmd │  │
│   └─────────────────────┴───────────┬───────────┴──────────────────┘   │
└─────────────────────────────────────┼──────────────────────────────────┘
                                      │ Invocação assíncrona de CLI:
                                      │ - prisma-ctl status --json
                                      │ - prisma-ctl set ...
                                      │ - prisma-ctl profile ...
                                      ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        Camada 2: Userspace CLI                         │
│                              prisma-ctl                                │
│   ┌─────────────────────┬───────────────────────┬──────────────────┐   │
│   │     device.cpp      │     telemetry.cpp     │   profiles.cpp   │   │
│   │  /dev/chroma_amd    │  sysfs / hwmon (AMD)  │ ~/.config/prisma │   │
│   └──────────┬──────────┴───────────────────────┴──────────────────┘   │
└──────────────┼─────────────────────────────────────────────────────────┘
               │ Escrita: "saturation=160 contrast=115 brightness=10"
               │ Leitura: "enabled=1\nsaturation=160\n..."
               ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        Camada 1: Kernel Space                          │
│                           chroma_amd.ko                                │
│                                                                        │
│   Compositor Wayland (Niri, Hyprland, Sway, KDE, GNOME) ou X11        │
│                         │                                              │
│                         ▼ SYS_ioctl (DRM_IOCTL_MODE_ATOMIC)            │
│          drm_atomic_helper_commit(...)                                 │
│                         │                                              │
│                         ├─► [ kprobe: chroma_amd.c ]                   │
│                         │   ├─ Filtro de driver: amdgpu                │
│                         │   ├─ CTM Linear 3x3 (Saturação/Contraste/    │
│                         │   │   Brilho linear em ponto fixo S31.32)    │
│                         │   ├─ Preservação da curva sRGB nativa        │
│                         │   │  (gamma_lut = NULL / intocada)           │
│                         │   ├─ drm_property_replace_blob(...)          │
│                         │   └─ state->color_mgmt_changed = true        │
│                         ▼                                              │
│               amdgpu_dm_atomic_commit_tail                             │
│                         │                                              │
└─────────────────────────┼──────────────────────────────────────────────┘
                          ▼
            Hardware AMD Display Core Next (DCN)
            Bloco de Matriz CTM 3x3 -> Regamma sRGB Nativo
                          │
                          ▼
                    Monitor Físico (Scanout)
```

### 1.1. Camada 1: Kernel Space (`kernel/chroma_amd.c`)

O núcleo do sistema opera dentro do subsistema DRM/KMS do kernel Linux através de um módulo dinâmico (LKM):
- **Ponto de Interceptação (`kprobe`):** Anexado ao símbolo `drm_atomic_helper_commit`. Esse ponto é o afunilamento universal de todas as operações atômicas KMS, independente de partirem de compositores Smithay/Rust (como Niri via `rustix` com chamadas `SYS_ioctl` diretas), compositores C/C++ via `libdrm`, ou servidores X11 clássicos.
- **Filtro de Driver:** Verifica se o `struct drm_device` pertence estritamente ao driver `amdgpu` antes de qualquer manipulação de estado.
- **Matriz CTM Linear 3x3 em S31.32 (Sem FPU):** Modula saturação (pesos Rec.709 $R=0.2126$, $G=0.7152$, $B=0.0722$), contraste e ganho proporcional de brilho (`gain_q16 = 65536 + div_s64(brightness * 49152, 100)` de 0.25x a 1.75x) em uma única transformação linear. Toda a aritmética é estritamente inteira de 64 bits (`s64`/`u64` via `mul_u64_u64_div_u64`), convertida para o formato *sign-magnitude* de 64 bits exigido pelo kernel DRM (`1ULL << 63` para sinal negativo e $2^{32}$ para magnitude).
- **Preservação da Curva Regamma sRGB Nativa:** Ao aplicar o ganho de brilho de forma linear multiplicativa na matriz CTM, a propriedade `gamma_lut` do DRM é mantida intocada (`NULL`). Isso preserva o bloco Regamma de hardware nativo do painel, garantindo pretos puros e eliminando distorções de gama ou lavagem de sombras.
- **Detecção de Identidade (`chroma_is_identity`):** Quando saturação, contraste e brilho retornam aos valores neutros (`100`, `100`, `0`) ou o módulo é desativado (`enabled=0`), a CTM customizada é restaurada para `NULL`, retornando a GPU ao bypass de hardware sem custo de processamento.
- **Dispositivo de Caractere `/dev/chroma_amd`:** Registrado via `miscdevice` (`linux/miscdevice.h`) com permissão `0660` para o grupo `video` configurada via regra nativa devtmpfs no `udev`. Elimina race conditions associadas a permissões em sysfs.
- **Telemetria `/sys/kernel/chroma_amd/`:** Atributos expostos exclusivamente como somente leitura (`0444`, `__ATTR_RO`) para auditoria e depuração sem interferir na mutação de estado.

### 1.2. Camada 2: Userspace CLI & Telemetria (`src/cli/`)

A interface de controle de usuário é construída em C++20 com responsabilidades modulares:
- **`device.cpp` / `device.hpp`:** Gerencia a comunicação de baixo nível com `/dev/chroma_amd` via chamadas de sistema POSIX (`open`, `read`, `write`, `close`). Valida intervalos de parâmetros no cliente antes do envio e traduz códigos de erro do sistema (`ENOENT` para módulo descarregado, `EACCES` para permissão do grupo `video`, `EINVAL` para parâmetro rejeitado).
- **`telemetry.cpp` / `telemetry.hpp`:** Realiza a descoberta dinâmica de adaptadores gráficos AMD varrendo `/sys/class/drm/card*/device/vendor` em busca de `0x1002` (sem hardcodar índices como `card0` ou `card1`). Lê dados de carga (`gpu_busy_percent`), memória (`mem_info_vram_*`), frequência ativa de SCLK (`pp_dpm_sclk` procurando a linha com `*`), temperaturas de encapsulamento e hotspot (`temp*_label` e `temp*_input`) e potência em Watts (`power1_average` ou `power1_input`).
- **`profiles.cpp` / `profiles.hpp`:** Implementa armazenamento persistente em arquivos de texto no padrão INI em `~/.config/prisma/profiles/<nome>.conf`. Fornece operações de salvamento, carregamento, listagem, exclusão e restauração de padrões.
- **`main.cpp`:** CLI `prisma-ctl` oferecendo os subcomandos `status`, `set`, `telemetry` e `profile`. Suporta saída em texto legível para humanos e formato JSON estruturado (`--json`).

### 1.3. Camada 3: Plugin Noctalia Shell (`ui/prisma/`)

Interface gráfica oficial integrada ao Noctalia Shell no compositor Niri:
- **`plugin.toml`:** Manifesto do plugin com metadados, configurações do usuário (métrica da barra e ocultação quando inativo), declaração do serviço em background, do widget de barra e do painel flutuante.
- **`prisma_service.luau`:** Serviço persistente que monitora o estado do hardware e despacha comandos assíncronos (`prisma-ctl status --json`, `prisma-ctl set`, `prisma-ctl profile`). Implementa pooling adaptativo (1000ms com painel aberto, 2000ms em repouso) e debounce de 40ms para comandos interativos.
- **`prisma_widget.luau`:** Widget exibido na barra do Noctalia Shell com o ícone de paleta, métrica selecionada (Hotspot, Borda ou Carga) e tooltip com resumo de sensores.
- **`prisma_panel.luau`:** Central de controle interativa estruturada em 3 abas:
  - **Desempenho:** Indicadores proporcionais de carga da GPU, clock SCLK, temperatura de silício/borda, consumo elétrico e alocação de VRAM, além de identificador de hardware.
  - **Tela & Cor:** Chave mestre de calibragem, trackbars interativas de 25 segmentos clicáveis para Saturação, Contraste e Brilho, campo de entrada numérica direta (`width = 64`), botões de ajuste fino (`-1`/`+1`), restauração individual e presets rápidos (Padrão, Competitivo, Cinema e Monocromático).
  - **Perfis:** Formulário de salvamento do estado atual, listagem de perfis salvos com ações de aplicar/remover e restauração aos padrões de fábrica.

---

## 2. Inventário de Arquivos do Repositório

| Arquivo / Diretório | Responsabilidade Única |
| :--- | :--- |
| `kernel/chroma_amd.c` | Código-fonte do módulo de kernel. Intercepta `drm_atomic_helper_commit` via `kprobe`, calcula matriz linear CTM Rec.709 3x3 e registra `/dev/chroma_amd` e `/sys/kernel/chroma_amd/`. |
| `kernel/Makefile` | Automação de compilação Kbuild com detecção de `LLVM=1` para CachyOS, carga (`load`), descarga (`unload`) e instalação de regras udev (`install-udev`). |
| `kernel/99-chroma-amd.rules` | Regra devtmpfs atribuindo o dispositivo `/dev/chroma_amd` ao grupo `video` com permissões `0660`. |
| `kernel/dkms.conf` | Metadados do DKMS para compilação automática do módulo a cada atualização do kernel Linux. |
| `src/cli/device.hpp` / `device.cpp` | Controlador POSIX I/O para `/dev/chroma_amd`, validação de limites de parâmetros e tratamento detalhado de erros do sistema. |
| `src/cli/telemetry.hpp` / `telemetry.cpp` | Descoberta dinâmica de GPU AMD (`vendor == 0x1002`), leitura de métricas sysfs e hwmon e formatadores texto/JSON. |
| `src/cli/profiles.hpp` / `profiles.cpp` | Serialização, carregamento, listagem e exclusão de perfis INI em `~/.config/prisma/profiles/`. |
| `src/cli/main.cpp` | Ponto de entrada do executável `prisma-ctl`, despachando os comandos `status`, `set`, `telemetry` e `profile`. |
| `src/drm_ctm.hpp` / `drm_ctm.cpp` | Biblioteca userspace de cálculo matemático da matriz CTM Rec.709 e conversão para ponto fixo DRM S31.32. |
| `tests/test_ctm.cpp` | Testes unitários de validação matemática do ponto fixo S31.32 e saturação Rec.709. |
| `tests/test_cli.cpp` | Testes unitários do parser de dispositivo, validação de limites, telemetria mockada e gerenciador de perfis. |
| `ui/prisma/plugin.toml` | Manifesto do plugin oficial para Noctalia Shell. |
| `ui/prisma/prisma_service.luau` | Daemon de integração Luau para consulta de status e despacho de comandos. |
| `ui/prisma/prisma_widget.luau` | Indicador visual para a barra do Noctalia Shell. |
| `ui/prisma/prisma_panel.luau` | Painel gráfico completo do Prisma Control Center em Luau nativo. |
| `ui/prisma/translations/` | Arquivos de localização (pt-BR e en) para o plugin do Noctalia. |
| `CMakeLists.txt` | Configuração principal do CMake para os binários da CLI, testes unitários e alvos de instalação (`install-local`, `install-ui`). |

---

## 3. Contrato Oficial de Integração (`prisma-ctl status --json`)

O comando `prisma-ctl status --json` emite um objeto JSON no stdout com a seguinte estrutura padronizada:

```json
{
  "device": {
    "enabled": 1,
    "saturation": 160,
    "contrast": 115,
    "brightness": 10
  },
  "telemetry": {
    "card": "card1",
    "pci_id": "0000:2d:00.0",
    "device_id": "1002:7590",
    "gpu_busy_percent": 7,
    "sclk_mhz": 964,
    "vram_used_mib": 1304.5,
    "vram_total_mib": 16304.0,
    "temp_edge_c": 41.0,
    "temp_junction_c": 43.0,
    "power_watts": 22.0
  }
}
```

### 3.1. Especificação dos Campos

#### Objeto `device`

| Campo | Tipo | Faixa Válida | Padrão | Unidade / Significado |
| :--- | :--- | :--- | :--- | :--- |
| `enabled` | Inteiro | `0` ou `1` | `1` | Estado do módulo: `1` ativa injeção da CTM linear; `0` desativa e restaura bypass nativo. |
| `saturation` | Inteiro | `0` a `400` | `100` | Fator de saturação Rec.709. `100` = saturação normal (1.0x); `200` = dobro (2.0x); `0` = monocromático. |
| `contrast` | Inteiro | `10` a `300` | `100` | Fator de contraste linear. `100` = normal; `115` = +15%; `90` = -10%. |
| `brightness` | Inteiro | `-100` a `100` | `0` | Ganho de luminância linear proporcional na CTM (0.25x a 1.75x). `0` = ganho 1.0x (neutro). |

#### Objeto `telemetry`

| Campo | Tipo | Formato / Valores | Significado |
| :--- | :--- | :--- | :--- |
| `card` | String \| null | `"card0"`, `"card1"` | Nome do nó do dispositivo no DRM (`/sys/class/drm/`). |
| `pci_id` | String \| null | `"0000:2d:00.0"` | Identificador do slot PCI no barramento do sistema. |
| `device_id` | String \| null | `"1002:7590"` | Identificador de fornecedor e dispositivo PCI da GPU AMD. |
| `gpu_busy_percent` | Inteiro \| null | `0` a `100` | Porcentagem instantânea de utilização dos compute engines da GPU. |
| `sclk_mhz` | Inteiro \| null | Inteiro em MHz | Frequência de clock ativa do chip gráfico (extraída de `pp_dpm_sclk`). |
| `vram_used_mib` | Float \| null | Flutuante (MiB) | Quantidade de memória VRAM atualmente alocada pelo driver. |
| `vram_total_mib` | Float \| null | Flutuante (MiB) | Capacidade total de memória VRAM física disponível na GPU. |
| `temp_edge_c` | Float \| null | Flutuante (°C) | Temperatura do sensor de borda (`edge`) do encapsulamento da GPU. |
| `temp_junction_c` | Float \| null | Flutuante (°C) | Temperatura do ponto mais quente da matriz de silício (`junction` / Hotspot). |
| `power_watts` | Float \| null | Flutuante (W) | Consumo elétrico instantâneo em Watts reportado pelo sensor do regulador. |

*Regra de Robustez:* Sempre que um sensor não estiver disponível no hardware ou a versão do driver não expuser o arquivo correspondente, o valor será retornado como `null` literal. Consumidores do JSON nunca devem assumir a presença obrigatória de sensores específicos além de `card`.

---

## 4. Roteiro de Evolução

```
[ CONCLUÍDO ]                          [ CONCLUÍDO ]                        [ FUTURO ]
┌─────────────────────────┐            ┌───────────────────────┐            ┌────────────────────────┐
│ Camada 1: Kernel LKM    │            │ Camada 3: Noctalia UI │            │ Daemon de Perfis       │
│ - kprobe em commit DRM  │ ─────────► │ - Widget na barra     │ ─────────► │ - Detecção de janela   │
│ - CTM Linear 3x3        │            │ - Central de controle │            │   ativa no Wayland     │
│ - /dev/chroma_amd       │            │ - Sliders interativos │            │ - Chaveamento auto     │
│ Camada 2: CLI C++20     │            │ - Entrada direta      │            │   de perfis por jogo   │
│ - prisma-ctl & perfis   │            │ - Gestão de perfis    │            │                        │
└─────────────────────────┘            └───────────────────────┘            └────────────────────────┘
```

### 4.1. Funcionalidades Entregues
- Módulo de kernel Linux (`kernel/chroma_amd.ko`) para hardware AMD Display Core Next com suporte a saturação, contraste e brilho linear via matriz CTM 3x3.
- Preservação da curva de transferência sRGB de hardware nativa (sem injeção destrutiva de `GAMMA_LUT`).
- Interface `/dev/chroma_amd` protegida via udev com acesso rootless para o grupo `video`.
- Ferramenta CLI `prisma-ctl` desenvolvida em C++20 com detecção dinâmica de GPU AMD, sensores de telemetria, gerenciador de perfis e contrato de saída JSON.
- Plugin nativo para o Noctalia Shell com widget de barra, central de controle completa com 3 abas, trackbars segmentadas de 25 passos e suporte a localização.
- Cobertura de testes unitários com aprovação em 100% dos testes.

### 4.2. Próximas Frentes
- **Daemon de Perfis Automáticos por Jogo:**
  - Serviço em segundo plano que monitora a janela em foco através do protocolo Wayland (ou rastreamento de processos).
  - Aplicação automática de perfis pré-configurados ao iniciar títulos específicos (ex: `cs2`, `apex`, `valorant`) e restauração automática do perfil padrão de desktop ao encerrar a aplicação.
