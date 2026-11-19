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

### Compilação do Módulo de Kernel

```bash
cd kernel
make
```

*Nota: Em sistemas CachyOS compilados com Clang/LLVM, o Makefile detecta automaticamente `CONFIG_CC_IS_CLANG=y` e ativa `LLVM=1`.*

### Compilação da CLI

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

## 4. Configuração do Módulo e Permissões

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

Por padrão, `/dev/chroma_amd` é criado com permissões `0660` para o grupo `video`. Instale a regra udev e adicione o seu usuário ao grupo:

```bash
cd kernel
sudo make install-udev
sudo usermod -aG video $USER
```

*Nota: Reinicie a sessão de login para que a alteração de grupo tenha efeito no shell do usuário.*

### Carga Automática no Boot

Para carregar o módulo automaticamente a cada inicialização do sistema:

1. Copie o binário do módulo compilado para a árvore de módulos extras do kernel:
```bash
sudo install -Dm644 kernel/chroma_amd.ko /lib/modules/$(uname -r)/extra/chroma_amd.ko
sudo depmod -a
```

2. Registre o módulo na inicialização:
```bash
echo "chroma_amd" | sudo tee /etc/modules-load.d/chroma_amd.conf
```

#### Suporte a DKMS (Opcional)

Para reconstrução automática em atualizações de kernel:

```bash
sudo cp -r kernel /usr/src/prism-amd-0.1.0
sudo dkms add -m prism-amd -v 0.1.0
sudo dkms build -m prism-amd -v 0.1.0
sudo dkms install -m prism-amd -v 0.1.0
```

## 5. Instalação no Noctalia Shell

O plugin gráfico pode ser instalado via CMake ou registrado como fonte local de desenvolvimento:

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

### Atalho de Teclado no Niri e Quick Tile

Para alternar rapidamente a exibição do painel, configure uma combinação de teclas no compositor Niri (`~/.config/niri/cfg/keybinds.kdl`). Recomenda-se a utilização da tecla `Mod` (Super / Windows):

```kdl
binds {
    Mod+Shift+P allow-when-locked=true { spawn-sh "noctalia msg panel-toggle deyvideguilherme/prisma:panel"; }
}
```

Caso prefira o atalho tradicional estilo AMD Adrenalin (`Alt+R`):

```kdl
binds {
    Alt+R allow-when-locked=true { spawn "noctalia" "msg" "panel-toggle" "deyvideguilherme/prisma:panel"; }
}
```

**Comportamento em Jogos (Proton / XWayland):** Em sessões Wayland, jogos em tela cheia que utilizam Wine, Proton ou SDL frequentemente solicitam inibição de atalhos (`keyboard-shortcuts-inhibit`) ou captura exclusiva de entrada. Modificadores convencionais como `Alt` ou `Ctrl` podem ser capturados pelo jogo antes de chegar ao compositor. Atalhos que utilizam o modificador privilegiado do compositor (`Mod`) têm prioridade garantida no Niri e são processados antes do encaminhamento ao cliente gráfico, assegurando que o painel abra de forma imediata e sem conflitos durante partidas.

O plugin também registra um quick tile (`[[shortcut]]`) acessível na central de ações do Noctalia Shell (`Mod+S`), permitindo alternar a visibilidade do painel e acompanhar o status do hardware e o perfil ativo.

### Detecção Automática e Perfis por Jogo

O serviço de segundo plano (`prisma_service.luau`) consulta periodicamente a lista completa de janelas da sessão no Niri (`niri msg --json windows`) de forma assíncrona, identificando tanto a janela ativa (`is_focused == true`) quanto as demais aplicações em execução:

- **Lista Dinâmica de Janelas Abertas:** Descobre todas as janelas ativas na sessão (filtrando componentes internos do shell) e expõe a lista `openApps` ordenada (janela em foco em primeiro lugar, seguida pelas demais). Isso permite vincular jogos em tela cheia mesmo que o usuário tenha alternado temporariamente de janela para abrir o painel.
- **Persistência de Regras:** As vinculações entre aplicativos (`app_id`) e perfis são salvas em `~/.config/prisma/app_rules.json`.
- **Transição Atômica:** Ao alternar o foco para um jogo mapeado, o estado atual de calibragem da área de trabalho é preservado em memória como `fallback_state` e o perfil vinculado é aplicado diretamente ao hardware.
- **Restauração Automática:** Ao alternar para a área de trabalho ou para qualquer aplicativo sem regra vinculada, o serviço restaura automaticamente os parâmetros anteriores de saturação, contraste e brilho.
- **Configuração Visual:** Na aba **Perfis**, controles de navegação `<` / `>` e uma lista de seleção rápida permitem escolher qualquer janela aberta na sessão e vinculá-la ao perfil desejado com um clique. Na aba **Tela & Cor**, um indicador informa quando um perfil automático está em execução.

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
