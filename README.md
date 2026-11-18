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

### Controle em tempo real (sysfs)

Os parâmetros podem ser alterados instantaneamente sem reiniciar a sessão gráfica:

```bash
# Saturação (Digital Vibrance): 100 = padrão, 160 = +60%, 200 = 2x, 0 = monocromático
echo 160 | sudo tee /sys/kernel/chroma_amd/saturation

# Contraste: 100 = padrão, 115 = +15%
echo 115 | sudo tee /sys/kernel/chroma_amd/contrast

# Brilho: 0 = padrão
echo 0 | sudo tee /sys/kernel/chroma_amd/brightness

# Ativar ou desativar a injeção em hardware
echo 0 | sudo tee /sys/kernel/chroma_amd/enabled
echo 1 | sudo tee /sys/kernel/chroma_amd/enabled
```

Para permitir escrita sem necessidade de `sudo`:

```bash
sudo chmod 0666 /sys/kernel/chroma_amd/*
echo 160 > /sys/kernel/chroma_amd/saturation
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
