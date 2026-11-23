# AGENTS

Consulte `ARCHITECTURE.md` para entender a arquitetura do projeto e `README.md` para instruções de compilação, requisitos e empacotamento.

O projeto trabalha com Kernel Linux e subsistemas de vídeo, incluindo DRM/KMS, Kprobes, CLI em C++20 e interface Luau no Wayland. Mantenha os comentários no código objetivos e técnicos. Evite adicionar comentários desnecessários ou modificar trechos fora do escopo da tarefa.

> [!IMPORTANT]
> **Regra de testes:** Toda correção de bug no Kernel, na CLI ou na interface deve incluir testes de regressão automatizados que comprovem a correção.

---

## 1. Gerenciamento de branches e regras de Git

Para preservar a integridade do repositório e evitar perda de trabalho, siga estas regras:

1. **Não desenvolva diretamente na `main`:** Essa branch representa a versão estável do projeto. Nenhuma alteração deve ser desenvolvida ou commitada diretamente nela.

2. **Verifique a branch atual:** Antes de criar uma nova branch, execute `git branch --show-current`. Se já estiver na branch correspondente à tarefa, continue nela. Evite criar branches desnecessárias ou alterar a branch ativa sem verificar o estado do repositório.

3. **Crie uma branch por tarefa:** Utilize os seguintes prefixos:
   - `fix/<descricao-curta>` para correções de bugs.
   - `feat/<descricao-curta>` para novas funcionalidades.
   - `refactor/<descricao-curta>` para refatorações sem alteração de comportamento.
   - `test/<descricao-curta>` para expansão de testes.
   - `docs/<descricao-curta>` para documentação.

4. **Preserve as alterações locais:** Execute `git status` antes de iniciar qualquer trabalho. Não descarte alterações, arquivos unstaged ou stashes do proprietário.

5. **Não execute operações destrutivas sem autorização:** Comandos como `git reset --hard`, `git clean -fd`, `git checkout -- .`, `git branch -D` e `git push --force` exigem autorização explícita do proprietário.

6. **Revise as alterações:** Antes de concluir uma tarefa, examine `git diff` e `git status`. Verifique se não foram introduzidos resíduos de depuração, como `printk`, arquivos temporários, logs ou modificações fora do escopo.

7. **Valide antes do merge:** Nenhuma branch deve ser mesclada à `main` sem que os testes automatizados relevantes tenham sido executados localmente ou na CI com sucesso.

---

## 2. Matriz de hardware e validação técnica

Problemas de hardware, calibração CTM e telemetria devem ser investigados utilizando as saídas de `prisma-ctl doctor`, `prisma-ctl telemetry --json` e logs do Kernel (`dmesg`).

A identificação da GPU deve considerar o PCI ID individual (`0x1002:...`), subvendor, arquitetura, geração DCN e dispositivo correspondente em `/sys/class/drm/card*`.

### 2.1. Validação de hipóteses

> [!WARNING]
> Um comportamento observado em uma GPU ou máquina específica não deve ser considerado válido para toda uma geração de hardware. Registre cada particularidade técnica junto ao seu nível de comprovação, versão do Kernel e ambiente de teste. Mantenha os registros separados por PCI ID.

### 2.2. Classificação dos equipamentos

- **Homologado:** Testado em hardware físico, com logs de telemetria, commits CTM auditados e validação do comportamento esperado.
- **Identificado:** Suporte mapeado a partir do código do driver AMDGPU e do PCI ID, com testes virtuais ou aguardando validação em hardware físico.
- **Ambiente virtual:** Testes realizados em máquinas virtuais ou ambientes de software que não utilizam uma GPU AMD física.

### 2.3. Hardware e comportamentos conhecidos

| PCI ID | Arquitetura / DCN | Modelo de Hardware | Status | Confirmação | Ambiente e Kernel | Particularidades Técnicas |
|---|---|---|---|---|---|---|
| `1002:7590` | RDNA 4 (DCN 4.x) | AMD Radeon RX 9060 XT | **Homologado** | Hardware real | CachyOS x86_64 `6.18.60-2-cachyos-lts` e `7.3.4-1-cachyos`, Niri 26.04 (Wayland) | Sensores independentes de borda e Hotspot; teto dinâmico de 300W+; VRAM física dedicada; CTM aplicada via fast path sem necessidade de modeset. |
| `1002:1638` | Vega / Cezanne / Renoir (DCN 2.1) | AMD Ryzen 5 5600GT (Gráficos Integrados) | **Homologado** | Hardware real e código do Kernel | Testado em hardware físico e verificado em `amdgpu_dm.c` (Linux 6.18 / 7.3.4) | Sensor de encapsulamento único (`temp_edge_c`; sem Hotspot `temp_junction_c`); teto dinâmico de 65W PPT; memória unificada `VRAM + GTT`; fallback obrigatório de `allow_modeset = true` durante a transição de CTM, pois DCN 2.x rejeita o fast path com `-EINVAL`. |
| `1002:73bf` | RDNA 2 (DCN 3.0) | AMD Radeon RX 6800 / 6800 XT / 6900 XT (Navi 21) | **Identificado** | Código do Kernel | Mapeamento no AMDGPU e análise de sysfs | Monitoramento simultâneo de Hotspot e borda; alta faixa de potência; suporte a CTM via fast path. A paridade com DCN 4.x para fast path sem modeset aguarda homologação física individual. |
| `1002:744c` | RDNA 3 (DCN 3.2) | AMD Radeon RX 7900 XT / XTX (Navi 31) | **Identificado** | Código do Kernel | Mapeamento no AMDGPU e análise de sysfs | Monitoramento simultâneo de Hotspot e borda; alta faixa de potência; suporte a CTM via fast path. A paridade com DCN 4.x para fast path sem modeset aguarda homologação física individual. |
| N/A | Virtual KMS | Driver VKMS (Linux DRM) | **Ambiente Virtual** | Teste em ambiente virtual | Ubuntu 24.04 (CI), CTest em `test_vkms_ctm` | Validação de atomic commits e injeção de blobs CTM em pipeline de vídeo virtual, sem GPU física AMD. |

Sempre que uma nova GPU ou APU for identificada, adicione uma linha própria à tabela, registrando o PCI ID, classificação, versão do Kernel e ambiente em que foi testada.

---

## 3. Antes de aplicar uma correção

Não presuma o comportamento do hardware ou do Kernel. Identifique a causa do problema antes de implementar uma solução.

O projeto possui três camadas principais, cada uma com suas próprias regras.

### 3.1. Kernel (módulo `kernel/`)

- **Estabilidade:** Bugs no Kernel podem travar ou derrubar a máquina do usuário. Toda alteração deve considerar os riscos de concorrência, interrupções e acesso ao hardware.

- **Segurança dos Kprobes:** O manipulador `chroma_pre_handler`, associado a `drm_atomic_helper_commit`, executa em contexto atômico e não preemptível. Dependendo da arquitetura e do modo de otimização do Kprobe, preempção ou interrupções podem estar desabilitadas.

  É proibido dormir, executar operações bloqueantes ou adquirir locks que possam bloquear, como mutexes. Também não utilize `printk()` ou `pr_*()` diretamente nesse contexto.

  A descoberta de dispositivos deve enfileirar referências seguras e transferir o trabalho para uma workqueue através de `irq_work`.

  - **Sincronização entre CPUs:** Handlers de Kprobes podem executar simultaneamente em CPUs diferentes. Qualquer estado compartilhado acessado pelo handler deve possuir sincronização segura, utilizando operações atômicas ou locks adequados, sem espera prolongada ou risco de contenção concorrente.

- **Commits atômicos:** Nunca altere ponteiros de estados DRM em andamento. Toda aplicação de CTM deve ocorrer por meio de um novo commit atômico independente.

  Utilize `drm_dev_enter/exit` para proteger o acesso ao ciclo de vida do dispositivo, evitando operações durante desconexão ou remoção do hardware. Respeite também os mecanismos de sincronização e locking do DRM/KMS.

- **Preservação de CTM e GAMMA_LUT externas:** O pipeline de gerenciamento de cores do DRM/KMS possui três propriedades sequenciais:

  `DEGAMMA_LUT` → `CTM` → `GAMMA_LUT`

  A matriz CTM (3x3) fica posicionada entre as duas LUTs. O Prisma dedica a CTM estritamente à saturação Rec.709 e a GAMMA_LUT 1D ao contraste (com pivô no cinza médio 0.5) e brilho (offset linear limitado a ±50%).

  O módulo inspeciona independentemente os coeficientes CTM (`chroma_is_identity_ctm`) e as entradas da GAMMA_LUT (`chroma_is_identity_gamma_lut` com tolerância ±1).

  Se uma propriedade estiver ausente ou contiver valores de identidade, o Prisma assume o controle dela. Se houver uma propriedade externa não identidade (como Night Light / wlsunset / perfil ICC na GAMMA_LUT), o Prisma cede exclusivamente a propriedade conflitante e continua gerenciando a outra (permitindo, por exemplo, saturação calibrada concomitantemente com Night Light ativo).

- **Tratamento de erros:** Trate `-EINVAL` na transição de CTM ou GAMMA_LUT em DCN 2.x com nova tentativa de commit atômico utilizando `allow_modeset = true`.

  Trate `-EBUSY`, causado por conflitos como pageflip em andamento do compositor, reagendando a operação de forma assíncrona até o limite de `CHROMA_MAX_BUSY_RETRIES = 5`.

### 3.2. CLI e telemetria (`src/cli/`)

- **Telemetria independente:** A leitura de sensores da GPU em `telemetry.cpp` deve funcionar através de `/sys/class/drm/` e `hwmon`, mesmo quando o módulo `/dev/chroma_amd` não estiver carregado.

- **Validação de valores:** Verifique os limites numéricos antes de qualquer escrita no dispositivo de caractere:
  - Saturação: 0–400.
  - Contraste: 10–300.
  - Brilho: -100–100.
  - Enabled: 0–1.

- **Persistência segura de perfis:** Em `profiles.cpp`, não siga links simbólicos (`O_NOFOLLOW`). Utilize descritores de diretório (`openat`, `unlinkat`), valide nomes contra path traversal e realize gravações atômicas utilizando arquivo temporário, `rename` e `fsync`.

### 3.3. Interface Noctalia Shell (`ui/prisma/`)

- **Prevenção de snap-back:** Respeite o fluxo de atualização otimista e as flags `hasPendingColorUpdate` e `isMutating`. Os sliders não devem retornar ao valor anterior enquanto o comando assíncrono estiver sendo processado.

- **Escala conforme o hardware:** Ajuste os limites visuais de acordo com o tipo de GPU identificado pela telemetria:
  - APUs: referência de 65W e memória unificada VRAM + GTT.
  - dGPUs: referência de 300W e VRAM dedicada.

---

## 4. Diagnóstico em tempo real

Antes de sugerir alterações no Kernel ou na CLI, utilize as ferramentas de inspeção disponíveis no sistema.

### 4.1. Dynamic Debug no Kernel Linux

O módulo utiliza `pr_debug` nos pontos críticos. Para acompanhar as mensagens do driver sem recompilar:

```bash
# Ativar mensagens de debug dinâmico do módulo chroma_amd
echo "module chroma_amd +p" | sudo tee /sys/kernel/debug/dynamic_debug/control

# Acompanhar as transições de CTM e a fila de trabalho
dmesg -w -T | grep -E "chroma_amd|amdgpu"
```

### 4.2. Diagnóstico e estado da CLI

```bash
# Diagnóstico do sistema, dispositivo e hardware
prisma-ctl doctor

# Consultar telemetria estruturada
prisma-ctl telemetry --json

# Consultar estado da calibração e telemetria
prisma-ctl status --json
```

### 4.3. Sysfs e permissões

```bash
# Verificar permissões do dispositivo
ls -l /dev/chroma_amd

# Consultar os atributos expostos pelo módulo
cat /sys/kernel/chroma_amd/abi_version
cat /sys/kernel/chroma_amd/enabled
cat /sys/kernel/chroma_amd/saturation
cat /sys/kernel/chroma_amd/contrast
cat /sys/kernel/chroma_amd/brightness
```

O dispositivo deve pertencer a `root:video` e utilizar permissões `0660`.

---

## 5. Ferramentas de depuração e sintomas

Utilize as ferramentas apropriadas antes de assumir uma causa para o problema.

| Sintoma / Cenário | Ferramenta / Procedimento | O que verificar |
|---|---|---|
| **Tela preta, travamento de vídeo ou rejeição de commit atômico** | Ativar Dynamic Debug: `echo "module chroma_amd +p" > /sys/kernel/debug/dynamic_debug/control` | Verificar no `dmesg` se ocorreu `-EINVAL` (necessidade de fallback com modeset) ou `-EBUSY` (conflito com pageflip). Confirmar se o fallback para `allow_modeset = true` foi acionado. |
| **Módulo não carrega ou símbolo indefinido** | `modinfo ./kernel/chroma_amd.ko` e `dmesg \| tail -n 50` | Verificar dependências de `drm_kms_helper` e a versão da API atômica utilizada na compilação. |
| **Permissão negada ao executar `prisma-ctl set`** | `ls -l /dev/chroma_amd` e `groups` | Verificar se o usuário pertence ao grupo `video` e se a regra `70-chroma-amd.rules` foi aplicada pelo udev. |
| **Telemetria vazia (`"card": null`) ou GPU não encontrada** | `prisma-ctl telemetry --json` e `prisma-ctl doctor` | Verificar `/sys/class/drm/card*/device/vendor` (deve retornar `0x1002`) e a existência de uma pasta `hwmon` no dispositivo PCI. |
| **APU reportando 0W ou sem temperatura Hotspot** | `prisma-ctl telemetry --json` | APUs utilizam o sensor Edge (`temp_edge_c`) e podem disponibilizar potência via PPT. Confirmar se `is_apu: true` foi identificado para ativar o fallback de sensor. |
| **Conflito com Night Light ou compositor Wayland (Niri)** | Dynamic Debug, filtrando `adopting CRTC` ou `has active non-identity external CTM` | O módulo deve ceder o controle quando uma CTM externa não identidade for detectada e retomá-lo quando a matriz voltar à identidade. |
| **Snap-back de slider ou inconsistência na interface Noctalia** | `python3 tests/check_ui.py` | O script valida o manifesto, as traduções e executa a suíte Luau `tests/test_ui.luau`. Os cinco cenários simulados são: APU, dGPU, módulo offline, debounce e telemetria desacoplada. |
| **Testar o módulo sem arriscar a máquina física** | `tests/run-kernel-vm.sh` com QEMU e BusyBox | Testar carga do módulo, sysfs, ioctls ABI v1 e valores inválidos. |
| **Validar cálculo CTM Rec.709 e conversão S31.32** | `./build-legacy/test_ctm` | Validar saturação, matriz identidade, truncamento e representação em ponto fixo S31.32. Alvo opcional da CI dedicada. |
| **Testar commits atômicos DRM em driver virtual** | `./build-legacy/test_vkms_ctm` | Validar a aplicação de CTM em ambiente virtual KMS sem GPU física AMD. Alvo opcional da CI dedicada. |
| **Validar CLI, telemetria e perfis** | `./build/test_cli` | Verificar nomes inválidos, links simbólicos, números malformados e caminhos concorrentes. Alvo obrigatório da CI. |

---

## 6. Divisão de tarefas entre agentes

Divida o trabalho entre agentes especializados, de acordo com a camada afetada.

- **Kernel e DRM**
  - Escopo: `kernel/` (`chroma_main.c`, `chroma_ctm.c`, `chroma_drm.c`, `chroma_iface.c`), Kprobes, atomic commits, tratamento de `-EBUSY` e `-EINVAL`, Kbuild, DKMS e udev.
  - Modelo sugerido: modelo com alto raciocínio técnico.
  - Foco: concorrência, segurança em contexto atômico e estabilidade do sistema operacional.

- **CLI e telemetria**
  - Escopo: `src/cli/`, C++20, acesso a sysfs/hwmon, I/O POSIX (`device.cpp`), persistência de perfis (`profiles.cpp`), diagnósticos (`doctor.cpp`) e `test_cli.cpp`.
  - Foco: validação de entradas, tratamento de erros POSIX e conformidade com o schema JSON.

- **Interface Noctalia Shell**
  - Escopo: `ui/prisma/` (`prisma_service.luau`, `prisma_panel.luau`, `prisma_widget.luau`), debounce de comandos, pooling adaptativo, traduções (`translations/`), `tests/test_ui.luau` e `tests/check_ui.py`.
  - Foco: responsividade, prevenção de snap-back e fidelidade visual.

- **QA e testes**
  - Escopo: `tests/`, automação em `CMakeLists.txt`, CI GitHub Actions, CTest (`test_cli`, `check_ui`), execução da VM QEMU e testes legados opcionais (`test_vkms_ctm`).
  - Foco: criação de testes conforme o tipo de tarefa e validação antes da integração.

- **Agente principal**
  - Escopo: coordenar os subagentes, revisar os diffs, preservar os contratos de integração (ABI v1 e JSON) e verificar os critérios de conclusão.

### 6.1. Coordenação de arquivos compartilhados

> [!IMPORTANT]
> Antes de delegar tarefas em paralelo, identifique os arquivos utilizados por mais de um agente, como `CMakeLists.txt`, workflows do GitHub Actions e arquivos de `tests/`.
>
> Não permita alterações simultâneas no mesmo arquivo sem coordenação. O agente principal deve consolidar as alterações sequencialmente e revisar possíveis conflitos.

---

## 7. Compilação e validação por camada

Execute os testes de acordo com os componentes modificados.

### 7.1. Matriz de validação por camada

| Camada / Alteração | Validação obrigatória | Ambiente / Ferramenta |
|---|---|---|
| **Kernel / Comportamento do driver** | Teste de integração em QEMU e validação física quando disponível | `tests/run-kernel-vm.sh`, Kernel Linux alvo e BusyBox estático |
| **Contrato ABI do Kernel** | `test_kernel_abi` (structs, offsets e ioctls da ABI v1) | Binário C++ compilado via CMake |
| **Matemática CTM / GAMMA_LUT** | `test_color_math` (CTM, GAMMA_LUT, rampa identidade, pivô e tolerância Night Light) | CTest |
| **CLI / Telemetria** | `test_cli` (limites numéricos, descritores POSIX, perfis e mocks) | CTest |
| **Interface Noctalia Shell** | `check_ui` (manifesto, sintaxe Luau e cinco cenários comportamentais) | `python3 tests/check_ui.py` |
| **Matemática CTM Rec.709** | `test_ctm` (cálculo matricial e conversão S31.32) | `build-legacy/`, na CI dedicada |
| **DRM/KMS virtual** | `test_vkms_ctm` (atomic commits em pipeline virtual) | `build-legacy/`, na CI dedicada |

### 7.2. Build padrão e testes obrigatórios

Os alvos padrão devem compilar em um ambiente limpo, sem dependências legadas.

```bash
# Configurar e compilar CLI e testes
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Executar testes da CLI
./build/test_cli

# Compilar e executar testes do contrato ABI
cmake --build build --target test_kernel_abi
./build/test_kernel_abi

# Executar test_cli e check_ui pelo CTest
ctest --test-dir build --output-on-failure

# Instalar a CLI em ~/.local/bin/prisma-ctl
cmake --build build --target install-local
```

### 7.3. Testes da interface Noctalia Shell

O script `check_ui.py` é utilizado localmente e pela CI para validar o plugin.

```bash
# Valida o manifesto, a sintaxe Luau e os cinco cenários de teste
python3 tests/check_ui.py
```

### 7.4. Compilação do módulo de Kernel

```bash
cd kernel
make

# Em kernels CachyOS compilados com Clang/LLVM,
# o Makefile detecta CONFIG_CC_IS_CLANG=y automaticamente.
```

### 7.5. Testes avançados e componentes legados

> [!NOTE]
> Estes alvos são executados em um job dedicado da CI e exigem `libdrm-dev`, `libvulkan-dev` e `glslang-tools`. São utilizados para validar a matemática CTM e o pipeline KMS virtual. Não fazem parte do build padrão de distribuição.

```bash
cmake -S . -B build-legacy \
    -DCMAKE_BUILD_TYPE=Release \
    -DPRISMA_BUILD_LEGACY_DRM=ON \
    -DPRISMA_BUILD_LEGACY_VULKAN=ON

cmake --build build-legacy --parallel

cmake --build build-legacy --target test_ctm test_vkms_ctm

ctest --test-dir build-legacy --output-on-failure
```

### 7.6. Empacotamento Arch Linux / DKMS

```bash
cd packaging/arch
makepkg -si
```

---

## 8. Critérios de conclusão de tarefa

Os testes exigidos dependem do tipo de alteração realizada.

### 8.1. Validação por tipo de tarefa

| Tipo de tarefa | Validação |
|---|---|
| `fix/*` | Criar teste de regressão que reproduza o problema, falhe antes da correção e passe após sua implementação. |
| `feat/*` | Criar testes automatizados que cubram os novos critérios de aceitação e os fluxos da funcionalidade. |
| `refactor/*` | Preservar os testes existentes e validar os comportamentos afetados, garantindo que não houve regressão. |
| `test/*` | Executar os novos testes e integrá-los à suíte e ao pipeline. |
| `docs/*` | Verificar consistência técnica, referências internas, comandos e exemplos práticos. |

### 8.2. Critérios obrigatórios

Uma tarefa só poderá ser considerada concluída quando atender aos seguintes critérios:

1. **Causa identificada:** Quando houver um bug, sua causa deve estar tecnicamente compreendida e documentada. Evite soluções paliativas e tentativas sem diagnóstico.

2. **Implementação:** O código deve respeitar a arquitetura da camada modificada, sem provocar efeitos colaterais em outros componentes.

3. **Testes:** Os testes devem ser criados ou atualizados conforme os requisitos da seção 7.1 e o tipo de tarefa definido na seção 8.1.

4. **Validação:** Todos os testes obrigatórios relacionados à alteração devem passar.

5. **Compatibilidade:** Os contratos entre os componentes devem ser preservados, incluindo ABI v1 em `/dev/chroma_amd`, schema `status --json` da CLI e API 36 do Noctalia Shell.

6. **Revisão do Git:** Execute e revise `git diff` e `git status`. Não deixe resíduos de debug, como `printk`, arquivos temporários, conflitos ou alterações fora do escopo.

7. **Registro dos testes:** Documente o ambiente em que a alteração foi validada. Exemplos:
   - Validado em hardware real RX 9060 XT.
   - Validado em VM QEMU/VKMS, com homologação em APU real pendente.

---

## 9. Regras de merge, ABI e componentes legados

- **Estabilidade da ABI v1:** O formato textual de `/dev/chroma_amd`, os ioctls ABI v1 (`GET_ABI`, `GET_STATE`, `SET_STATE`) e o schema de saída `prisma-ctl status --json` são contratos públicos do projeto. Não altere chaves ou tipos sem manter compatibilidade retroativa e atualizar `ARCHITECTURE.md`.

- **Componentes legados:** A camada Vulkan e o hook DRM via `LD_PRELOAD` devem permanecer sob as flags `PRISMA_BUILD_LEGACY_*`. O build padrão deve compilar exclusivamente a CLI moderna em C++20, sem dependências externas desnecessárias.

- **Mensagens de diagnóstico no Kernel:** Utilize `pr_debug()` nos contextos apropriados, permitindo ativação sob demanda pelo Dynamic Debug:

  ```bash
  echo "module chroma_amd +p" | sudo tee /sys/kernel/debug/dynamic_debug/control
  ```

  Nunca chame `printk()`, `pr_debug()` ou outras variantes de logging diretamente dentro de handlers Kprobe. Quando for necessário diagnosticar eventos capturados pelo Kprobe, transfira a coleta dos dados e a emissão das mensagens para o contexto de execução da workqueue.

- **Padrão de commits:** Utilize Conventional Commits:
  - `fix:`
  - `feat:`
  - `docs:`
  - `test:`
  - `refactor:`
  - `chore:`