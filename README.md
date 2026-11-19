# Prisma Control Center

Calibragem de cor em hardware e monitoramento de sensores para placas de vídeo AMD no Noctalia Shell (Niri / Wayland).

## 1. Visão Geral e Arquitetura

O Prisma Control Center (`chroma-layer`) é uma solução integrada para ajuste de exibição em hardware e telemetria de GPUs AMD (`amdgpu`) no Linux. Em vez de utilizar shaders ou pós-processamento na composição gráfica, o projeto atua diretamente na saída de vídeo do Display Core Next (DCN) via DRM/KMS, aplicando saturação, contraste e ganho de brilho na matriz linear 3x3 com custo computacional nulo para a CPU e GPU.

A suíte é composta por três componentes:

- **Módulo de Kernel (`kernel/chroma_amd.ko`):** Módulo dinâmico que intercepta requisições atômicas KMS (`drm_atomic_helper_commit`) via kprobe. Injeta a matriz CTM Rec.709 3x3 calculada em aritmética inteira de 64 bits (formato S31.32), integrando saturação, contraste e brilho linear. Mantém a curva de transferência sRGB nativa de hardware do display controller inalterada, evitando distorções de gama e lavagem de tons escuros. Expõe o dispositivo de caractere `/dev/chroma_amd` (`miscdevice`) e nós de telemetria em `/sys/kernel/chroma_amd/`.
- **Utilitário CLI (`prisma-ctl`):** Utilitário de linha de comando em C++20 para consulta de telemetria do driver `amdgpu` e `hwmon`, ajuste atômico de parâmetros em `/dev/chroma_amd` e gerenciamento de perfis em `~/.config/prisma/profiles/`.
- **Plugin Noctalia Shell (`ui/prisma`):** Plugin nativo para o ambiente Noctalia Shell no compositor Niri (Wayland). Oferece um widget de barra com monitoramento em tempo real e uma central de controle visual com abas de **Desempenho**, **Tela & Cor** e **Perfis**.

## 2. Requisitos de Sistema

- **Distribuição:** Arch Linux, CachyOS ou distribuições baseadas em Linux com suporte a DRM/KMS e GPUs AMD (RDNA / GCN).
- **Pacotes do sistema:**
  - Headers do kernel Linux ativo (`linux-headers` ou `linux-cachyos-headers`)
  - Compilador C/C++ (`clang` com suporte a `LLVM=1` para CachyOS, ou `gcc`)
  - `cmake` (>= 3.20) e `make`
  - Ferramentas de desenvolvimento base (`base-devel`, `libdrm`)
  - Noctalia Shell (para execução do painel gráfico)

## 3. Compilação e Instalação

### Instalação Recomendada no Arch Linux / CachyOS (DKMS)

O repositório fornece empacotamento completo com suporte a DKMS para Arch Linux, CachyOS e distribuições derivadas. O pacote compila a CLI, instala as regras de permissão udev, configura o carregamento automático do módulo e registra o código-fonte no DKMS para reconstrução automática a cada atualização de kernel.

Para compilar e instalar o pacote:

```bash
cd packaging/arch
makepkg -si
```

O suporte a GPUs abrange tanto placas dedicadas (Radeon RX) quanto processadores com vídeo integrado (APUs Ryzen), identificando dinamicamente a GPU ativa através da enumeração de `/sys/class/drm/` e conectores com display ativo, sem necessidade de configuração manual de índices (`card0` ou `card1`).

Após a instalação do pacote, habilite o plugin no Noctalia Shell:

```bash
noctalia msg plugins source add deyvideguilherme path /usr/share/prisma-control-center/ui
noctalia msg plugins update deyvideguilherme
noctalia msg plugins enable deyvideguilherme/prisma
```

### Instalação Manual para Desenvolvimento

Caso prefira compilar os componentes individualmente sem utilizar o gerenciador de pacotes:

#### 1. Compilação do Módulo de Kernel

```bash
cd kernel
make
```

*Nota: Em sistemas CachyOS compilados com Clang/LLVM, o Makefile detecta automaticamente `CONFIG_CC_IS_CLANG=y` e ativa `LLVM=1`.*

#### 2. Compilação da CLI

A partir da raiz do repositório:

```bash
cmake -B build
cmake --build build
cmake --build build --target install-local
```

O binário `prisma-ctl` é instalado em `~/.local/bin/prisma-ctl`. Certifique-se de que `~/.local/bin` está presente no seu `$PATH`.

Para permitir que o serviço do Noctalia Shell valide e execute a CLI em qualquer contexto, crie o link simbólico global:

```bash
sudo ln -sf ~/.local/bin/prisma-ctl /usr/local/bin/prisma-ctl
```

## 4. Configuração do Módulo e Permissões (Instalação Manual)

Se você instalou via pacote `PKGBUILD`, as permissões udev e o carregamento no boot já foram configurados automaticamente. Para instalações manuais:

### Carregamento Temporário

Para testar o módulo na sessão atual:

```bash
cd kernel
sudo make load
```

Para verificar o registro no log do kernel:

```bash
sudo dmesg | grep chroma_amd
```

Para descarregar:

```bash
cd kernel
sudo make unload
```

### Acesso sem Root (Regra udev)

Instale a regra udev para conceder acesso local via `uaccess` e grupo `video`:

```bash
cd kernel
sudo make install-udev
```

### Carga Automática no Boot

Para carregar o módulo automaticamente a cada inicialização do sistema:

1. Copie o binário compilado para a árvore de módulos do kernel:
```bash
sudo install -Dm644 kernel/chroma_amd.ko /lib/modules/$(uname -r)/extra/chroma_amd.ko
sudo depmod -a
```

2. Registre o módulo na inicialização:
```bash
echo "chroma_amd" | sudo tee /etc/modules-load.d/chroma_amd.conf
```

#### Suporte Manual a DKMS

Para registrar o módulo manualmente no DKMS:

```bash
sudo cp -r kernel /usr/src/chroma-amd-0.1.0
sudo dkms add -m chroma-amd -v 0.1.0
sudo dkms build -m chroma-amd -v 0.1.0
sudo dkms install -m chroma-amd -v 0.1.0
```

## 5. Instalação Manual no Noctalia Shell

Se você não utilizou o pacote Arch Linux, o plugin pode ser instalado via CMake ou registrado como fonte local:

### Opção A: Instalação Materializada via CMake (Recomendada)

```bash
cmake --build build --target install-ui
noctalia msg plugins update deyvideguilherme
noctalia msg plugins enable deyvideguilherme/prisma
```

O plugin é instalado em `~/.local/state/noctalia/plugins/materialized/deyvideguilherme/prisma`.

### Opção B: Fonte Local para Desenvolvimento

```bash
noctalia msg plugins source add deyvideguilherme path $(pwd)/ui
noctalia msg plugins update deyvideguilherme
noctalia msg plugins enable deyvideguilherme/prisma
```

### Verificação do Status do Plugin

```bash
noctalia msg plugins list | grep prisma
```

## 6. Referência da CLI (prisma-ctl)

O utilitário `prisma-ctl` gerencia parâmetros de hardware, consulta de sensores e persistência de perfis.

### Inspeção de Estado e Sensores

```bash
# Exibe estado atual da calibragem e sensores de telemetria
prisma-ctl status

# Saída em formato JSON estruturado
prisma-ctl status --json

# Exibe exclusivamente os sensores de hardware (carga, clock, temperaturas, potência, VRAM)
prisma-ctl telemetry
prisma-ctl telemetry --json
```

### Ajuste de Parâmetros de Cor

```bash
# Ajustar saturação, contraste e brilho
prisma-ctl set --saturation 160 --contrast 115 --brightness 10

# Usando flags curtas
prisma-ctl set -s 140 -c 110 -b 5

# Desativar temporariamente a calibragem sem descarregar o driver
prisma-ctl set --enabled 0

# Reativar a calibragem
prisma-ctl set --enabled 1
```

Faixas de valores suportadas:
- `saturation` (`-s`): `0` a `400` (padrão `100`; `0` = monocromático, `100` = normal, `200` = saturação dobrada)
- `contrast` (`-c`): `10` a `300` (padrão `100`; fator linear de contraste)
- `brightness` (`-b`): `-100` a `100` (padrão `0`; ganho de luminância linear de 0.25x a 1.75x na matriz CTM)
- `enabled` (`-e`): `0` ou `1` (padrão `1`; restaura bypass quando desativado)

### Gerenciamento de Perfis

Os perfis são armazenados como arquivos individuais em `~/.config/prisma/profiles/<nome>.conf`:

```bash
# Salvar o estado ativo como um perfil
prisma-ctl profile save jogos

# Aplicar um perfil salvo diretamente ao hardware
prisma-ctl profile apply jogos

# Listar perfis salvos
prisma-ctl profile list

# Excluir um perfil
prisma-ctl profile delete jogos

# Restaurar valores padrão (sat=100, con=100, bri=0, en=1)
prisma-ctl profile reset
```

## 7. Controle Direto via Shell

É possível inspecionar e alterar o estado do hardware sem utilizar a CLI:

```bash
# Escrita de parâmetros (atômica, sem root com udev configurado)
echo "saturation=160 contrast=115 brightness=10" > /dev/chroma_amd

# Leitura do estado ativo
cat /dev/chroma_amd

# Leitura individual de telemetria via sysfs (somente leitura)
cat /sys/kernel/chroma_amd/saturation
cat /sys/kernel/chroma_amd/contrast
cat /sys/kernel/chroma_amd/brightness
cat /sys/kernel/chroma_amd/enabled
```

## 8. Licença

Código em userspace sob licença MIT. Módulo de kernel sob licença Dual MIT / GPL v2.
