# Arquitetura e Mapa do Sistema (prisma)

O **prisma** (`prism-amd`) é uma suíte de controle de cor e exibição em hardware projetada para GPUs AMD Radeon (`amdgpu`) no Linux.

Este documento consolida a arquitetura em três camadas, o inventário completo de arquivos do repositório, o contrato oficial de integração JSON e o roteiro de evolução do projeto.

---

## 1. Camadas do Sistema

```
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
│                         │   ├─ CTM Rec.709 3x3 (ponto fixo S31.32)     │
│                         │   ├─ GAMMA_LUT (inteiro 64 bits, até 4096)   │
│                         │   ├─ drm_property_replace_blob(...)          │
│                         │   └─ state->color_mgmt_changed = true        │
│                         ▼                                              │
│               amdgpu_dm_atomic_commit_tail                             │
│                         │                                              │
└─────────────────────────┼──────────────────────────────────────────────┘
                          ▼
            Hardware AMD Display Core Next (DCN)
            Bloco de Matriz CTM 3x3 + Bloco de Tabela GAMMA_LUT
                          │
                          ▼
                   Monitor Físico (Scanout)
```

### 1.1. Camada 1: Kernel Space (`kernel/chroma_amd.c`)

O núcleo do sistema opera dentro do subsistema DRM/KMS do kernel Linux através de um módulo dinâmico (LKM):
- **Ponto de Interceptação (`kprobe`):** Anexado ao símbolo `drm_atomic_helper_commit`. Esse ponto é o afunilamento universal de todas as operações atômicas KMS, independente de partirem de compositores Smithay/Rust (como Niri via `rustix` com chamadas `SYS_ioctl` diretas), compositores C/C++ via `libdrm`, ou servidores X11 clássicos.
- **Filtro de Driver:** Verifica se o `struct drm_device` pertence estritamente ao driver `amdgpu` antes de qualquer manipulação de estado.
- **Matriz CTM Rec.709 em S31.32 (Sem FPU):** Modula a saturação combinando os pesos de luminância Rec.709 ($R=0.2126$, $G=0.7152$, $B=0.0722$) com o fator de contraste. Toda a aritmética é estritamente inteira de 64 bits (`s64`/`u64` via `mul_u64_u64_div_u64`), convertida para o formato *sign-magnitude* de 64 bits exigido pelo kernel (`1ULL << 63` para sinal negativo e $2^{32}$ para magnitude).
- **Estágio GAMMA_LUT (Sem FPU):** Gera uma tabela linear compensada de até 4096 entradas de 16 bits (`struct drm_color_lut`) para ajuste fino de brilho (`brightness`) com clamping estrito entre `0` e `65535`, preservando o nível de preto sem lavagem.
- **Dispositivo de Caractere `/dev/chroma_amd`:** Registrado via `miscdevice` (`linux/miscdevice.h`) com permissão `0660` para o grupo `video` configurada via regra nativa devtmpfs no `udev`. Elimina race conditions associadas a permissões em sysfs.
- **Telemetria `/sys/kernel/chroma_amd/`:** Atributos expostos exclusivamente como somente leitura (`0444`, `__ATTR_RO`) para auditoria e depuração sem interferir na mutação de estado.

### 1.2. Camada 2: Userspace CLI & Telemetria (`src/cli/`)

A interface de controle de usuário é construída em C++20 com as seguintes responsabilidades modulares:
- **`device.cpp` / `device.hpp`:** Gerencia a comunicação de baixo nível com `/dev/chroma_amd` via chamadas de sistema POSIX (`open`, `read`, `write`, `close`). Valida intervalos de parâmetros no cliente antes do envio e traduz códigos de erro do sistema (`ENOENT` para módulo descarregado, `EACCES` para permissão do grupo `video`, `EINVAL` para parâmetro rejeitado).
- **`telemetry.cpp` / `telemetry.hpp`:** Realiza a descoberta dinâmica de adaptadores gráficos AMD varrendo `/sys/class/drm/card*/device/vendor` em busca de `0x1002` (sem hardcodar índices como `card0` ou `card1`). Lê dados de carga (`gpu_busy_percent`), memória (`mem_info_vram_*`), frequência ativa de SCLK (`pp_dpm_sclk` procurando a linha com `*`), temperaturas de encapsulamento e hotspot (`temp*_label` e `temp*_input`) e potência em Watts (`power1_average` ou `power1_input`).
- **`profiles.cpp` / `profiles.hpp`:** Implementa armazenamento persistente em arquivos de texto no padrão INI em `~/.config/prisma/profiles/<nome>.conf`. Fornece operações de salvamento, carregamento, listagem, exclusão e restauração de padrões.
- **`main.cpp`:** CLI `prisma-ctl` oferecendo os subcomandos `status`, `set`, `telemetry` e `profile`. Suporta saída em texto legível para humanos e formato JSON estruturado (`--json`).

### 1.3. Camada 3: Vulkan Implicit Layer Opcional (`src/`)

Para casos de uso onde o usuário deseja aplicar saturação ou contraste exclusivamente a um jogo específico em vez de todo o monitor:
- **`layer.cpp`:** Camada implícita Vulkan que intercepta `vkCreateSwapchainKHR` (adicionando `VK_IMAGE_USAGE_STORAGE_BIT`) e `vkQueuePresentKHR`.
- **`shader.comp`:** Compute shader GLSL que processa os pixels da swapchain com parâmetros de vibrance Rec.709 via Push Constants antes da apresentação final. Monitora o arquivo `/tmp/chroma.conf` a cada quadro via `stat`/`mtime`.

---

## 2. Inventário de Arquivos do Repositório

| Arquivo | Responsabilidade Única |
| :--- | :--- |
| `kernel/chroma_amd.c` | Código-fonte do módulo de kernel. Intercepta `drm_atomic_helper_commit` via `kprobe`, calcula CTM/GAMMA_LUT sem FPU e registra `/dev/chroma_amd` e `/sys/kernel/chroma_amd/`. |
| `kernel/Makefile` | Automação de compilação Kbuild com detecção de `LLVM=1` para CachyOS, carga (`load`), descarga (`unload`) e instalação de regras udev (`install-udev`). |
| `kernel/99-chroma-amd.rules` | Regra devtmpfs atribuindo o dispositivo `/dev/chroma_amd` ao grupo `video` com permissões `0660`. |
| `kernel/dkms.conf` | Metadados do DKMS para compilação automática do módulo a cada atualização do kernel Linux. |
| `src/cli/device.hpp` / `device.cpp` | Controlador POSIX I/O para `/dev/chroma_amd`, validação de limites de parâmetros e tratamento detalhado de erros do sistema. |
| `src/cli/telemetry.hpp` / `telemetry.cpp` | Descoberta dinâmica de GPU AMD (`vendor == 0x1002`), leitura de métricas sysfs e hwmon e formatadores texto/JSON. |
| `src/cli/profiles.hpp` / `profiles.cpp` | Serialização, carregamento, listagem e exclusão de perfis INI em `~/.config/prisma/profiles/`. |
| `src/cli/main.cpp` | Ponto de entrada do executável `prisma-ctl`, despachando os comandos `status`, `set`, `telemetry` e `profile`. |
| `src/drm_ctm.hpp` / `drm_ctm.cpp` | Biblioteca userspace de cálculo matemático da matriz CTM Rec.709 e conversão para ponto fixo DRM S31.32. |
| `src/drm_hook.cpp` | Protótipo histórico de biblioteca compartilhada (`libchroma_drm.so`) via `LD_PRELOAD` em `drmModeAtomicCommit`. |
| `src/config.hpp` | Parser leve de configuração INI em userspace (`/tmp/chroma.conf`) utilizado pela camada Vulkan. |
| `src/layer.cpp` | Implementação da Vulkan Implicit Layer (`VK_LAYER_chroma`) para pós-processamento pontual por aplicação. |
| `src/shader.comp` | Compute shader GLSL que aplica transformação de cor Rec.709 nos quadros da swapchain Vulkan. |
| `src/vk_dispatch.hpp` | Gerenciador de dispatch tables e resolução de ponteiros de funções da API Vulkan. |
| `tests/test_ctm.cpp` | Testes unitários de validação matemática do ponto fixo S31.32, Rec.709 e clamping da tabela LUT. |
| `tests/test_cli.cpp` | Testes unitários do parser de dispositivo, validação de limites, telemetria mockada e gerenciador de perfis. |
| `CMakeLists.txt` | Configuração principal do CMake para os binários da CLI, bibliotecas compartilhadas, testes e alvos de instalação. |
| `chroma.conf.example` | Arquivo de exemplo com valores recomendados de saturação, contraste e brilho para a camada Vulkan. |
| `VkLayer_chroma.json.in` | Template do manifesto de configuração para registro da camada implícita Vulkan no sistema. |
| `cmake/bin2c.cmake` | Script utilitário do CMake para converter o binário SPIR-V em vetor estático de bytes em C++. |

---

## 3. Contrato Oficial de Integração (`prisma-ctl status --json`)

O utilitário `prisma-ctl` foi projetado para servir como backend de controle único e não bloqueante para interfaces gráficas (como o painel do Noctalia Shell) e serviços de automação (daemons de perfil por jogo).

### 3.1. Esquema JSON de Saída

O comando `prisma-ctl status --json` emite um objeto JSON no stdout com a seguinte estrutura:

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

### 3.2. Especificação dos Campos

#### Objeto `device`

| Campo | Tipo | Faixa Válida | Padrão | Unidade / Significado |
| :--- | :--- | :--- | :--- | :--- |
| `enabled` | Inteiro | `0` ou `1` | `1` | Estado do módulo: `1` ativa injeção da CTM/GAMMA_LUT; `0` desativa e restaura valores nativos. |
| `saturation` | Inteiro | `0` a `400` | `100` | Fator de saturação Rec.709. `100` = saturação normal (1.0x); `200` = dobro (2.0x); `0` = monocromático. |
| `contrast` | Inteiro | `10` a `300` | `100` | Fator de contraste linear. `100` = normal; `115` = +15%; `90` = -10%. |
| `brightness` | Inteiro | `-100` a `100` | `0` | Deslocamento da curva linear na `GAMMA_LUT`. Mapeado internamente para $\pm 16384$ na escala de 16 bits. |

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

### 3.3. Comandos de Mutação para Consumidores Externos

Qualquer interface gráfica ou serviço em segundo plano pode aplicar alterações pontuais invocando `prisma-ctl`:

```bash
# Aplicação atômica de múltiplos parâmetros:
prisma-ctl set --saturation 160 --contrast 115 --brightness 5

# Aplicação de perfil pré-configurado:
prisma-ctl profile apply cs2
```

Códigos de saída:
- `0`: Operação concluída com sucesso.
- `1`: Erro de permissão, dispositivo inexistente ou parâmetro inválido (mensagem descritiva emitida no `stderr`).

---

## 4. Roteiro de Evolução

```
[ CONCLUÍDO ]                          [ PRÓXIMO PASSO ]                    [ FUTURO ]
┌─────────────────────────┐            ┌───────────────────────┐            ┌────────────────────────┐
│ Camada 1: Kernel LKM    │            │ Painel Visual         │            │ Daemon de Perfis       │
│ - kprobe em commit DRM  │ ─────────► │ - Widget no Noctalia  │ ─────────► │ - Detecção de janela   │
│ - CTM & GAMMA_LUT DCN   │            │ - Consumo de JSON     │            │   ativa no Wayland     │
│ - /dev/chroma_amd       │            │ - Sliders em tempo    │            │ - Chaveamento auto     │
│ Camada 2: CLI C++20     │            │   real de vibrance    │            │   de perfis por jogo   │
│ - prisma-ctl & perfis   │            │                       │            │                        │
└─────────────────────────┘            └───────────────────────┘            └────────────────────────┘
```

### 4.1. Status Atual (Concluído)
- Módulo de kernel Linux (`kernel/chroma_amd.ko`) totalmente funcional para hardware AMD Display Core Next com suporte a saturação, contraste e brilho real.
- Interface `/dev/chroma_amd` protegida via udev com acesso rootless para o grupo `video`.
- Ferramenta CLI `prisma-ctl` desenvolvida em C++20 com detecção dinâmica de GPU AMD, sensores de telemetria, gerenciador de perfis e contrato de saída JSON.
- Cobertura de testes unitários em CTest com 100% de sucesso.

### 4.2. Próximas Frentes Planejadas
1. **Painel Visual no Noctalia Shell:**
   - Criação de uma interface visual nativa para o desktop baseada no Noctalia Shell.
   - Leitura de métricas em tempo real consumindo `prisma-ctl status --json` periodicamente sem travar a interface.
   - Controles visuais (sliders de saturação, contraste, brilho e seletor de perfis) despachando comandos diretos para o `prisma-ctl`.
2. **Daemon de Perfis Automáticos por Jogo:**
   - Serviço em segundo plano que monitora a janela em foco através do protocolo Wayland (ou rastreamento de processos).
   - Aplicação automática de perfis pré-configurados ao iniciar títulos específicos (ex: `cs2`, `apex`, `valorant`) e restauração automática do perfil padrão de desktop ao sair.
