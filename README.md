# Prisma Control Center

Calibragem de cor em hardware e monitoramento de sensores para placas de vídeo AMD no Noctalia Shell (Niri / Wayland).

## 1. Visão Geral e Arquitetura

O Prisma Control Center (`prism-amd`) ajusta a saída de vídeo de GPUs AMD (`amdgpu`) via DRM/KMS e monitora sensores pelo sysfs. A calibração SDR usa uma matriz linear de saturação, contraste e ganho de brilho no pipeline de exibição.

A suíte é composta por três componentes:

- **Módulo de Kernel (`kernel/chroma_amd.ko`).** Descobre GPUs AMD com suporte a CTM ao observar `drm_atomic_helper_commit`. Aplica a matriz Rec.709 em uma transação própria validada pelo DRM e pelo AMDGPU. Expõe a configuração em `/dev/chroma_amd` e atributos de leitura em `/sys/kernel/chroma_amd/`.
- **Utilitário CLI (`prisma-ctl`):** Utilitário de linha de comando em C++20 para consulta de telemetria do driver `amdgpu` e `hwmon`, ajuste atômico de parâmetros em `/dev/chroma_amd` e gerenciamento de perfis em `~/.config/prisma/profiles/`.
- **Plugin Noctalia Shell (`ui/prisma`):** Plugin nativo para o ambiente Noctalia Shell no compositor Niri (Wayland). Oferece um widget de barra com monitoramento em tempo real e uma central de controle visual com abas de **Desempenho**, **Tela & Cor** e **Perfis**.

## 2. Requisitos de Sistema

- **CLI.** Linux com compilador C++20, CMake 3.20 ou mais recente e Make ou Ninja.
- **Módulo.** Headers do kernel alvo, Kbuild, suporte a Kprobes, IRQ work e ao acesso a argumentos da arquitetura, DRM/KMS e AMDGPU com propriedade CTM nas saídas de interesse.
- **DKMS no Arch/CachyOS.** Instale os headers e a cadeia de compilação do kernel alvo. Kernels compilados com Clang usam Clang, LLVM e LLD. Headers que geram BTF para módulos podem exigir `pahole`.
- **Interface.** Noctalia Shell com suporte à API 36 do plugin.

O módulo foi compilado para CachyOS x86_64 **6.18.60-2-cachyos-lts** e **7.3.4-1-cachyos**. A integração com o dispositivo de caractere foi executada nesses kernels em QEMU. O [relatório de auditoria](AUDIT_REPORT.md) registra o alcance dos testes de DRM e hardware.

## 3. Compilação e Instalação

### Instalação Recomendada no Arch Linux / CachyOS (DKMS)

A partir de um checkout completo do repositório, é possível usar o empacotamento com suporte a DKMS para Arch Linux, CachyOS e distribuições derivadas. O pacote compila a CLI, instala as regras de permissão udev, configura o carregamento automático do módulo e registra o código-fonte no DKMS para reconstrução automática a cada atualização de kernel.

Para compilar e instalar o pacote:

```bash
cd packaging/arch
makepkg -si
```

A CLI enumera adaptadores AMD via `/sys/class/drm/` e prioriza a telemetria de uma GPU com tela conectada. O módulo acompanha separadamente os CRTCs dos dispositivos AMDGPU descobertos com suporte a CTM.

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

### Compilação dos componentes históricos

As opções abaixo habilitam a camada Vulkan e o interceptor DRM via `LD_PRELOAD`. São componentes experimentais independentes do módulo e da CLI. Vulkan exige os headers Vulkan e `glslangValidator` ou `glslc`. O interceptor DRM exige `pkg-config` e os headers de libdrm. Ambos usam a configuração descrita em `chroma.conf.example`.

```bash
cmake -S . -B build-legacy -DPRISMA_BUILD_LEGACY_VULKAN=ON -DPRISMA_BUILD_LEGACY_DRM=ON
cmake --build build-legacy
ctest --test-dir build-legacy --output-on-failure
```

Com as opções habilitadas, `install-local` instala também os legados selecionados. A camada Vulkan usa `ENABLE_CHROMA_LAYER=1` e o manifesto local do Vulkan. Use as opções correspondentes ao executar `uninstall-local` para remover uma instalação legada anterior.

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

Para desativar a calibração e descarregar, mantenha disponível um TTY ou uma sessão SSH com acesso administrativo.

```bash
prisma-ctl set --enabled 0
sudo modprobe -r chroma_amd
```

Se a remoção do módulo for feita numa instalação de desenvolvimento, use `sudo make -C kernel unload`. O descarregamento também tenta remover as CTMs ainda pertencentes ao Prisma. Um erro de restauração é registrado no log do kernel. Se a sessão gráfica precisar de recuperação, confirme que o módulo foi removido pelo TTY ou SSH e reinicie o compositor. O parâmetro de boot `module_blacklist=chroma_amd` impede a carga durante uma inicialização de recuperação.

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
sudo cp -r kernel /usr/src/chroma-amd-1.0.0
sudo dkms add -m chroma-amd -v 1.0.0
sudo dkms build -m chroma-amd -v 1.0.0
sudo dkms install -m chroma-amd -v 1.0.0
```

## 5. Instalação Manual no Noctalia Shell

Se você não utilizou o pacote Arch Linux, o plugin pode ser instalado via CMake ou registrado como fonte local diretamente pela CLI do Noctalia:

### Opção A: Instalação via CMake (Recomendada)

Com o binário `prisma-ctl` instalado no seu `PATH`, execute:

```bash
cmake --build build --target install-ui
```

Esse comando registra o diretório `ui/` do Prisma como uma fonte local no Noctalia (`noctalia msg plugins source add prisma path <caminho>/ui`), sincroniza o catálogo com `noctalia msg plugins update prisma` e ativa o plugin com `noctalia msg plugins enable deyvideguilherme/prisma`.

Para remover a integração:
```bash
cmake --build build --target uninstall-ui
```

### Opção B: Registro Manual de Fonte Local

Você também pode registrar e gerenciar o plugin diretamente pelos comandos de mensageria IPC do Noctalia:

```bash
noctalia msg plugins source add prisma path $(pwd)/ui
noctalia msg plugins update prisma
noctalia msg plugins enable deyvideguilherme/prisma
```

### Verificação do Status do Plugin

Confirme que o plugin foi reconhecido e está ativo:

```bash
noctalia msg plugins list | grep prisma
```
Saída esperada:
```
deyvideguilherme/prisma [prisma] 1.0.0 enabled requires prisma-ctl
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
- `enabled` (`-e`): `0` ou `1` (padrão `1`; desativa a calibração do Prisma, preservando a CTM externa)

### Gerenciamento de Perfis

Os perfis são armazenados como arquivos individuais em `~/.config/prisma/profiles/<nome>.conf` ou `$XDG_CONFIG_HOME/prisma/profiles/<nome>.conf`. Use um diretório real e gravável apenas pelo proprietário. Perfis existentes no formato `chave=valor` continuam válidos. O módulo reinicia com os valores padrão. Use `profile apply` para reaplicar um perfil após reiniciar. Para migrar perfis mantidos por links, copie os arquivos regulares para esse diretório e substitua os links.

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

### Autodiagnóstico do Sistema (prisma-ctl doctor)

O subcomando `doctor` executa uma bateria completa de verificações para identificar inconsistências no kernel, no módulo, no nó de dispositivo e na GPU:

```bash
prisma-ctl doctor
```

Exemplo de saída:

```text
Prisma Control Center - System Self-Diagnostic (doctor)
======================================================

[OK] Linux kernel: 7.3.4-1-cachyos (x86_64)
[OK] Kernel module: chroma_amd is loaded (version 1.0.0)
[OK] Device node: /dev/chroma_amd (group: video, read/write access granted)
[OK] Protocol ABI: v1 (kernel module and CLI userspace are synchronized)
[OK] AMD GPU detected: card1 (driver: amdgpu, PCI: 0000:2d:00.0)
     - Display output: DP-2 (connected, 2560x1440)
     - Hardware telemetry:
       * Edge temperature:     40.0 °C
       * Hotspot temperature:  42.0 °C
       * Graphics clock (SCLK): 1800 MHz
       * GPU load:              4 %
       * VRAM usage:            1974 / 16304 MiB (12.1 %)
       * Board power:           20.0 W

------------------------------------------------------
Diagnosis: Module access, ABI and AMD GPU detection passed.
```

O comando retorna código de saída `0` quando todos os componentes estão saudáveis e `1` caso seja detectada qualquer falha crítica (módulo não carregado, ausência de permissões no grupo `video`, descompasso de ABI ou ausência de GPU AMD).

### Versão e ABI

```bash
prisma-ctl --version
# Exemplo de saída: prisma-ctl 1.0.0 (ABI v1)
```

## 7. Compatibilidade de Espaço de Cor: Modo SDR vs HDR / ICC

A calibração destina-se a SDR Rec.709. O Prisma usa a propriedade CTM de cada CRTC elegível. As propriedades de gamma e degamma permanecem sob controle do compositor. Com HDR ou um perfil ICC ativo, desative a calibração para evitar uma transformação SDR no pipeline.

A descoberta de uma GPU requer que ela passe pelo helper DRM observado depois que o módulo é carregado. Em uma sessão ociosa, a primeira descoberta pode depender do próximo commit do compositor. Em GPUs descobertas, a CLI confirma que o módulo aceitou a configuração. A transação de hardware ocorre separadamente, sem reiniciar a sessão. O driver pode rejeitá-la. Consulte o log do kernel para erros de commit.

Se o compositor já tiver uma CTM, a calibração cede a saída ao compositor. O módulo remove apenas o blob exato que instalou com sucesso. A desativação também preserva uma matriz mais recente definida pelo compositor. O comportamento vale por saída e por GPU.

## 8. Sincronização e Automação de Versões (scripts/bump-version.sh)

Para garantir integridade e consistência de versão entre todos os subsistemas (módulo de kernel, cabeçalho de ABI, utilitário CLI, empacotamento Arch/DKMS e manifesto do plugin Noctalia), o repositório disponibiliza o utilitário `scripts/bump-version.sh`:

```bash
./scripts/bump-version.sh 1.0.0
```

O script executa as seguintes etapas:
1. Valida o formato semântico (`X.Y.Z`).
2. Atualiza `CMakeLists.txt` (`project(prisma VERSION ...)`).
3. Atualiza `kernel/dkms.conf` (`PACKAGE_VERSION=...`).
4. Atualiza a macro de versão no cabeçalho de ABI `kernel/prisma_abi.h` (`PRISMA_VERSION`).
5. Atualiza `packaging/arch/PKGBUILD` (`pkgver=...` e redefine `pkgrel=1`).
6. Atualiza o manifesto do plugin Noctalia Shell `ui/prisma/plugin.toml` (`version = "..."`).
7. Regenera `packaging/arch/.SRCINFO` executando `makepkg --printsrcinfo`.
8. Realiza validação cruzada rigorosa, assegurando que nenhum arquivo permaneça com versão divergente.

## 9. Controle Direto via Shell

É possível inspecionar e alterar o estado do hardware sem utilizar a CLI:

```bash
# Escrita transacional dos parâmetros desejados (sem root com udev configurado)
echo "saturation=160 contrast=115 brightness=10" > /dev/chroma_amd

# Leitura do estado ativo
cat /dev/chroma_amd

# Leitura individual de telemetria via sysfs (somente leitura)
cat /sys/kernel/chroma_amd/abi_version
cat /sys/kernel/chroma_amd/saturation
cat /sys/kernel/chroma_amd/contrast
cat /sys/kernel/chroma_amd/brightness
cat /sys/kernel/chroma_amd/enabled
```

## 10. Licença

Código em userspace sob licença MIT. Módulo de kernel sob GPL-2.0-only.
