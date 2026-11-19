# prisma

Controle de cor em hardware (saturação, contraste e brilho) para GPUs AMD (`amdgpu`) no Linux, atuando no desktop inteiro (Wayland/X11) e em jogos com custo zero de performance.

## Como funciona

O módulo de kernel `prisma_amd.ko` (atualmente `chroma_amd.ko`) intercepta `drm_atomic_helper_commit` via `kprobe` e injeta a matriz de transformação de cor Rec.709 (`CTM` 3x3 em ponto fixo S31.32, sem FPU) diretamente no pipeline Display Core Next (DCN) da GPU antes do envio ao monitor.

O repositório também mantém uma Vulkan Implicit Layer (`src/`) para pós-processamento opcional por aplicação.

## Dependências (Arch / CachyOS)

```bash
sudo pacman -S --needed base-devel linux-cachyos-headers clang lld cmake libdrm vulkan-headers vulkan-icd-loader shaderc
```

Em kernels padrão Arch Linux (`linux`), substitua `linux-cachyos-headers` por `linux-headers`.

## Módulo de Kernel (Backend Principal)

### Compilação e carga

```bash
cd kernel
make
sudo make load
```

Para verificar se o módulo e o kprobe foram inicializados com sucesso:

```bash
sudo dmesg | grep chroma_amd
```

### Controle em tempo real (/dev/chroma_amd)

Os parâmetros podem ser alterados instantaneamente via `/dev/chroma_amd` (usuários no grupo `video` não necessitam de `sudo`):

```bash
# Saturação (Digital Vibrance): 100 = padrão, 160 = +60%, 200 = 2x, 0 = monocromático
echo "saturation=160" > /dev/chroma_amd

# Contraste: 100 = padrão, 115 = +15%
echo "contrast=115" > /dev/chroma_amd

# Brilho: 0 = padrão
echo "brightness=0" > /dev/chroma_amd

# Múltiplos parâmetros simultâneos
echo "saturation=150 contrast=110 brightness=5" > /dev/chroma_amd

# Ativar ou desativar a injeção em hardware
echo "enabled=0" > /dev/chroma_amd
echo "enabled=1" > /dev/chroma_amd
```

Para inspecionar o estado atual:

```bash
cat /dev/chroma_amd
# ou via sysfs (somente leitura):
cat /sys/kernel/chroma_amd/saturation
```

### Descarga

```bash
sudo make unload
```

## Vulkan Implicit Layer (Opcional, por aplicação)

Caso deseje aplicar o efeito apenas a um jogo ou aplicativo Vulkan específico:

```bash
cmake -B build
cmake --build build
cmake --build build --target install-local
```

Execução:

```bash
ENABLE_CHROMA_LAYER=1 vkcube
```

A camada lê `/tmp/chroma.conf` a cada quadro apresentado para ajustes em tempo real.

## Estrutura do Repositório

- `kernel/`: Módulo de kernel Linux (`chroma_amd.c`) e Makefile Kbuild.
- `src/`: Vulkan Implicit Layer (`layer.cpp`), shader computacional (`shader.comp`) e protótipo userspace DRM (`drm_hook.cpp`).
- `tests/`: Testes unitários do cálculo CTM e conversão S31.32 (`test_ctm.cpp`).
- `ARCHITECTURE.md`: Documentação técnica sobre a evolução da arquitetura e detalhes de baixo nível.

## Licença

MIT para componentes em userspace e Dual MIT/GPL v2 para o módulo de kernel.
