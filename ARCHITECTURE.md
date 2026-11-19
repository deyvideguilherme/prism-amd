# Arquitetura do Prisma

O **prisma** é uma suíte de controle de cor e exibição em nível de hardware desenvolvida para GPUs AMD Radeon (`amdgpu`) no Linux.

Este documento detalha os fundamentos técnicos, os desafios enfrentados em compositores Wayland modernos e a evolução arquitetural do projeto.

## 1. Visão Geral da Arquitetura

O sistema atua interceptando commits DRM/KMS atômicos diretamente no kernel Linux através de um kprobe:

1. O compositor Wayland (Niri, Hyprland, Sway, GNOME, KDE) ou o servidor X11 prepara a cena e emite uma requisição atômica DRM (`DRM_IOCTL_MODE_ATOMIC`).
2. A requisição atinge `drm_atomic_helper_commit` no subsistema DRM do kernel.
3. O módulo de kernel registra um kprobe nesta função, filtra se o dispositivo gráfico pertence ao driver `amdgpu` e inspeciona o estado do CRTC ativo.
4. Se a injeção estiver habilitada:
   - Para saturação e contraste, calcula a matriz Rec.709 em aritmética inteira de 64 bits (sem FPU) e substitui o blob da propriedade `CTM` (`struct drm_color_ctm`) no estado do CRTC (`new_crtc_state->ctm`).
   - Para brilho, calcula a tabela de correção gama de até 4096 entradas (`struct drm_color_lut`) e substitui o blob da propriedade `GAMMA_LUT` (`new_crtc_state->gamma_lut`).
   - Sinaliza `new_crtc_state->color_mgmt_changed = true`.
5. A função `amdgpu_dm_atomic_commit_tail` recebe o estado alterado e programa diretamente os blocos de hardware Display Core Next (DCN).
6. O hardware da GPU aplica as matrizes e curvas durante a varredura do sinal para o monitor, resultando em latência zero e 0% de overhead sobre CPU ou GPU.

## 2. Evolução Arquitetural

### Fase 1: Camada Vulkan Implícita (Userspace)

A primeira implementação foi desenvolvida como uma *implicit layer* Vulkan (`VK_LAYER_chroma`):
- Intercepta `vkCreateSwapchainKHR` para injetar `VK_IMAGE_USAGE_STORAGE_BIT`.
- Em `vkQueuePresentKHR`, executa um compute shader GLSL/SPIR-V sobre a imagem da swapchain antes da apresentação.
- **Limitações:** Atua apenas sobre aplicações Vulkan nativas. Não abrange o restante da área de trabalho, navegadores, reprodutores de vídeo ou jogos baseados em OpenGL sem wrappers. Além disso, consome ciclos de computação dos compute units da GPU.

### Fase 2: Interceptador Userspace via LD_PRELOAD (`libdrm.so`)

Para atingir todo o desktop sem custo computacional, foi desenvolvido um interceptor de biblioteca compartilhada (`libchroma_drm.so`) projetado para ser injetado no compositor via `LD_PRELOAD`:
- Intercepta chamadas a `drmModeAtomicCommit` e `drmModeSetCrtc`.
- Cria blobs DRM via `drmModeCreatePropertyBlob` e injeta `drmModeAtomicAddProperty` para a propriedade `CTM`.
- **O problema do Rust e syscalls diretas:** Compositores Wayland modernos baseados em Rust e na biblioteca Smithay (como o Niri) utilizam a crate `rustix`. Em vez de carregar dinamicamente a `libdrm.so`, o Smithay invoca a syscall `SYS_ioctl` diretamente no descritor de arquivo DRM. Isso contorna completamente qualquer interceptação baseada em `LD_PRELOAD` de funções userspace. Além disso, instâncias aninhadas de compositores não possuem o status de DRM Master, impossibilitando a manipulação direta do KMS.

### Fase 3: Módulo de Kernel Linux (LKM via kprobe)

A solução definitiva foi mover o ponto de controle para o espaço de kernel (`kernel/chroma_amd.c`):
- Um módulo dinâmico anexa um kprobe ao símbolo `drm_atomic_helper_commit`.
- Como essa função é o ponto central onde todo commit atômico do subsistema DRM converge — independentemente de ter sido disparado por `libdrm.so`, `rustix`, X11 ou qualquer chamada direta de syscall —, a injeção torna-se 100% universal.

## 3. Interface de Controle: miscdevice vs sysfs

### Por que o sysfs mutável falhou com udev

Inicialmente, os parâmetros eram expostos via atributos mutáveis (`0664`) em `/sys/kernel/chroma_amd/`. No entanto, regras `udev` baseadas em `SUBSYSTEM=="module"` sofrem com race conditions: o kernel emite o evento de carga do módulo antes que `kobject_create_and_add` e `sysfs_create_group` terminem de criar os nós de atributos. Scripts shell com `sleep` ou gatilhos manuais no udev mostraram-se frágeis e não determinísticos.

### Padrão miscdevice (`/dev/chroma_amd`)

A solução de nível de produção adota a infraestrutura oficial de drivers do kernel Linux:
- O módulo registra um `miscdevice` (`linux/miscdevice.h`) criando o nó de caractere `/dev/chroma_amd`.
- A regra udev nativa em `devtmpfs` (`KERNEL=="chroma_amd", SUBSYSTEM=="misc", GROUP="video", MODE="0660"`) atribui permissões determinísticas no exato momento da criação do dispositivo sem scripts de shell ou delays.
- A função `chroma_dev_write` recebe comandos de texto atômicos (como `saturation=160 contrast=115 brightness=10`), valida os limites de cada parâmetro e incrementa a geração de configuração (`config_generation`).
- A função `chroma_dev_read` retorna o estado atual via `simple_read_from_buffer` (`cat /dev/chroma_amd`).
- Os nós em `/sys/kernel/chroma_amd/` são mantidos exclusivamente em modo leitura (`__ATTR_RO`, `0444`) para fins de telemetria e depuração.

## 4. Pipeline de Cor em Hardware (AMDGPU DCN)

O pipeline de exibição do AMD Display Core Next (DCN) possui blocos dedicados para transformação de cor durante o scanout:

```
[Frame Buffer] -> [DEGAMMA_LUT] -> [CTM 3x3 Matrix] -> [GAMMA_LUT] -> [Display Interface]
```

### 4.1. Bloco CTM: Saturação Rec.709 e Contraste

O bloco Color Transformation Matrix (CTM) opera multiplicando o vetor RGB por uma matriz $3 \times 3$.

#### Coeficientes de Luminância Rec.709

Para preservar a percepção visual de luminância humana ao modular saturação, são adotados os pesos do padrão ITU-R BT.709:
- Vermelho: 0.2126 (2126 / 10000)
- Verde: 0.7152 (7152 / 10000)
- Azul: 0.0722 (722 / 10000)

A matriz de saturação $3 \times 3$ interpola a matriz de escala de cinza com a matriz identidade e escala com o fator de contraste:

$$M = c \cdot \left[ (1 - s) \begin{bmatrix} 0.2126 & 0.7152 & 0.0722 \\ 0.2126 & 0.7152 & 0.0722 \\ 0.2126 & 0.7152 & 0.0722 \end{bmatrix} + s \begin{bmatrix} 1 & 0 & 0 \\ 0 & 1 & 0 \\ 0 & 0 & 1 \end{bmatrix} \right]$$

#### Formato Sign-Magnitude S31.32

O kernel Linux define que a estrutura `struct drm_color_ctm` armazena cada elemento da matriz em um inteiro de 64 bits (`__u64 matrix[9]`) com formato *sign-magnitude*:
- **Bit 63 (`1ULL << 63`):** Bit de sinal (1 se o valor for estritamente negativo, 0 se positivo ou zero).
- **Bits 0 a 62:** Magnitude absoluta do valor escalada por $2^{32}$ (`1ULL << 32`).

Quando `saturation > 1.0` (Digital Vibrance aumentado), os elementos fora da diagonal principal assumem valores negativos. A função `to_s31_32` calcula o sinal e escala o módulo através de `mul_u64_u64_div_u64` sem usar FPU.

### 4.2. Bloco GAMMA_LUT: Controle de Brilho

O estágio `GAMMA_LUT` aplica uma tabela de consulta pós-CTM com até 4096 entradas de 16 bits (`struct drm_color_lut`):

- **Tamanho dinâmico:** O tamanho da tabela é consultado em `crtc->gamma_size` (tipicamente 4096 no AMDGPU DCN) e limitado a um teto seguro (`MAX_GAMMA_LUT_SIZE = 4096`).
- **Aritmética inteira sem FPU:** Para cada entrada $i \in [0, \text{gamma\_size}-1]$, o valor base linear é calculado via:
  $$\text{base} = \text{div\_u64}(i \cdot 65535, \text{gamma\_size} - 1)$$
- **Offset e Clamping:** O valor de `brightness` ($-100$ a $+100$) é mapeado para um deslocamento de 16 bits ($-16384$ a $+16384$) via `div_s64`. O valor final sofre clamping estrito em $[0, 65535]$, preservando o contraste e evitando estouro de brancos ou lavagem de pretos.
- **Restauração Limpa:** Quando `brightness == 0` ou `enabled == 0`, o módulo substitui `new_crtc_state->gamma_lut` por `NULL`, devolvendo o controle da curva gama nativa ao compositor ou ao monitor.

## 5. Gerenciamento de Estado Atômico

A substituição dos blobs de propriedades no kernel utiliza:
- `drm_property_create_blob(dev, size, data)` para alocar o novo blob no subsistema DRM.
- `drm_property_replace_blob(&new_crtc_state->prop, new_blob)` para transferir a referência de forma segura.
- `drm_property_blob_put(new_blob)` para liberar a referência local.
- `new_crtc_state->color_mgmt_changed = true` para informar ao driver `amdgpu` que os blocos DCN devem ser reprogramados no commit tail sem a necessidade de um modeset destrutivo.
