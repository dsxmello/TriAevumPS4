# Fase 5 — otimização (plano, 2026-10-23)

Base: 1.27 sem PGO, vila -> Árvore Deku, render scale 3x. Log: 23–44 quadros
do jogo/s (38–74% do tempo real; 204.5 dsp frames/s = 100%). Quadro do host
nos piores trechos: ~49 ms, dos quais só ~33 ms aparecem nas fases medidas
(guest 30, dsp 1, frame start 1.4, visual 1).

Perfil (samples.log, thread `main`, 17 janelas de 10 s):
- **30% no libkernel (um único ponto, 25–38% por janela)**: a thread do jogo
  parada. Na 1.22 o kernel era 7%.
- Código gerado do jogo (AOT): ~8.5% visível + cauda longa (~50% das amostras
  ficam em buckets únicos e não aparecem listadas).
- mem* (memcpy 4.2, memset 1.1, memmove 0.4) ~5.7%.
- VFP em software (Oot3dVfpMultiplyAccumulate, VfpMultiply, RoundFinite,
  F32Mul, VfpBinary32Compare) ~1.6% visível.
- Frontend PICA (SubmitGspCommand, ApplyShader*SideEffects,
  BuildOot3dPicaCanonicalDrawIdentity, CaptureGeometryResource, hash) ~3%.
- Áudio HLE (AudioInterp::Linear, Source::MixInto, DecodeADPCM) ~1.1%.
- malloc/free/checkHeap ~0.5%.
- Thread `render`: 80% esperando; ajudantes `pica plan`: 98% esperando.

Causa dos 30%: o commit do upstream `9299fc31` ("sample input after the
previous frame ended", veio no sync beta.1.2) faz
`renderStream.WaitForFramesInFlight(0U)` no começo de cada quadro
(`oot3d_native_a32_window_frame.cpp`): o jogo espera o render terminar o
quadro anterior (CPU do render ~10 ms + GPU ~6 ms) antes de começar o
próximo, então jogo e render ficam em série. No PC isso reduz a latência de
entrada; no PS4, limitado pela CPU, custa ~16 ms por quadro.
`TRIAEVUM_INPUT_ADMISSION=0` volta ao modo em paralelo.

## Lista (ganho estimado de velocidade na vila)

| # | Item | Ganho estimado | Esforço | Mexe no PGO? |
|---|------|----------------|---------|--------------|
| 1 | Desligar a espera do quadro anterior no PS4 (`TRIAEVUM_INPUT_ADMISSION=0` no init) | **+25–45%** | minutos | não |
| 2 | ~~Compilar para o Jaguar (`-march=btver2`)~~ — **já estava ligado** (`refs/DolphinPS4/toolchain/ps4-love-style.cmake`) | 0% | — | — |
| 3 | VFP em software -> float nativo onde o resultado é idêntico | +2–6% | médio | sim (runtime header) |
| 4 | Cortar memcpy/memset (cópias de geometria/snapshots PICA) | +3–5% | médio | parcial |
| 5 | Frontend PICA (efeitos de registrador, identidade de draw, hash) | +2–4% | médio | parcial |
| 6 | Áudio HLE numa thread própria | +1–2% (e menos cortes no som) | médio | parcial |
| 7 | malloc/free e checkHeap no caminho quente | ~+1% | baixo | parcial |
| 8 | Gerador do AOT (entrada de bloco, flags, acesso à memória) | +5–15% | alto | sim, tudo |
| 9 | **PGO (por último, uma coleta só)** | **+20–30%** | 1 sessão de jogo | — |
| — | Afinidade de núcleos | 0% (piorou na 1.21) | — | — |
| — | Render scale menor | ~0% (GPU não é o limite) | — | — |

Projeção (estimativa, não medida): 1 sozinho -> vila ~55–75% do tempo real;
1+2+3..7 -> ~65–85%; tudo + PGO -> ~85–100%+. Os ganhos não somam de forma
linear (cada um reduz o tempo que sobra para os outros).

Ordem: 1 (testar já), 2, 4–7, 3, 8, e o PGO no fim.

## 1.28 — primeira rodada (2026-10-23)

- Item 1: `inputAdmissionMode = "0"` fixo no PS4
  (`oot3d_native_a32_window_frame.cpp`). Jogo e render voltam a trabalhar em
  paralelo (um quadro em voo).
- Item 2: nada a fazer. O `ps4-love-style.cmake` do DolphinPS4 já passa
  `-march=btver2` para tudo; o CMake descarta a opção repetida (por isso o
  ninja não recompilou nada quando ela foi adicionada no produto).
- Item 3: caminhos rápidos nativos em `oot3d_native_a32_vfp_ops.cpp` para
  VDIV, VSQRT, VCVT.S32/U32.F32 e VCVT.F32.U32 (mesmos limites do
  `oot3d_native_a32_vfp_fast.h`; resto vai para o software). Teste no PC:
  40 milhões de entradas aleatórias + casos de borda, 7 modos de FPSCR,
  resultado e flags idênticos à referência. VCMP e VCVT.F32.S32 já eram
  baratos. Só o .cpp muda: o código gerado e o perfil PGO dele não.
- Item 5: tabela constexpr dos registradores com efeito colateral em
  `ApplyShaderRegisterSideEffects` (a maioria das escritas sai no primeiro
  teste).
- Item 7: `PS4_HEAP_CHECKS=0` nos fontes do runtime do DolphinPS4 (era 1: a
  cada malloc/free, contador global + leitura do bloco de controle, escritos
  por todas as threads).
- Profiler: linhas `mem<- 0x...` com o ponto de chamada de memcpy/memmove/
  memset/memcmp (endereço de retorno no topo da pilha, conferido contra o
  `call rel32`; essas funções do `ps4_string.c` não usam pilha). Base do
  item 4.
- Itens 6 (áudio em thread) e 8 (gerador do AOT) ficam para depois da medição
  da 1.28: são os mais arriscados e dependem do quanto sobra.
- Overlay de desempenho (pedido do Daniel, para avaliar cada mudança): canto
  superior esquerdo, ligado por padrão, **L3+R3 liga/desliga**. Mostra FPS
  e frametime médio/máximo (intervalo entre quadros apresentados, janela de
  0,5 s), velocidade do jogo (quadros de DSP / 204,5; verde >= 97%, amarelo
  >= 75%, vermelho abaixo) com os quadros do jogo/s (60 = cheio), CPU da
  thread do jogo por quadro e GPU (% do tempo ocupada e ms por quadro: cada
  flush envia e espera a fence, então a espera é o trabalho da GPU).
  `OrbisDrawPerformanceOverlay` (orbis_imgui_platform.cpp), chamado do
  `Fast3dGui::DrawMenu`; números em `gOrbisPerformanceGame/Render`
  (gfx_orbis.h), publicados por `oot3d_native_a32_window_frame.cpp` e
  `gfx_orbis_rendering.cpp`.
- Correção do profiler: as funções do `ps4_string.c` usam frame pointer
  (`push %rbp`), então o retorno está em [rsp], [rsp+8] ou [rsp+16]; o
  `memCaller` testa os três contra o `call rel32`. Diff anexado a
  `patches/dolphinps4-ps4-runtime.patch`.
- PKG `out/...-1.28.pkg` (sem PGO), enviado para `/data/pkg`.

## Medição da 1.28 (2026-10-23)

- Daniel: GPU folgada em 1080p e em 4K (render scale até 6x); casas ~90%,
  vila "mal chega a 60%". RAM/VRAM com uso baixo.
- Log: casas 86–99% (1.27: ~72–90%), vila 37–66%. Thread do jogo: espera no
  kernel caiu de 30% para 1,3% (item 1 confirmado); agora ela trabalha o
  tempo todo e é o único limite. Na vila são 45–74 ms de CPU por quadro
  apresentado (2 quadros do jogo; o orçamento é 33 ms).
- Perfil da thread do jogo: memcpy 6,6% + memset 1,5% + memmove 0,6%,
  `CaptureGeometryResource` 3,9%, cauda longa do código gerado.
- Só 1,4% dos mem* foi atribuído: com -fPIC o `call` vai para o stub da PLT
  (ou pela GOT), não direto para o memcpy. 1.29 aceita qualquer `call rel32`
  ou `call *disp32(%rip)` logo antes do retorno.

## 1.29 — cache de geometria (troca RAM por CPU)

- O cache de snapshots de geometria do PS4 era de 512 entradas (desktop
  4096), por medo do heap. Na vila (~400 draws, vários vertex loaders cada)
  ele estourava: cada falta copiava o buffer de novo (memcpy) e varria o
  cache inteiro procurando a entrada mais velha (`min_element`).
- Agora 4096 entradas com teto de 160 MiB (`mGeometrySnapshotCacheBytes`,
  contado na inserção/descarte/restauração de savestate), e descarte em
  lote: o oitavo menos usado sai de uma vez (`nth_element`), em vez de uma
  varredura por falta.
- Log a cada 5 s: `[triaevum] geometry cache: N entries, X MiB | per second:
  hits, misses, MiB copied`.

## Medição da 1.29 (2026-10-23)

- Vila 25–32 quadros do jogo/s (42–53%), 330–420 draws, render scale 6x;
  áreas leves 50–57 (85–95%). Mesma faixa da 1.28: sem ganho.
- Cache de geometria: < 2 MiB, > 98% de acertos, 0,2 MiB/s copiados. Ele não
  era a origem do memcpy.
- Thread do jogo: kernel ~3%; mem* ~9,8%, agora quase tudo atribuído:
  construtor de **move** do `Oot3dPicaVulkanDrawPlan` ~4,4%, `operator=`
  (move e cópia) ~1,6%, `RunGuestRefresh`/`Oot3dPicaDrawSubmission`/
  `Capture` ~1,7%. O plano tem ~15,5 KB (~14 KB de estado inline copiado
  por memcpy a cada move) e cada draw era movido 4–5 vezes por quadro.
  As cópias de verdade só aparecem na interpolação visual (upstream).
- Endereços do sampler: `elf` = endereço do ELF; `jit` acima de 64 MiB do
  .text também é eboot (subtrair 0x400000). Agregador por função:
  símbolos do `build/product/triaevum_product` (llvm-nm do SDK).

## 1.30 — menos moves do plano de draw

- `oot3d_native_a32_window_frame.cpp`: o plano é usado no lugar (prebuilt
  ou local em `std::optional`), sem construir um vazio e mover para ele.
- `Capture`/`Append` com sobrecargas `const&` e `&&` (antes por valor: um
  move a mais em cada uma). Os testes continuam passando cópias.
- `FinishFrames`: quando um quadro leva todos os draws pendentes, o vetor
  inteiro é movido (um ponteiro) em vez de cada plano.
- Por draw: de 1 construção vazia + 4 moves para 1 move (o `push_back`).

## Medição da 1.30 (2026-10-23)

- Vila (> 300 draws): 32,0 quadros do jogo/s (53%) contra 29,3 (49%) na 1.29;
  25,9 ms de CPU por quadro do jogo (era 27,0); pica submit 9,9 -> 7,9 ms e
  plan 5,0 -> 4,0 ms por quadro do host. Poucas janelas na vila (5): ganho
  de ~+5–9%, dentro do estimado. Áreas leves sem mudança (76%).
- Thread do jogo: memcpy 7,5% -> 4,4%; construtor de move do plano
  4,5% -> 3,1%; `operator=` 0,95% -> 0,16%.
- O move que sobra é o único necessário (`Append` -> `push_back`) mais
  realocações do `mPendingDraws` quando um quadro passa do maior anterior.
  Para cortar mais é preciso encolher o plano (os ~12 KB inline dos shaders
  gerados/estado), não movê-lo menos.

## 1.31 — item 8, entrada de bloco do AOT

- Cada bloco do guest entra por `Oot3dAotEnterBlock`. Com o caminho do hook
  na mesma função ele não era inlinado: 10.329 cópias fora de linha e 167 mil
  chamadas no binário, cada bloco pagando call + 7 push/pop para só
  decrementar o orçamento e testar o filtro de PCs. Visível no perfil da
  1.30: 2,9% da thread do jogo (mais que todo o corpo gerado visível, 1,9%;
  ~69% das amostras ficam na cauda do código gerado).
- `oot3d_native_whole_aot_runtime.h`: orçamento + filtro inline
  (`always_inline` no ORBIS, como o `ReadFast`/`WriteFast`); a busca exata
  do PC e o hook vão para `Oot3dAotNotifyBlock` (`noinline`). Mesma
  semântica. Só o header muda: o código gerado não precisa ser regenerado.
- Binário: 0 cópias de `Oot3dAotEnterBlock`, eboot +15 MB. Recompila todos
  os shards (~8 min).
- Próximo do item 8 (precisa do C++ gerado de novo: as entradas do gerador,
  program/seleção/code.bin, não estão neste PC): LDM/STM com uma busca de
  página e um passo de geração para todas as palavras (~25 instruções e ~8
  acessos por palavra hoje).

## Medição da 1.31 (2026-10-23)

- Áreas leves (< 200 draws): 48,2 quadros do jogo/s (80%) contra 45,6 (76%)
  na 1.30; 17,8 ms de CPU por quadro do jogo (era 19,2): ~-7%.
- Vila: só 2 janelas (29,7 quadros/s): pouco para comparar.
- Perfil da thread do jogo: `EnterBlock` 2,9% -> 0 (sem cópias fora de
  linha). Sobrou 1,35% em `Oot3dAotNotifyBlock`, quase tudo na busca
  binária, em `BgCheck_LineTestImpl` e `FUN_004a0338`: blocos de laços
  quentes que caem em bits ocupados do filtro de 4096 bits (falso positivo
  a cada passagem).

## 1.32 — filtro de entrada de bloco maior

- `Oot3dAotBlockEntryFilter` com 65.536 bits (8 KiB) no PS4 (antes 4096):
  ~16x menos falsos positivos. Fora do PS4 o tamanho fica (faz parte da ABI
  do módulo do título). Recompila todos os shards.

## Medição da 1.32 (2026-10-23)

| Faixa | 1.29 | 1.30 | 1.31 | 1.32 |
|---|---|---|---|---|
| leve (< 200 draws) | 74% | 76% | 80% | **86%** (16,5 ms/quadro do jogo) |
| média (200–300) | 69% | 58% (2 jan.) | 60% (2 jan.) | **71%** (20,4 ms) |
| vila (> 300) | 49% (27,0 ms) | 53% (25,9) | 50% (2 jan.) | **54%** (25,3 ms, 11 janelas, 376 draws) |

- `NotifyBlock` 1,35% -> 0,22%: o filtro maior acabou com os falsos
  positivos. Entrada de bloco somada: 2,9% (1.30) -> 0,2%.
- Na vila a thread do jogo continua em ~50 ms por quadro apresentado; o que
  sobra visível: mem* 7,2%, frontend PICA 5,3%, VFP 3%; ~70% na cauda do
  código gerado (acesso à memória do guest: próximo passo do item 8).

## Medição da 1.45 com profiler (2026-10-24)

Meta (Daniel): ~16 ms de CPU e de GPU por quadro apresentado (jogo a 30 Hz
interpolado para 60; GPU em 4K: 14–17 ms).

| | Interiores | Vila |
|---|---|---|
| Quadros apresentados | 25–32/s | 15–21/s |
| Thread do jogo | ~27 ms/quadro, 17% parada no kernel | ~50 ms/quadro, sem espera |
| Thread de render | 67% parada: ~40% esperando a GPU, ~23% esperando trabalho | parecido |
| GPU | 15–18 ms | 16–18 ms |

- Interiores: o limite é o envio síncrono. O backend submetia o quadro e
  esperava a fence (`gpu wait` 15–17 ms) antes de gravar o próximo: CPU de
  render (~11 ms) + GPU (~17 ms) em série, ~31 fps; a thread do jogo espera
  a de render.
- Vila: o limite é a thread do jogo (mem* ~9%, frontend PICA/plano ~6,6%,
  VFP ~3,4%, o resto no código AOT).

## 1.46 — GPU em paralelo com a CPU de render

- `Ps4VkPicaRenderBackend::SetAsyncFrames`: dois slots de quadro (command
  pool e buffers, fence, descriptor pool, metade do ring de 64 MiB cada). O
  `EndFrame` submete sem esperar, troca de slot e espera só o quadro
  anterior daquele slot. Flushes no meio do quadro (leitura de
  materialização, ring cheio, troca de render scale) continuam síncronos;
  a GPU executa em ordem numa fila só, então render targets e texturas não
  precisam de duplicação.
- `Ps4Display::SetDeferredFlips`: o flip de um quadro sai no `Flip` do
  quadro seguinte (quando o `EndFrame` já esperou por ele); 4 buffers de
  scanout (era 3) e `WaitPendingFlip` permite um flip a menos em voo.
  +1 quadro de latência (aceito pelo Daniel).
- Texturas de UI descartadas são destruídas um quadro depois.
- O `gpu wait` do log/overlay passa a medir só a espera da CPU, não o tempo
  da GPU.
- Estimativa nos interiores: ~28 ms (CPU+GPU em série) -> ~17 ms (o maior
  dos dois); o limite passa a ser a thread do jogo.

## Medição da 1.46 (2026-10-24)

- Interiores: 29,3 -> **35,9** quadros apresentados/s (picos de 41); espera
  pela GPU por quadro 16,2 -> 1,5 ms; thread do jogo 28,1 -> 22,9 ms por
  quadro do host. A thread de render passa 55% esperando trabalho: o limite
  agora é a thread do jogo (sessão sem vila).
- mem* na thread do jogo: construtor de move do plano de draw 2,2%,
  realocação do vetor de planos 0,5%.

## 1.47 — plano menor e LDM/STM em lote no AOT

- `Oot3dPicaVulkanDrawPlan::UberFragmentShader` vira só identidade
  (`StateKey`, `SourceIdentity`, `Hooks`): os uniforms (com a tabela de
  neblina e a iluminação) e o `Source` do shader uber nunca eram lidos. Cópia
  por move: ~14,6 KB -> ~8,4 KB.
- Item 8, LDM/STM: `NativeA32Memory::ReadWordsFast`/`WriteWordsFast<N>` (uma
  busca de página e, na escrita, um passo de geração de N, o mesmo estado de
  N `WriteFast`; recusam em página lenta, divisa de página ou trace;
  `NativeA32UnobservedMemory` sempre recusa). O gerador
  (`whole_aot_cpp.py _emit_block_transfer`) emite o lote para 2+ palavras
  com o código por palavra como fallback; `ps4/tools/aot_batch_block_transfers.py`
  aplica o mesmo formato aos shards já gerados (`title-content/aot-cpp`;
  originais em `title-content/aot-cpp.orig`): 18.595 LDM e 18.220 STM,
  166.262 palavras, 37% dos acessos estáticos à memória do código gerado.
  Idempotente.

## Medição da 1.47 (2026-10-24)

- Interiores 35,9 -> **43,1** fps (trechos de 55–58); médias (~210 draws)
  21,3 -> **24,4** fps. Nas áreas pesadas a thread do jogo trabalha o tempo
  todo (espera 0,2–0,6%) e a de render fica 60–70% parada: o limite é a
  thread do jogo. Visível nela: frontend PICA 5,6%, mem* 4,5%, VFP 3,6%,
  funções nativas à mão 3,3% (`Mtx3x4Multiply`, VFP exato em double),
  áudio 2%; ~65% é a cauda do código AOT, abaixo do corte do profiler.
- Decisão (Daniel): foco no código AOT do jogo.

## 1.48 — profiler sem corte

- `ps4_sampler.cpp` (DolphinPS4, diff anexado ao patch): toda bucket de 16
  bytes de cada janela, com três casas (uma amostra = 0,010%); antes só as
  com 2+ amostras e no máximo 1500, o que escondia ~2/3 da thread do jogo.
  Para achar as funções do jogo mais quentes e o peso dos acessos à memória
  dentro delas.

## Medição da 1.48 com perfil completo (2026-10-24)

- Sessão até a Árvore Deku. Interiores 42 fps, médias (~185 draws) 26 fps.
- Áreas pesadas (>= 140 draws), thread do jogo: código gerado 51% em 1415
  funções, muito espalhado (top 10 = 10%, top 50 = 23%, top 200 = 37%):
  reescrever poucas funções à mão rende pouco.
- Classificação por instrução das amostras nas 300 funções geradas mais
  quentes (41% da thread): leituras/escritas dos registradores do guest em
  memória (`state.R`) 36%, acessos à memória do guest 33%, entrada de bloco
  16%, cálculo/desvios 13%, chamadas 2%.

## 1.49 — registradores do guest em locais

- Os registradores eram referências para `state.R[]`: cada instrução do
  guest ia à memória, e depois de cada escrita na memória do guest (ponteiro
  de byte, que pode apelidar tudo) o compilador relia todos. O modo
  `local_state` do gerador os mantém em locais e grava/relê nos pontos de
  saída; o comentário do upstream diz que no desktop isso piorou o
  workload, aqui a medição decide.
- `ps4/tools/aot_promote_registers.py` aplica a forma do `local_state` aos
  shards já gerados (os lambdas `commitState`/`reloadState`, já chamados em
  todo return, chamada direta/externa/indireta e hook, passam a gravar e
  reler): 12.414 de 12.439 funções. Antes dela: `title-content/aot-cpp.batched`.
  Regenerar no futuro: `generate(..., local_state=True)`.
- eboot 206 -> 244 MB (gravação/releitura em cada saída e chamada).

## Medição da 1.49 (2026-10-24)

- Pior: interiores 42,2 -> 32,4 fps, médias 25,6 -> 22,1 fps. Revertido aos
  shards `aot-cpp.batched`.

## `__restrict` no state (não enviado)

- `ps4/tools/aot_restrict_state.py` marca `Oot3dAotArchitecturalState&
  __restrict state` nos shards (54.999 parâmetros); o gerador já emite assim.
- Sem efeito: compilado com e sem, o shard 000 tem 9.956 contra 9.961
  leituras e 8.400 contra 8.407 escritas. O `state` escapa (é passado aos
  hooks de entrada de bloco e às chamadas), então a análise de alias não
  pode usar o `noalias`; o assembly lê e grava `state.R[]` a cada instrução
  do guest (`addl $-44, 52(%r14)`), não só depois das escritas na memória.
  A versão não foi para o console: o código é praticamente o da 1.48.

## 1.50 — cache de dispatch, `Mtx3x4Multiply` em AVX, entrada de bloco mais curta

- Cache de dispatch: `FindDispatchEntry` (`oot3d_whole_aot_generated.cpp`)
  fazia busca binária numa tabela de 162.757 entradas (2,6 MB) a cada
  retorno, desvio e chamada indireta; as sondagens erravam o cache (1,3% da
  thread do jogo no perfil da 1.48). Agora um cache direto por thread de
  1024 entradas (pc -> índice, chave `~pc`, faltas também guardadas) fica na
  frente da busca. O gerador emite assim (`whole_aot_cpp.py`) e
  `ps4/tools/aot_dispatch_cache.py` aplica o mesmo texto ao arquivo já
  gerado (idempotente, `--check`).
- `TryFastMtx3x4Multiply` com AVX: os quatro componentes de uma linha de
  uma vez, com as mesmas contas em double e o mesmo arredondamento do
  caminho escalar; entradas e valores arredondados conferidos no fim (algum
  valor fora do caminho rápido recusa a chamada inteira). Teste no PC: 20
  milhões de matrizes aleatórias (com zeros, denormais, inf/NaN e extremos),
  resultado, flag de inexato e recusa idênticos ao escalar.
- Entrada de bloco (item 8): cada bloco testava hook nulo, pulo do primeiro
  bloco, lista vazia, filtro nulo e o filtro. No PS4 o contexto guarda
  `ActiveBlockEntryFilter`, resolvido no primeiro bloco (antes dele um
  filtro cheio manda esse bloco ao caminho lento, que resolve e aplica o
  teste exato do primeiro bloco): o caminho inline vira orçamento + um
  ponteiro + um bit. Mesma semântica.
- Shards: os da 1.48 (`aot-cpp.batched`) com `__restrict` e o cache de
  dispatch. Recompila todos.

## Medição da 1.50 (2026-10-24)

- Sessão na Árvore Deku e arredores, comparada com a última da 1.48 (só
  janelas com o jogo rodando: >= 40 quadros do jogo/s e >= 20 draws; as de
  menu/pausa/carregamento ficam de fora):

| Draws | 1.48 fps | 1.50 fps | 1.48 thread do jogo | 1.50 thread do jogo |
|---|---|---|---|---|
| < 100 | 56,7 | 54,2 | 12,3 ms | 12,4 ms |
| 100–140 | 43,1 | 46,0 | 18,6 ms | 17,6 ms |
| 140–180 | 28,5 | **30,0** | 30,4 ms | **27,6 ms** |
| 180–250 | 25,2 | **25,6** | 33,7 ms | **32,5 ms** |

- Ganho pequeno e no sentido certo: -4% a -9% de CPU na thread do jogo nas
  áreas pesadas. Abaixo de 100 draws o jogo já fica perto dos 60.
- Perfil: `FindDispatchEntry` 1,3% -> 0,12%; `Mtx3x4Multiply` 0,15%;
  `Oot3dAotNotifyBlock` ainda 1,1% (com `ObserveBlockEntry` 0,6%): hooks de
  verdade, a investigar. Código gerado 54% da thread.

## 1.51 — acesso à memória do guest mais curto

O `ReadFast`/`WriteFast` inline no código gerado (1.50, `BgCheck_LineTestImpl`):
leitura ~17 instruções e 5 loads, escrita ~24 instruções, 7 loads e 4 stores.
Três cortes, mesma semântica:

- Leitura: o caminho lento preenche um valor próprio. Antes o endereço do
  valor do chamador ia para a chamada lenta, e o resultado de toda leitura
  rápida passava pela pilha (store + reload).
- Trace (só no PS4, `kFastPagesMayBeTraced = false`): com o trace ligado
  nenhuma página fica rápida (`RebuildPageTable`, `ReleaseDeferredPageTraps`;
  `EnableWriteTraceFingerprint` refaz a tabela quando muda), e todo acesso
  vai pelo caminho checado, que já registra o trace. O inline não testa mais
  a flag (um load + desvio por acesso). Fora do PS4 a leitura traçada
  continua conferida contra o caminho checado.
- Geração: `mRangeGeneration` (= geração arquitetural + época dos
  certificados) é o único contador que a escrita inline incrementa; a
  arquitetural (exclusivos, save state, fingerprint) vira
  `mRangeGeneration - mRangeGenerationBias`. Antes: incremento de
  `mWriteGeneration` + load do bias + soma.

Teste no PC (diferencial): a memória antiga (git), a nova e a nova com a
regra do trace do PS4 compiladas lado a lado, 60 sementes x 60 mil operações
aleatórias (leituras/escritas de 1–8 bytes com divisas de página, LDM/STM,
`WriteBytes`, exclusivos, `RangeWriteGeneration`, cópia, move, save/restore
de estado, trace, watch, observadores, escritas adiadas): ~9,9 milhões de
resultados idênticos. Única diferença, esperada: com a regra do PS4, depois
de ligar o trace, `RangeWriteGeneration` devolve gerações mais novas (a
tabela refeita marca as páginas como escritas agora; caches só invalidam a
mais).

Binário: eboot 193 -> 173 MB; `BgCheck_LineTestImpl` 15.105 -> 12.949
instruções (-14%: 245 testes de trace, 96 somas de bias e as idas à pilha
saíram). Só o runtime muda; recompila todos os shards.

## Medição da 1.51 (2026-10-24)

Duas sessões (a primeira curta, quase toda na vila). Métrica: CPU da thread
do jogo por quadro do jogo (ms por quadro do host x quadros do host/s /
quadros do jogo/s), só janelas com >= 40 quadros do jogo/s e >= 20 draws:

| Draws | 1.48 | 1.50 | 1.51 (2ª sessão) | fps 1.50 -> 1.51 |
|---|---|---|---|---|
| 100–140 | 13,2 ms | 12,6 ms (11 jan.) | **11,7 ms** (17 jan.) | 46,0 -> 52,6 |
| 180–250 | 16,9 ms | 16,3 ms (22 jan.) | **14,5 ms** (10 jan., 224 draws em média contra 203) | 25,6 -> 29,7 |
| >= 250 | — | 17,9 ms (3 jan.) | **16,4 ms** (17 jan.) | 23,2 -> 24,7 |

- -7% a -11% de CPU por quadro do jogo nas áreas pesadas, mesmo com mais
  draws em média. Sem erros no log.
- 140–180 draws quase não apareceu nas sessões da 1.51.

## 1.52 — entrada de bloco com um contador só

A entrada de cada bloco do guest no PS4 (1.51): lê e grava `BlocksRemaining`,
incrementa `BlocksConsumed` na memória (ler-modificar-gravar), lê o filtro
ativo, testa se é nulo e testa o bit. Os dois contadores não ficam em
registrador porque toda escrita na memória do guest pode apelidar o contexto.

- `BlocksConsumed` derivado: todo consumidor de blocos tira de
  `BlocksRemaining` (entrada de bloco, regiões de memória, laços de região do
  gerador), então consumido = orçamento inicial - restante. O contexto
  guarda o orçamento (`BlockBudget`, inicializador padrão que copia
  `BlocksRemaining`: os pontos que constroem o contexto não mudam) e quem lê
  usa `Oot3dAotBlocksConsumed(context)`. A entrada de bloco deixa de
  incrementar `BlocksConsumed` no PS4.
- Sem hook, o filtro resolvido é um filtro vazio em vez de nulo: cada bloco
  testa só o bit.
- O orçamento continua exato (quantum dos workers em segundo plano, saída
  `BlockLimit` com a contagem).
- Código gerado: `ps4/tools/aot_blocks_consumed.py` troca as leituras de
  `context.BlocksConsumed` em `oot3d_whole_aot_generated.cpp` (idempotente,
  `--check`); o gerador já emite assim. Os shards não leem os contadores.
- Perfil da 1.51 (hooks de entrada de bloco): `NotifyBlock` 1,1%, rastreador
  de composição PICA 0,8%, sonda de iluminação 0,2%, mais o despacho:
  ~3% da thread do jogo em hooks que fazem trabalho de verdade. Fica para
  um item próprio.

## Medição da 1.52/1.53 (2026-10-24)

- 1.52: sem diferença clara contra a 1.51 pela contagem de draws, mas a
  sessão foi quase toda em Hyrule Field e as da 1.51 na Floresta Kokiri:
  áreas diferentes com draws parecidos não se comparam. O ruído entre
  sessões da mesma versão também é grande (três sessões da 1.51: 16,7–17,3
  ms estimados em 300 draws).
- 1.53 = 1.52 + ID da cena no fim de cada `[triaevum] game:` (`| scene N`,
  `PlayState + 0x104` como a scene view lê; -1 antes do primeiro
  PlayState). A partir daqui as comparações são por cena.
- A/B por cena com o mesmo roteiro: 1.51s (a 1.51 com só o log da cena;
  pkg `...-1.51s.pkg`) contra a 1.53. Dentro da cena os draws variam muito
  com a câmera (Kokiri: média 372 contra 293), então a comparação é pela
  reta ms x draws da cena:

| Cena | 1.51s | 1.53 |
|---|---|---|
| 85 Floresta Kokiri, 250 / 300 draws | 16,5 / 17,2 ms | **15,6 / 16,4 ms (-5%)** |
| 81 Hyrule Field, 250 / 300 draws | 16,9 / 17,3 ms | 15,3 / 16,3 ms (inclinações bem diferentes: inconclusivo) |

- Kokiri tem a mesma inclinação nas duas (custo por draw), então os -5%
  valem na faixa toda. Fica.
- Ferramenta: `perscene.sh`-style (scratch) = CPU da thread do jogo por
  quadro do jogo por cena; para A/B, reta por cena e comparação no mesmo
  número de draws.

## 1.54 — frontend PICA: escrita de registrador inline, hash do arquivo de registradores memoizado

Perfil da 1.51s (thread do jogo): frontend PICA ~10% — `SubmitGspCommand`
3,2%, `BuildOot3dPicaCanonicalDrawIdentity` 2,4%, efeitos de registrador
2,5%, identidade visual 0,9%; cópia do pacote de draw ~1,4% (o pacote tem
~10 KB e é copiado duas vezes por draw: frontend e fila).

- `applyWrite` (uma vez por escrita de registrador da command list) não era
  inlinado: carregava o pacote do draw no próprio frame (~11 KB de pilha),
  e toda escrita pagava chamada + 5 push/pop + a chamada de
  `ApplyShaderRegisterSideEffects`. O disparo do draw vai para um lambda
  `noinline` (`submitDraw`), e a tabela `kShaderSideEffectRegisters` é
  testada antes da chamada (a função testa a mesma); `applyWrite` vira
  `always_inline` (usado em 3 pontos do laço, não era inlinado). Mesma
  semântica.
- `HashRegisterFile` (ID do arquivo de registradores, valor exato
  preservado — vai para registros do renderer e para a validade da scene
  view): já retomava do primeiro bloco de 32 palavras alterado, mas todos
  os blocos depois dele eram dobrados byte a byte (4 multiplicações de 64
  bits encadeadas por palavra). No FNV-1a o byte baixo do estado só depende
  do byte baixo anterior, então num bloco de n bytes
  `s_out = s_in * P^n + C(L_in, bytes)` e `L_out = f(L_in, bytes)`: um bloco
  inalterado vira uma multiplicação e uma soma, memoizado por bloco e
  `L_in` (256 entradas por bloco, invalidado por geração quando o bloco
  muda; ~100 KB por thread no heap, não em TLS estático).
  Teste no PC: 400 mil draws simulados (poucos registradores mudando por
  draw, trocas do arquivo inteiro), 0 diferenças contra o FNV direto;
  1,42 -> 0,34 µs por draw (4,1x) com `-march=btver2`.

## Medição da 1.54 e 1.55 — hashes por draw memoizados (2026-10-24)

- 1.54 no console: Kokiri igual à 1.53 (15,6 / 16,5 ms em 250 / 300 draws);
  Hyrule Field com 9 janelas, inconclusivo. Perfil contra a 1.51s:
  `SubmitGspCommand` 3,21% -> 2,28%, `ApplyShaderRegisterSideEffects`
  1,68% -> 1,11% (~1,5% da thread, dentro do ruído do tempo por quadro);
  `BuildOot3dPicaCanonicalDrawIdentity` 2,37% -> 2,32%: o laço quente não
  era o arquivo de registradores. Pelo deslocamento no pacote (layout
  calculado no PC), os laços liam `ProcTexLuts` (0x1C38) e os registradores
  0x140–0x1DA: `BuildDynamicStateId` hasheia por draw os 96 uniforms float
  do vertex shader (384 palavras), os atributos padrão (64), a LUT de
  neblina (128) e os registradores de iluminação (154), e
  `BuildFragmentProgramId` as LUTs de proctex (1.024) quando usadas.
- 1.55: `WordFoldMemo` generaliza a memoização da 1.54 (blocos de 32
  palavras, passo afim por byte baixo de entrada, mesmo valor) para
  qualquer arranjo hasheado por draw; um memo por ponto de chamada e por
  thread (`OOT3D_PICA_WORD_FOLD_MEMO()`), nos 9 pontos acima. Teste no PC:
  600 mil dobras (tamanhos 0–1000, fora de múltiplos de 32, mudanças
  esparsas e totais, estados de entrada variados), 0 diferenças; uniforms
  com 2 vec4 mudando por chamada: 1,40 -> 0,18 µs (8x).
- Medição da 1.55: perfil contra a 1.51s — `BuildOot3dPicaCanonicalDrawIdentity`
  2,37% -> 1,29%, `SubmitGspCommand` 3,21% -> 2,55%,
  `ApplyShaderRegisterSideEffects` 1,68% -> 1,41% (~2 pontos do frontend).
  Tempo por quadro do jogo por cena (reta ms x draws, 250 / 300 draws):
  Kokiri 16,1 / 16,7 ms (1.53: 15,6 / 16,4), Hyrule Field 14,9 / 16,6 ms em
  35 janelas (1.53: 15,3 / 16,3). Ganhos de ~2% somem no ruído entre sessões
  (±0,5 ms nas retas): ficam pelo perfil. Para medir ganhos pequenos é
  preciso um trecho repetível (mesmo lugar e câmera por um minuto).

## 1.56 — plano de draw menor, tabela de páginas plana, contagem dos hooks

- mem* na thread do jogo (perfil da 1.55, chamadores atribuídos): o move do
  `Oot3dPicaVulkanDrawPlan` 2,6%, o `resize` que constrói os planos
  prontos 0,7%, `operator=` 0,2% — ~4%, mais que as cópias do pacote
  (submissão 0,9%, frontend 0,6%). O plano tinha 9.680 bytes, 6.112 nos
  uniforms do fragment shader, 3.616 deles no `ProcTexProgram` (LUT de 3,5
  KB, toda zero em draw sem proctex).
  `Oot3dPicaProcTexUniform`: registradores inline, LUT compartilhada e
  imutável (nula quando zero; a anterior é reaproveitada enquanto não
  muda), `WriteProgram` grava os mesmos bytes std140 no uniform buffer.
  64 bytes em vez de 3.616: plano ~6,1 KB, cada move e construção ~37% menor.
  Teste no PC: 200 mil programas (LUT zero, compartilhada, mudando), bytes
  idênticos ao programa; cópia que escreve (`MutableLut`) não afeta a outra.
- Tabela de páginas da memória do guest plana: uma entrada por página (1M x
  24 bytes, alocada no primeiro rebuild) em vez de grupos de 1 MiB. O acesso
  inline carrega o ponteiro da tabela (independente do endereço) e a entrada;
  antes carregava o grupo indexado pelo endereço, testava e então a entrada.
  Só as páginas das regiões mapeadas são preenchidas (`mFastPageSpans`) e o
  rebuild limpa só elas. Teste diferencial da 1.51 (antiga, nova, nova com a
  regra do trace do PS4; 60 sementes x 60 mil operações): idêntico.
- Hooks de entrada de bloco (~5% somando `NotifyBlock`, a política e os
  observadores): o despacho tem centenas de linhas e condições; antes de
  mexer, a contagem por PC. A cada 5 s o log ganha
  `[triaevum] block-entry hooks: N calls/s | top: PC=chamadas/s ...`.

## Medição da 1.56 (2026-10-25) — A/B parado

Método novo: ~60 s parado no mesmo ponto, câmera fixa (Kokiri: porta da
casa do Link; Hyrule Field: mesmo ponto nas duas). Desvio padrão por janela
0,02–0,09 ms (no roteiro livre as retas variavam ±0,5 ms).

| Cena | 1.55 | 1.56 |
|---|---|---|
| 85 Floresta Kokiri | 17,46 ms, 353 draws, 22,9 fps | **17,25 ms, 355 draws, 23,7 fps** |
| 81 Hyrule Field | 19,48 ms, 394 draws, 21,3 fps | 19,42 ms, 384 draws, 21,8 fps |

- Kokiri: -1,2% de CPU por quadro do jogo com 2 draws a mais, +3,5% fps.
  Hyrule: 10 draws a menos na 1.56; corrigido pela inclinação (~0,02–0,035
  ms por draw) fica empatado. Sem regressão; fica.
- Hooks de entrada de bloco: 10–15 mil chamadas/s, ~5% da thread: 3–5 µs
  por chamada. Os PCs mais chamados (~2 mil/s cada) são de submissão de
  render (`MeshCommandPacket_Submit` 0x00466E2C, `CmbRenderer_SubmitDrawHandle`
  0x0030F4D0, `ModelPreview_SetSlotTransformRows`/`SetSlotTranslation`,
  `oot3d_field_80x4_positive`, 0x00313444, 0x003FBC5C, 0x003FB5EC,
  0x003FB998), quase todos do rastreador de composição, e cada chamada
  percorre a política inteira (sonda de visibilidade de atores, dezenas de
  comparações e buscas). Próximo: caminho rápido para esses PCs.
