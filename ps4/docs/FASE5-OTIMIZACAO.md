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
