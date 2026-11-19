# prisma

Controle de cor em hardware (saturação, contraste e brilho) para GPUs AMD (`amdgpu`) no Linux, atuando no desktop inteiro (Wayland/X11) e em jogos com custo zero de performance.

## Como funciona

O módulo de kernel `chroma_amd.ko` intercepta `drm_atomic_helper_commit` via `kprobe` e injeta a matriz de transformação de cor Rec.709 (`CTM` 3x3 em ponto fixo S31.32) e a tabela de correção gama (`GAMMA_LUT` de até 4096 entradas) diretamente no pipeline Display Core Next (DCN) da GPU antes do envio ao monitor.

A mutação de estado é realizada através do dispositivo `/dev/chroma_amd` (`miscdevice`) ou pela CLI `prisma-ctl`, com permissão de acesso ao grupo `video`. O diretório `/sys/kernel/chroma_amd/` permanece disponível como telemetria em modo somente leitura (`0444`).

O repositório também mantém uma Vulkan Implicit Layer (`src/`) para pós-processamento opcional por aplicação via shader computacional.

## Dependências (Arch / CachyOS)

```bash
sudo pacman -S --needed base-devel linux-cachyos-headers clang lld cmake libdrm vulkan-headers vulkan-icd-loader shaderc
```

Em kernels padrão Arch Linux (`linux`), substitua `linux-cachyos-headers` por `linux-headers`.

## Módulo de Kernel (Backend Principal)

### 1. Compilação e carga

```bash
cd kernel
make
sudo make load
```

Para verificar se o módulo e o kprobe foram inicializados com sucesso:

```bash
sudo dmesg | grep chroma_amd
```

### 2. Configuração de permissões (udev)

Para permitir leitura e escrita sem `sudo`, instale a regra udev e garanta que seu usuário pertença ao grupo `video`:

```bash
cd kernel
sudo make install-udev
sudo usermod -aG video $USER
```

*Nota: Se o seu usuário acabou de ser adicionado ao grupo `video`, faça logout e login novamente para aplicar a alteração de grupo.*

### 3. Descarga do módulo

```bash
cd kernel
sudo make unload
```

### 4. Suporte a DKMS

O diretório `kernel/` inclui `dkms.conf` para recompilação automática a cada atualização de kernel.

Para registrar o módulo no DKMS do sistema:

```bash
sudo cp -r kernel /usr/src/prism-amd-0.1.0
sudo dkms add -m prism-amd -v 0.1.0
sudo dkms build -m prism-amd -v 0.1.0
sudo dkms install -m prism-amd -v 0.1.0
```

## Utilitário CLI (prisma-ctl)

O repositório inclui a CLI `prisma-ctl` (C++20) para controle de parâmetros, telemetria de hardware AMD e gerenciamento de perfis.

### Compilação e instalação

```bash
cmake -B build
cmake --build build
cmake --build build --target install-local
```

O binário é instalado em `~/.local/bin/prisma-ctl`. Certifique-se de que `~/.local/bin` está no seu `PATH`.

### Comandos principais

```bash
# Exibir estado atual e telemetria da GPU AMD
prisma-ctl status
prisma-ctl status --json

# Exibir apenas sensores de telemetria da GPU
prisma-ctl telemetry
prisma-ctl telemetry --json

# Ajustar parâmetros em hardware
prisma-ctl set --saturation 160 --contrast 115
prisma-ctl set -s 180 -c 110 -b 5
prisma-ctl set --enabled 0
prisma-ctl set --enabled 1
```

#### Faixa de valores suportados

- `saturation`: `0` a `400` (padrão `100`; `0` = escala de cinza, `100` = normal, `200` = 2x saturação)
- `contrast`: `10` a `300` (padrão `100`; `100` = normal, `115` = +15%)
- `brightness`: `-100` a `100` (padrão `0`; escala de compensação via `GAMMA_LUT`)
- `enabled`: `0` ou `1` (padrão `1`; restaura as propriedades nativas quando `0`)

### Gerenciamento de Perfis

Os perfis são salvos individualmente em `~/.config/prisma/profiles/<nome>.conf`:

```bash
# Salvar o estado atual em um perfil
prisma-ctl profile save cs2

# Aplicar um perfil salvo ao hardware
prisma-ctl profile apply cs2

# Listar todos os perfis salvos
prisma-ctl profile list

# Deletar um perfil
prisma-ctl profile delete cs2

# Restaurar valores padrão de hardware
prisma-ctl profile reset
```

Formato do arquivo `.conf`:

```ini
# Prisma Color Profile: cs2
saturation=160
contrast=115
brightness=10
enabled=1
```

## Controle direto via /dev/chroma_amd

Além do utilitário `prisma-ctl`, comandos diretos de shell podem ser enviados ao dispositivo:

```bash
echo "saturation=160" > /dev/chroma_amd
echo "saturation=150 contrast=115 brightness=5" > /dev/chroma_amd
cat /dev/chroma_amd
```

Telemetria em modo somente leitura via sysfs:

```bash
cat /sys/kernel/chroma_amd/saturation
cat /sys/kernel/chroma_amd/contrast
cat /sys/kernel/chroma_amd/brightness
cat /sys/kernel/chroma_amd/enabled
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

- `kernel/`: Módulo de kernel Linux (`chroma_amd.c`), Makefile Kbuild, regra udev (`99-chroma-amd.rules`) e configuração DKMS (`dkms.conf`).
- `src/cli/`: Utilitário `prisma-ctl`, controlador de dispositivo (`device.cpp`), telemetria AMD (`telemetry.cpp`) e perfis (`profiles.cpp`).
- `src/`: Vulkan Implicit Layer (`layer.cpp`), shader computacional (`shader.comp`) e protótipo userspace DRM (`drm_hook.cpp`).
- `tests/`: Suíte de testes unitários (`test_ctm.cpp` e `test_cli.cpp`).
- `ARCHITECTURE.md`: Documentação técnica sobre a evolução da arquitetura e pipeline DCN.

## Licença

MIT para componentes em userspace e Dual MIT/GPL v2 para o módulo de kernel.
