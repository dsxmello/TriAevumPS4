# Fase 1 — Jogo AOT headless (TRIA00004): instruções de teste

Data: 2026-10-22. Primeiro build do **TriAevum de verdade na stack Vulkan**:
closure AOT inteiro (256 shards whole-AOT + módulo + serviços + PICA host +
TopScreen + áudio DSP HLE) compilado com clang 23 + **libc++ 21** sobre o
runtime DolphinPS4 (heap dlmalloc com malloc-replace, crash.log,
boot-trace.log) — **sem SDL, sem Piglet, sem Shacc**, identidade plain
(`CATEGORY gd`), VideoOut/scePad diretos.

## Resultado do 0.90 em console (2026-10-22): jogo RODANDO, tela preta

Rodou 10+ min **sem crash**, heap estável (512 MiB), jogo vivo: conteúdo
montado (`content.tap 1220 bytes`), `entries=162757`, `pica_gsp` ~12
comandos/frame (mesma taxa/frame do marco 2026-10-19), draws/completions
subindo, áudio na taxa exata, scanout do título capturado (portrait 240×400
BGR8 em duplo buffer — mesmos endereços do port antigo). fps assenta em
~14,7 (o mesmo perfil dos PKGs 0.07–0.15 antigos: o custo é a captura de
vértices/texturas por draw no Jaguar, `queue_ms` ~35; não é regressão do
runtime novo). **MAS a TV ficou 100% preta**: a página de status (fundo
azul + texto verde + barra verde do heartbeat) nunca apareceu — e ela é
desenhada ANTES e INDEPENDENTE do módulo, então não é o conteúdo/apagamento
de dump.

Análise diferencial contra o probe VK (que mostra imagem na MESMA TV/console
hoje): sysmodules, `sceVideoOutOpen`, `RegisterBuffers`, flip/equeue,
formato (`A8B8G8R8_SRGB` linear 1920×1080 pitch 1920) e tipo de memória
(`WC_GARLIC` 0x3) — tudo idêntico. A diferença única é **quem escreve os
pixels do flip buffer: GPU (probe, visível) vs CPU (host, preto)**.
Hipótese primária: garlic é servido ao display através do L2 da GPU, que
escritas de CPU não invalidam — o scanout fica mostrando as linhas velhas
(os zeros do `memset` de inicialização). Nenhum homebrew dos refs escreve
flip buffer por CPU (todos GPU) — não há precedente que valide o caminho.

## Diagnóstico 0.91 (build atual)

O 0.91 mantém o **buffer 0 garlic** (controle) e troca o **buffer 1 para
WB onion** (coerente com CPU). O heartbeat ganha `fails=/fb0=/fb1=/err=`:
`fb0/fb1` são readbacks do pixel sentinela logo após a cópia de CPU (volátil,
prova que a escrita pousou no mapeamento; valores esperados: `0xff181010`
fundo ou `0xff60ff60` barra — `0x0` seria buffer nunca escrito). O boot log
agora também registra o retorno de `sceVideoOutRegisterBuffers`.

**Como testar:** instale o PKG 00.91 (mesmo content id — vira update) e
rode ~1 min. Reporte **o que a TV mostra** + o `triaevum_boot.log` novo.

| TV | Diagnóstico | Próximo passo |
|---|---|---|
| Status **piscando** (alterna visível/preto) | onion visível, garlic invisível para CPU-escrita — **hipótese confirmada** | 0.92: os dois buffers onion (fix; custo de banda ~120 MiB/s, trivial) |
| Status **estável** | ambos visíveis; o preto do 0.90 tinha outra causa | conferir `fb0=/fb1=` no log e re-analisar |
| Preto total | nenhum buffer escaneável com escrita CPU | `fb0/fb1` corretos no log ⇒ cópia pousa e o scanout não lê ⇒ escalar (cópia via GPU/RADV ou alocação estilo probe) |

## Entregável (verificado no build)

| Item | Valor |
|---|---|
| PKG | `triaevum-ps4-vk/out/IV0000-TRIA00004_00-TRIAEVUMVKGAME00.pkg` |
| Tamanho | 94 MB (eboot 92 MB, 402 objetos, check-oelf OK) |
| SHA-256 | `bee33b33dcd2bb6b60a523106a46bd6827e031627df366a10c38c34863943a14` |
| SFO | APP_TYPE=1, CATEGORY=gd, APP_VER=**00.91** (build 0.91 — diagnóstico) |
| Conteúdo | o mesmo `/data/triaevum/game/` do app antigo (nada mudou no staging) |
| Shims | `ps4_compat.h` inteiro caiu (libc++ 21 tem bit_cast/concepts/abs(float)/stop_token); ficou só `-DTRIAEVUM_PS4_HOST=1` |

## O que este PKG faz

Igual ao marco 2026-10-19 do port antigo (fase 3.0/0.15), agora no runtime novo:

1. Abre 2 framebuffers 1080p no VideoOut e apresenta a **tela de status**
   (fonte 8x8): conteúdo, estado do módulo, entry points whole-AOT,
   frames/fps, contadores PICA/áudio/input/FS, log do módulo, heartbeat.
2. Lê `/data/triaevum/game/content.tap` (content-index do Forge já no console).
3. Sobe o módulo: 162.757 entry points whole-AOT esperados.
4. Roda o loop de frames a 30 Hz com PICA real (defer=false): o batch sink
   consome os draws (conta), transfers/fills rodam no caminho CPU, e as
   **completions publicam os interrupts PSC0/PSC1** (sem isso o GSP do guest
   trava).
5. Presenter de scanout por software (transpose portrait→landscape, 3x).
6. Áudio: valida e conta (descarta — saída real é a fase de áudio).
7. Input: DualShock via **scePad** (cross=A, circle=B, square=X, triangle=Y,
   options=START, touchpad=SELECT, L2/R2→ZL/ZR, gatilho > 64).

## Como testar (Daniel)

1. Instale o PKG (não substitui nenhum app antigo: TRIA00004 é novo).
2. **O conteúdo em `/data/triaevum/game/` continua valendo** — nada a refazer.
3. Abra o app. Esperado na TV, em ordem:
   - tela de status imediata (fundo azul-escuro, texto verde claro);
   - `content: content.tap NNNN bytes`;
   - `module: initialized` → **frames subindo a ~30 fps**;
   - `pica[gsp=...]` crescendo (~380/s) e `compl` crescendo (GSP vivo);
   - `audio[frames=...]` crescendo (DSP HLE produziando);
   - heartbeat de 1 s andando (barra verde).
4. Logs via FTP: `/data/triaevum_boot.log` (+ `/data/DolphinPS4/crash.log` se
   morrer — o crashlog do runtime DolphinPS4 pega rip/stack em offsets ELF).
   klog ao vivo (192.168.0.15:3232) também recebe as linhas `[triaevum]`.

## Cenários e o que reportar

| Sintoma | Significado | O que mandar |
|---|---|---|
| Status andando a ~30 fps por minutos | **MARCO FASE 1** | boot log (p/ registrar) + tempo até qualquer problema |
| Crash antes do status | heap/boot do runtime novo | crash.log + boot-trace.log + triaevum_boot.log |
| `run_frame failed (status N)` | memória (bad_alloc?) no novo heap | boot log completo + até qual frame chegou |
| fps < 25 | presenter software mais caro (sem SDL agora) | heartbeat + work_ms |
| Tela preta sem status | VideoOut falhou | triaevum_boot.log (linhas `videoout`) |
| Botões errados | mapeamento scePad | quais índices acendem na tela de título |

## Linhas-chave no boot log

- `direct memory total = 4xxx MiB` (identidade plain: ~4,5 GiB)
- `malloc 256MiB probe = ok` (heap dlmalloc/system flexible no lugar)
- `whole-aot program: abi=2 entries=162757`
- `pica host ready (regions=2)`
- `game module initialized - entering frame loop`
- `scanout screen=0 address=0x... format=1 stride=720` (o framebuffer do título)
- `heartbeat frames=N fps=30.x ...`

## Notas (para quem continuar)

- Os shims do `ps4_compat.h` morreram com a libc++ 21; o marcador
  `TRIAEVUM_PS4_HOST` (guards da árvore: filesystem canonização, getcwd do
  manifest, captura de geometria > 16 MiB) virou `-D` no CMake.
- Shadowing de headers vendored: `azahar_pica/common/{assert,common_types}.h`
  sombreia o do `azahar_audio`; os TUs de áudio (+ dsp_hle) recebem
  `azahar_audio` na frente via `INCLUDE_DIRECTORIES` por arquivo.
- O `__cxa_thread_atexit_impl` local saiu (o `ps4_runtime.cpp` fornece).
- O probe de alocação global saiu (o heap dlmalloc + malloc-replace substitui;
  `heap~NMiB` do heartbeat mostra a disponibilidade).
