# Arquitetura do Prisma

O **prisma** é uma suíte de controle de cor e exibição em nível de hardware desenvolvida para GPUs AMD Radeon (`amdgpu`) no Linux.

Este documento detalha os fundamentos técnicos, os desafios enfrentados em compositores Wayland modernos e a evolução arquitetural do projeto.

## 1. Visão Geral da Arquitetura

O sistema atua interceptando commits DRM/KMS atômicos diretamente no kernel Linux através de um kprobe:

1. O compositor Wayland (Niri, Hyprland, Sway, GNOME, KDE) ou o servidor X11 prepara a cena e emite uma requisição atômica DRM (`DRM_IOCTL_MODE_ATOMIC`).
2. A requisição atinge `drm_atomic_helper_commit` no subsistema DRM do kernel.
3. O módulo de kernel registra um kprobe nesta função, filtra se o dispositivo gráfico pertence ao driver `amdgpu` e inspeciona o estado do CRTC ativo.
4. Se a injeção estiver habilitada, calcula a matriz Rec.709 em aritmética inteira de 64 bits (sem FPU) e substitui a propriedade `CTM` (`struct drm_color_ctm`) no estado do CRTC (`new_crtc_state->ctm`).
5. A função `amdgpu_dm_atomic_commit_tail` recebe o estado alterado e programa diretamente os blocos de hardware Display Core Next (DCN).
6. O hardware da GPU aplica a matriz de cor durante a varredura do sinal para o monitor, resultando em latência zero e 0% de impacto sobre CPU ou GPU.

## 2. Evolução Arquitetural

### Fase 1: Camada Vulkan Implícita (Userspace)

A primeira implementação foi desenvolvida como uma *implicit layer* Vulkan (`VK_LAYER_chroma`):
- Intercepta `vkCreateSwapchainKHR` para injetar `VK_IMAGE_USAGE_STORAGE_BIT`.
- Em `vkQueuePresentKHR`, executa um compute shader GLSL/SPIR-V sobre a imagem da swapchain antes da apresentação.
- **Limitações:** Atua apenas sobre aplicações Vulkan nativas. Não abrange o restante da área de trabalho, navegadores, reprodutores de vídeo ou jogos baseados em OpenGL sem wrappers. Além disso, consome ciclos de execução dos compute units da GPU.

### Fase 2: Interceptador Userspace via LD_PRELOAD (`libdrm.so`)

Para atingir todo o desktop sem custo computacional, foi desenvolvido um interceptor de biblioteca compartilhada (`libchroma_drm.so`) projetado para ser injetado no compositor via `LD_PRELOAD`:
- Intercepta chamadas a `drmModeAtomicCommit` e `drmModeSetCrtc`.
- Cria blobs DRM via `drmModeCreatePropertyBlob` e injeta `drmModeAtomicAddProperty` para a propriedade `CTM`.
- **O problema do Rust e syscalls diretas:** Compositores Wayland modernos baseados em Rust e na biblioteca Smithay (como o Niri) utilizam a crate `rustix`. Em vez de carregar dinamicamente a `libdrm.so`, o Smithay invoca a syscall `SYS_ioctl` diretamente no descritor de arquivo DRM. Isso contorna completamente qualquer interceptação baseada em `LD_PRELOAD` de funções userspace. Além disso, instâncias aninhadas de compositores não possuem o status de DRM Master, impossibilitando a manipulação direta do KMS.

### Fase 3: Módulo de Kernel Linux (LKM via kprobe)

A solução definitiva foi mover o ponto de controle para o espaço de kernel (`kernel/chroma_amd.c`):
- Um módulo dinâmico anexa um kprobe ao símbolo `drm_atomic_helper_commit`.
- Como essa função é o ponto central onde todo commit atômico do subsistema DRM converge — independentemente de ter sido disparado por `libdrm.so`, `rustix`, X11 ou qualquer chamada direta de syscall —, a injeção torna-se 100% universal.
- O controle é realizado via dispositivo de caractere `/dev/chroma_amd` (`miscdevice`, grupo `video`, modo `0660`), com `/sys/kernel/chroma_amd/` exposto como telemetria em modo somente leitura (`0444`).

## 3. Matemática CTM e Formato S31.32 no Kernel

### Coeficientes de Luminância Rec.709

Para preservar a percepção visual de luminância humana ao modular saturação, são adotados os pesos do padrão ITU-R BT.709:
- Vermelho: 0.2126 (2126 / 10000)
- Verde: 0.7152 (7152 / 10000)
- Azul: 0.0722 (722 / 10000)

A matriz de saturação $3 \times 3$ interpola a matriz de escala de cinza com a matriz identidade:

$$M_{sat} = (1 - s) \begin{bmatrix} 0.2126 & 0.7152 & 0.0722 \\ 0.2126 & 0.7152 & 0.0722 \\ 0.2126 & 0.7152 & 0.0722 \end{bmatrix} + s \begin{bmatrix} 1 & 0 & 0 \\ 0 & 1 & 0 \\ 0 & 0 & 1 \end{bmatrix}$$

### Aritmética Inteira Sem FPU

O contexto de execução de um kprobe em rotinas DRM ocorre no fluxo do kernel onde operações de ponto flutuante (FPU) não estão disponíveis. Toda a computação matricial é realizada em inteiros de 64 bits (`s64` e `u64`), utilizando funções auxiliares seguras como `mul_u64_u64_div_u64` para evitar overflow em multiplicações intermediárias.

### Formato Sign-Magnitude S31.32

O subsistema DRM do kernel Linux define que a estrutura `struct drm_color_ctm` armazena cada elemento da matriz $3 \times 3$ em um inteiro de 64 bits (`__u64 matrix[9]`) com formato *sign-magnitude*:
- **Bit 63 (`1ULL << 63`):** Bit de sinal (1 se o valor for estritamente negativo, 0 se positivo ou zero).
- **Bits 0 a 62:** Magnitude absoluta do valor escalada por $2^{32}$ (`1ULL << 32`).

Quando `saturation > 1.0` (Digital Vibrance aumentado), os elementos fora da diagonal principal assumem valores negativos. A rotina matemática do módulo detecta o sinal negativo, seta o bit 63 e aplica a escala de $2^{32}$ sobre o módulo do número.

## 4. Integração com o Driver AMDGPU (DCN)

No driver `amdgpu`, o pipeline de cor do Display Core Next (DCN) gerencia blocos como DEGAMMA, CTM e GAMMA. Ao substituir o blob da propriedade `CTM` em `new_crtc_state->ctm`, o módulo sinaliza a necessidade de reconfiguração de cor setando a flag `new_crtc_state->color_mgmt_changed = true`.

Isso instrui o driver a reprogramar os registradores de matriz de cor do DCN sem a necessidade de um modeset completo, garantindo transições suaves e instantâneas a cada alteração no sysfs.
