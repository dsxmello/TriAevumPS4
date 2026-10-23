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
