# prisma

Controle de cor em hardware (saturação, contraste e brilho) para GPUs AMD (`amdgpu`) no Linux, atuando no desktop inteiro (Wayland/X11) e em todos os jogos simultaneamente com custo zero de performance.

O projeto também é referenciado internamente como `prism-amd`.

## Visão Geral

O controle tradicional de vibrance via shaders consome recursos de computação da GPU e introduz latência de quadro. O `prisma` atua no nível mais baixo do subsistema DRM/KMS: ele intercepta as requisições atômicas de exibição e programa os blocos de hardware Display Core Next (DCN) da GPU AMD durante o escaneamento do sinal (scanout).

O sistema é composto por duas partes principais:
1. **Módulo de Kernel Linux (`kernel/chroma_amd.ko`):** Intercepta `drm_atomic_helper_commit` via `kprobe`, injeta a matriz `CTM` Rec.709 (ponto fixo S31.32) e a tabela `GAMMA_LUT` (até 4096 entradas) calculadas inteiramente em inteiros de 64 bits sem FPU. Expõe o dispositivo de caractere `/dev/chroma_amd` (`miscdevice`) e nós de telemetria em `/sys/kernel/chroma_amd/` (`0444`).
2. **Utilitário CLI (`prisma-ctl`):** Ferramenta em C++20 para inspeção de estado, telemetria em tempo real de sensores do driver `amdgpu` / `hwmon` e gerenciamento de perfis persistentes.

Para aprofundamento na arquitetura interna, inventário de arquivos e especificação do contrato JSON, consulte o documento [ARCHITECTURE.md](ARCHITECTURE.md).

## Requisitos de Sistema (Arch Linux / CachyOS)

```bash
sudo pacman -S --needed base-devel linux-cachyos-headers clang lld cmake libdrm vulkan-headers vulkan-icd-loader shaderc
```

Em kernels padrão Arch Linux (`linux`), substitua `linux-cachyos-headers` por `linux-headers`.

## Módulo de Kernel

### 1. Compilação e carga

```bash
cd kernel
make
sudo make load
```

Para verificar se o módulo e o kprobe foram registrados com sucesso no kernel:

```bash
sudo dmesg | grep chroma_amd
```

### 2. Permissões de acesso (udev)

Para permitir leitura e escrita sem privilégios de root (`sudo`), instale a regra udev nativa e garanta que o seu usuário pertença ao grupo `video`:

```bash
cd kernel
sudo make install-udev
sudo usermod -aG video $USER
```

*Nota: Caso o usuário tenha sido adicionado ao grupo `video` nesta etapa, encerre e reinicie a sessão de login para atualizar as permissões do shell.*

### 3. Descarga do módulo

```bash
cd kernel
sudo make unload
```

### 4. Suporte a DKMS

O diretório `kernel/` contém `dkms.conf` para recompilação automática a cada atualização do kernel Linux:

```bash
sudo cp -r kernel /usr/src/prism-amd-0.1.0
sudo dkms add -m prism-amd -v 0.1.0
sudo dkms build -m prism-amd -v 0.1.0
sudo dkms install -m prism-amd -v 0.1.0
```

## Utilitário CLI (prisma-ctl)

O utilitário `prisma-ctl` centraliza a leitura e escrita no dispositivo de hardware, além de coletar métricas da GPU e persistir perfis.

### Compilação e instalação local

```bash
cmake -B build
cmake --build build
cmake --build build --target install-local
```

O comando copia o executável para `~/.local/bin/prisma-ctl`. Certifique-se de que o diretório `~/.local/bin` consta na sua variável de ambiente `$PATH`.

Para desinstalar:

```bash
cmake --build build --target uninstall-local
```

### Comandos da CLI

#### 1. Inspeção de estado e telemetria

```bash
# Exibe estado atual do hardware e métricas da GPU
prisma-ctl status

# Exibe o estado em formato JSON estruturado (contrato oficial)
prisma-ctl status --json

# Exibe apenas os sensores de telemetria da GPU
prisma-ctl telemetry
prisma-ctl telemetry --json
```

#### 2. Ajuste de parâmetros de cor

```bash
# Ajustar saturação e contraste
prisma-ctl set --saturation 160 --contrast 115

# Usando flags curtas
prisma-ctl set -s 180 -c 110 -b 10

# Desativar ou reativar a injeção em hardware
prisma-ctl set --enabled 0
prisma-ctl set --enabled 1
```

Faixas de valores válidos:
- `saturation`: `0` a `400` (padrão `100`; `0` = escala de cinza, `100` = normal, `200` = dobro de saturação)
- `contrast`: `10` a `300` (padrão `100`; `100` = normal, `115` = +15%)
- `brightness`: `-100` a `100` (padrão `0`; ajuste fino da curva gama via `GAMMA_LUT`)
- `enabled`: `0` ou `1` (padrão `1`; restaura as curvas e matrizes nativas do compositor quando `0`)

#### 3. Gerenciamento de Perfis

Os perfis são salvos individualmente em `~/.config/prisma/profiles/<nome>.conf`:

```bash
# Salvar o estado atual sob um nome de perfil
prisma-ctl profile save cs2

# Aplicar um perfil salvo diretamente ao hardware
prisma-ctl profile apply cs2

# Listar todos os perfis existentes
prisma-ctl profile list

# Deletar um perfil salvo
prisma-ctl profile delete cs2

# Restaurar parâmetros padrão (sat=100, con=100, bri=0, en=1)
prisma-ctl profile reset
```

## Controle Direto via Shell

É possível interagir diretamente com o driver sem o `prisma-ctl`:

```bash
# Ajustes pontuais
echo "saturation=160" > /dev/chroma_amd

# Múltiplos parâmetros na mesma linha
echo "saturation=150 contrast=115 brightness=5" > /dev/chroma_amd

# Leitura do estado ativo
cat /dev/chroma_amd

# Leitura individual via nós de telemetria sysfs (somente leitura)
cat /sys/kernel/chroma_amd/saturation
cat /sys/kernel/chroma_amd/contrast
cat /sys/kernel/chroma_amd/brightness
cat /sys/kernel/chroma_amd/enabled
```

## Vulkan Implicit Layer (Opcional, por aplicação)

Caso deseje aplicar saturação pontual a um único jogo Vulkan sem alterar a exibição de todo o monitor:

```bash
cmake -B build
cmake --build build
cmake --build build --target install-local
```

Execução:

```bash
ENABLE_CHROMA_LAYER=1 vkcube
```

A camada lê `/tmp/chroma.conf` a cada quadro apresentado para ajustes em tempo real via push constants e compute shader.

## Detalhes Arquiteturais e Contrato JSON

Para detalhes sobre a matemática de conversão de ponto fixo S31.32, por que `LD_PRELOAD` falha em compositores baseados em Rust/Smithay, inventário completo de código e especificação do contrato JSON para integração com o Noctalia Shell, leia o [ARCHITECTURE.md](ARCHITECTURE.md).

## Licença

MIT para componentes em userspace e Dual MIT/GPL v2 para o módulo de kernel.
