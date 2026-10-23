# Fase 3 — O produto completo no PS4 (TopScreen, menu F1, freecam)

Data: 2026-10-22. Pedido do Daniel após o 0.93: single screen (mod TopScreen),
o menu F1 do TriAevum no L3+R3 (gráficos, freecam etc.) e nada de segunda
tela.

## Por que o PS4 não tem isso hoje

O PS4 replica o **module host** (`triaevum_oot3d_module_host.cpp`): o jogo
como módulo + serviços + renderer PICA. Esse host é um harness. O produto
que o desktop distribui é outro executável, o **`oot3d_native_game`** (a
"sessão de janela", `tools/oot3d/native_game_runtime/oot3d_native_a32_window*`,
~8 mil linhas + bootstrap), e é ele que tem:

- **TopScreen / single screen**: o candidate dispatch intercepta funções de
  UI do jogo (63 contratos em `ui_contract`), a
  `Oot3dNativeUiLifecycleBridge` reconstrói o HUD em primitivas de UI, o
  `Oot3dNativeA32UiTextureProvider` resolve as texturas do pack
  (`atlas_overrides.o3tu`, `zelda_link_ext.zar` — os que estão em
  `refs/data/mods/topscreen/`) e o `N64UiRenderer` desenha sobre o frame;
- **F1**: janela ImGui do framework "ship" (`runtime/three_ds_recomp/src/ship/
  window/gui`, `fast/oot3d/graphics_settings_window.cpp`,
  `oot3d_top_screen_settings_panel.cpp`), lida/salva em `config/TriAevum.json`;
- freecam, FOV/widescreen, controles configuráveis, savestates, interpolação.

O módulo do jogo só aplica os patches de layout do TopScreen ("TopScreen
profile active"), por isso o HUD não aparece no PS4.

## O precedente: o Switch já roda o produto

`scripts/switch/build-product.sh` + `docs/OOT3D_SWITCH_WHOLE_AOT_PORT.md`: o
`oot3d_native_game` inteiro em homebrew, com SDL2 + **OpenGL** no lugar do
NRI, TopScreen com a arte SingleScreen 3.0 que o repo embarca
(`runtime/triaevum_resources/topscreen/singlescreen-3.0/custom_ui01.ctxb`),
gameplay 30 Hz sem interpolação. São 45 guards `__SWITCH__` de cola de
plataforma. Os efeitos Vulkan-only do desktop (AO, SSSR, TAA/upscalers, GI,
sombras virtuais) **não** existem no backend do Switch, e não vão existir no
PS4 nesta fase.

## Plano (PS4 = "Switch com Vulkan")

1. **Janela/plataforma `__ORBIS__`**: espelhar os 45 guards do Switch
   (caminhos `/data/triaevum/...`, boot, pacing 30 Hz), backend de janela
   Fast3D próprio (VideoOut + scePad) no lugar do `gfx_sdl2`.
2. **`GfxRenderingAPI` para PS4**: o backend Vulkan atual (`ps4_vk_pica_backend`)
   já cumpre a parte PICA; falta a parte Fast3D (texturas, programas do
   combinador, `DrawTriangles`) que o `N64UiRenderer` (HUD do TopScreen) usa,
   portada do `gfx_vulkan.cpp`/`gfx_opengl.cpp` do desktop.
3. **ImGui** com `imgui_impl_vulkan` (já vendored) e navegação por controle:
   o F1 abre no L3+R3; aparecem as opções que o PS4 de fato tem (TopScreen,
   câmera/FOV, freecam, controles, resolução). Os efeitos NRI ficam ocultos.
4. **Conteúdo**: staging do pack TopScreen e das configs em
   `/data/triaevum/` (o `refs/data` do Daniel tem tudo).

Ordem: 1 → 2 → TopScreen visível → 3. Cada etapa sai com PKG + log.

## Antes disso: 0.94 (lentidão)

Causa encontrada: o host desktop roda o módulo em passos de **1/60 s**
(`kSimulationStepNanoseconds = 16'666'667`) com drain + poll da GPU a cada
passo. O PS4 rodava 1 passo de 1/30 s por frame → o jogo só conseguia uma
ida e volta à GPU (submit → interrupção de término) a cada 2 vblanks e
renderizava pela metade: câmera lenta com o host a 30 fps cravados, igual em
1× e 4× (a GPU gasta ~1,5 ms/frame; o gargalo nunca foi gráfico).

0.94: 2 sub-passos de 1/60 s por frame apresentado, cada um com
poll → run_frame → drain (ordem do desktop); flip sem bloquear (espera só
antes de reescrever o buffer que está na tela); `work_ms` passa a medir só o
trabalho (antes incluía o sleep). Menu sem a opção de duas telas.

PKG 00.94: `37b60cf105c607ce1d2dce9af4246794cbb4be315574b0731d65cea2fc60a7d4`.

## Resultado do 0.94 em console (2026-10-22)

Lentidão do jogo resolvida: o título agora gera ~57 draws por passo de 1/60 s
(antes ~19; o gargalo de "uma ida à GPU por 2 vblanks" sumiu). Mas o
frame apresentado custa **37–43 ms de CPU** (2× run_frame + drain) contra o
orçamento de 33 ms → 17–19 fps. GPU ≈ 2,5 ms/frame: o limite é CPU (A32
AOT + frontend PICA + plan/bridge no Jaguar). Precisa de profiling por fase
(run_frame vs. drain vs. backend) antes de otimizar; o mesmo custo existe no
produto completo (mesmo processo A32), então vale nos dois caminhos.
`display transfer source missing` (14×, endereço 0x182447c0): transfer que
lê um alvo ainda não desenhado na sessão; investigar junto.

## Levantamento do produto para PS4 (2026-10-22)

Fecho de alvos extraído do CMake do próprio TriAevum (configure no host,
modo `OOT3D_DIRECT_AOT_PLUGIN=ON`, Vulkan/NRI OFF = closure do Switch, via
CMake File API: `tools/product/closure.py`). Pacotes do host substituídos por
configs stub só para configurar. **43 alvos, 585 fontes** (sem os shards),
sendo `three_ds_recomp_runtime` (ship + Fast3D + áudio + controles) 312.

`tools/product/syntax_survey.py` compila cada fonte com o toolchain PS4
(clang + libc++ 21, alvo FreeBSD) em `-fsyntax-only`:

| Rodada | OK |
|---|---|
| crua | 464/584 (112 = spdlog ausente, 5 = headers Vulkan) |
| + spdlog 1.14.1 (tag 27cb4c76, sha256 1586508029a7…) + headers Mesa | 469 (113 = `consteval` do fmt 10 embutido com clang moderno) |
| + `-DFMT_CONSTEVAL=` | **581/584** |

Restam: `gfx_opengl.cpp` (sai: o PS4 tem backend próprio) e dois usos de
`std::filesystem::file_time_type` (rep `__int128` na libc++ do PS4:
`to_string` ambíguo, sem `clock_cast`) — correções de uma linha sob
`TRIAEVUM_PS4_HOST`. Ressalva: o levantamento usou os headers SDL2 do host
(`/usr/include/SDL2`); o build real usa o SDL2 2.0.18 do PacBrew
(`ps4-openorbis-sdl2-2.0.18-16`, sha256 d447992b…, já baixado: driver de
áudio `sceAudioOut`, joystick; o vídeo dele é Piglet e não será usado).

Fonte do TriAevum versionado à parte para gerar o patch da fase 4:
`git --git-dir=triaevum-ps4-vk/triaevum-src.git --work-tree=TriAevum`
(baseline `dd2de71` = arquivo b20ff67; o `.git` fica fora da árvore para
não mudar o hash de commit que o CMake do TriAevum lê).

### Próximas etapas concretas

1. Build do produto no PS4 a partir do `closure.json` (lista gerada, não à
   mão), com spdlog/SDL2 do PS4 e os deps do FetchContent (imgui
   v1.91.9b-docking com o patch do TriAevum, prism, monocypher, zstd,
   thread-pool) vendorizados com hash.
2. `WindowBackend::FAST3D_ORBIS_VULKAN`: `GfxWindowBackendOrbis` (VideoOut +
   SDL joystick) e `GfxRenderingAPIOrbis` = backend PICA atual + parte Fast3D
   (portada do `gfx_vulkan.cpp`) + `imgui_impl_vulkan`.
3. Guards `__ORBIS__` ao lado dos 45 `__SWITCH__` (caminhos `/data/triaevum`,
   boot status, 30 Hz) + AOT estático no lugar do plugin.
4. Staging: pack TopScreen e configs de `refs/data` → `/data/triaevum/`.

## Progresso da etapa 1–3 (2026-10-22, sessão interrompida por limite de uso)

Feito:
- Deps vendorizadas com commit em `src/deps/MANIFEST.txt` (imgui+patch, prism,
  monocypher, zstd, thread-pool, stb); spdlog 1.14.1; SDL2 PS4 em `sysroot/sdl2`.
- `tools/product/{closure,gen_cmake,syntax_survey}.py`, `ports/ps4-product/`
  (CMake + `generated/`), `tools/build-product.sh`. Primeira compilação: 844/847
  objetos (shards incluídos); as 3 falhas já foram corrigidas (install_config.h
  gerado; `file_time_type` sob `TRIAEVUM_PS4_HOST`). Ainda não relinkado.
- Patch no TriAevum (rastreado em `triaevum-src.git`): `gfx_orbis.h` (novo),
  `WindowBackend::FAST3D_ORBIS_VULKAN` no `Fast3dWindow`, ganchos ORBIS no
  `Fast3dGui` (janela F1 "OOT3D Graphics", L3+R3, ImGui via `ImGuiRenderHooks`).
- Port: `ps4_display.*`, `gfx_orbis_window.cpp`, `orbis_imgui_platform.cpp`,
  `gfx_orbis_rendering.cpp` (Fast3D + ImGui + PICA). Backend PICA refatorado
  (`ps4_vk_pica_backend_impl.h`, apresentação imediata, `OpenScanoutPass`);
  harness do PC continua PASS.

Falta (próxima sessão):
1. Adicionar ao CMake do produto: fontes do port, `ps4_vk_pica_backend.cpp`,
   `imgui_impl_vulkan.cpp` com `IMGUI_IMPL_VULKAN_NO_PROTOTYPES`, RADV
   (`ps4_link_mesa`), glslang, SDL2; regerar `product_sources.cmake`; compilar
   e corrigir erros dos arquivos novos; linkar o eboot.
2. `main` do PS4 em `oot3d_native_game.cpp` sob `__ORBIS__`: `chdir("/data/triaevum")`,
   argumentos fixos (manifesto `/data/triaevum/game/process-manifest.json`,
   `--config /data/triaevum/config/TriAevum.json`, `--ui-profile topscreen`,
   `--topscreen-config`, `--topscreen-texture-overrides`,
   `--topscreen-link-animations`, `--controls-config`, `--save-data`,
   `native30_no_interpolation`, `--presentation-rate 30`), sysmodules,
   mapeamento SDL do DualShock com Options=start (o driver PacBrew mapeia
   Options→back, touchpad→start; nome "Sony DualShock 4 V2", GUID = 16
   primeiros bytes do nome). `Context::GetAppDirectoryPath`→`/data/triaevum`,
   `GetAppBundlePath`→`/app0`. `kMaximumGuestRefreshesPerPresentation=1` e
   boot-status também para `__ORBIS__`.
3. Staging FTP: `refs/data/mods/topscreen/<hash>/{atlas_overrides.o3tu,zelda_link_ext.zar}`
   (os dois diretórios são idênticos), `refs/data/config/*.json` →
   `/data/triaevum/{mods/topscreen,config}/`; `gamecontrollerdb.txt` em `/app0/resources`.
4. Validar no harness do PC o caminho Fast3D/ImGui antes do PKG.

## Produto 1.00 (TRIA00005) — build pronto, pendente console (2026-10-22)

- `tools/build-product.sh` gera `out/IV0000-TRIA00005_00-TRIAEVUMPRODUCT0.pkg`
  (eboot 133 MB, check-oelf OK). SHA-256:
  `c7b46e40349e7942a8aa406d8312431cba1149cc64abddb9e8831de6c059cdfc`.
- Deps novas compiladas para o PS4: SDL2 do fork PacBrew **sem vídeo/render**
  (`tools/build-sdl2.sh`; o pacote pronto importa EGL/GLES do Piglet), libzip
  1.11.4 (`tools/build-libzip.sh`), tinyxml2 10.0.0. zstd com `ZSTD_TRACE=0`
  (símbolo fraco que o create-fself recusa).
- Link estático do título: `Oot3dWholeAotEntryPoints` /
  `ExecuteOot3dWholeAotFunction` do lado do jogo renomeados no alvo `ps4_title`
  (no desktop eles vivem na DLL do plugin).
- Fontes fora do fecho "Vulkan off" mas referenciados: `diffuse_gi_*.cpp`
  portáteis (menos o executor Vulkan) e `pica_nri_shader_contract.cpp`.
- Patch do TriAevum exportado em `patches/triaevum-ps4-product.patch`.
- Dados do usuário: `tools/stage-product-data.sh ../refs/data [--with-save]`
  envia configs e o pacote TopScreen para `/data/triaevum/{config,mods/topscreen}`.

Não validado no PC: o caminho Fast3D (HUD) e o ImGui do renderer PS4 (o
harness cobre só a parte PICA). Primeiro boot no console é o teste.

### 1.01 — getcwd
- 1.00 no console: `filesystem error: in create_directories: Invalid argument ["logs"]`
  logo após o banner. O `getcwd` da libc do SDK (musl/OpenOrbis) é um stub
  que sempre retorna NULL, então todo caminho relativo do std::filesystem
  falha. `ps4_process_init.cpp` agora define `getcwd` (devolve
  `/data/triaevum`, o diretório do chdir; o processo nunca muda de diretório).

### 1.02 — caminhos relativos
- 1.01 deu o mesmo erro: o problema real é o kernel do PS4, cujas chamadas
  de caminho (open/stat/mkdir/...) respondem EINVAL para caminho relativo
  mesmo com chdir OK. `src/ps4_paths.cpp` + `-Wl,--wrap=` (open, stat, lstat,
  mkdir, rmdir, unlink, rename, access, chmod, truncate, readlink, link,
  statvfs, pathconf, utimensat, fchmodat) prefixam `/data/triaevum/` em todo
  caminho relativo; o getcwd próprio mudou para lá. Cobre fopen/opendir/
  realpath/std::filesystem (todos chegam ao kernel por essas funções).

### 1.03 — ImGui sem WSI
- 1.02 passou do filesystem: RADV pronto, VideoOut registrado, configs e
  TopScreen layout carregados (`contracts=102`). Parou em
  `ImGui Vulkan entry points unavailable`: `ImGui_ImplVulkan_LoadFunctions`
  exige também as funções de surface/swapchain, que não existem sem WSI.
  O loader agora devolve um stub (VK_ERROR_EXTENSION_NOT_PRESENT) para elas
  (só os helpers ImGui_ImplVulkanH_* usam) e registra qualquer outra ausente.

### 1.04 — L3+R3 (menu F1)
- 1.03 no console: jogo roda (TopScreen HUD visível, áudio com cortes).
  L3+R3 crasha: SIGSEGV em `ImGui::NewFrame` -> `SetCurrentFont`
  (`font->ContainerAtlas` nulo = atlas de fontes não montado) chamado por
  `Ship::Gui::StartDraw`. O backend Vulkan do ImGui não montou o atlas antes
  do primeiro quadro do menu. 1.04: `NewImGuiFrame` monta o atlas se ninguém
  montou e registra cada passo (`[triaevum] imgui: ...`); Fast3dGui registra
  se os hooks do PS4 não forem encontrados.
- Pipelines não cacheadas levam ~750 ms cada (um VS ubershader de 65 KB
  recompilado por pipeline pelo ACO). O cache salva a cada 600 quadros, então
  as travadas diminuem com o tempo; próximo passo: compilação assíncrona.
- Áudio picotando: o jogo roda abaixo do tempo real (limite de CPU), então o
  áudio fica sem amostras.

### 1.05 — menu na thread de render, profiler, teste de vértices
- 1.04: o menu aparece e o jogo cai em `begin_subpass` (RADV) na thread
  `NativeRenderCommandStream::WorkerLoop`: com parallel presentation, o
  ImGui gravava no command buffer da thread principal enquanto a thread de
  render gravava o PICA. 1.05 roteia como o Vulkan desktop: NewFrame via
  `RendererRouting.Synchronous`, RenderDrawData via `Deferred` com cópia.
- Profiler: com `/data/triaevum/profile`, o sampler do DolphinPS4 amostra as
  threads `main` (jogo/AOT) e `render` a 1 kHz -> /data/DolphinPS4/samples.log
  (endereços `elf` = offset do .text; `jit` acima de 64 MiB = endereço
  absoluto, subtrair 0x400000).
- Teste: com `/data/triaevum/force_cpu_vertex_conversion`, todo atributo não
  float passa pela conversão na CPU (isola o vertex fetch do RADV/GFX7 nas
  deformações do Link).

### 1.06 — vértices, configurações salvas, medição
- 1.05 confirmou: menu L3+R3 funciona; com a conversão de atributos inteiros
  na CPU o Link fica correto (RADV/GFX7 deforma modelos com os formatos
  R8/R8G8/R8G8B8(A8)_U/SSCALED, R16G16_SSCALED). 1.06 converte por padrão
  (custo ~1% da thread de render); `/data/triaevum/native_vertex_fetch`
  volta ao fetch do driver para teste.
- Configurações não persistiam: o renderer PS4 não reconhecia a transação
  de apresentação (window/resolução/vsync), e o GraphicsSettingsRuntime não
  grava nada com uma aberta. 1.06 faz o que gfx_vulkan_frame.cpp faz
  (TickPresentation, ResolveRendererPresentationRequest,
  AcknowledgePresentationApplied) e confirma o candidato direto (a saída é
  sempre 1080p no VideoOut).
- Profile 1.05 (60–190 s): thread `main` ~50–65% esperando no kernel, ~25–40%
  jogo (ReadFast 6%, memcpy 2%, ~3% libunwind = exceções C++ por quadro);
  thread `render` ~80–90% esperando. O backend faz Flush síncrono (submit +
  vkWaitForFences) várias vezes por quadro. 1.06 registra a cada 5 s
  `[triaevum] perf:` (fps, draws, submits, espera de GPU e de flip).

### 1.07 — resolução, render scale e widescreen do menu
- 1.06 no console: configurações persistem; perf (renderer) 40–60 quadros/s,
  1 submit por quadro, GPU ~4 ms, flip ~8 ms. Resolução/Internal Render
  Scale do F1 não faziam nada (o produto nunca chamava SetPresentOptions,
  escala fixa 4x) e a imagem era 5:3 com barras.
- 1.07: `ApplyRenderResolution` (gfx_orbis_rendering.cpp) por revisão das
  configurações: escala inteira = round(OutputHeight x
  ResolveInternalResolutionScale / 240), 1–6x; `outputAspect` = 16:9 do
  scanout; publica DisplayMetrics.
- Backend: o alvo nativo da tela de cima (480x400, Y = largura apresentada)
  cresce por outputAspect/(5:3) como o alvo do desktop
  (ResolveNativePicaRenderExtent); Target/Display guardam scaleX/scaleY;
  viewport/scissor por `ResolvePicaRasterCanvas` com a evidência de
  `ResolvePicaUiFrameContent` (cena preenche, UI posicionada fica no canvas
  nativo centralizado); PresentDisplay encaixa pela proporção em pixels.
- Harness: fase widescreen (480x400 -> 960x853 em 2x, borda esquerda do
  scanout preenchida), 0 erros de validação. Host de módulo compila.

### 1.08 — triple buffering
- 1.07: resolução/escala funcionam. Usuário: "uncapped" parece melhor que 30
  fps, mas ~15–20 fps a olho. Perf 1.07: 30–45 quadros/s do renderer, flip
  wait 8–10 ms/quadro (double buffering: o render esperava o flip anterior
  todo quadro), GPU 1.5–9 ms conforme a escala. No modo 30 fps o pacer por
  software + vsync de 60 Hz quantiza um quadro de >33 ms para 50 ms (20 fps).
- 1.08: 3 scanouts (`Ps4VkPicaRenderBackend::kScanoutCount`), Ps4Display com
  N buffers; `WaitPendingFlip` espera só até haver ≤ N-2 flips na fila
  (`sceVideoOutGetFlipStatus.numFlipPending`, eventos só acordam). Log de perf
  com flips/s e apresentações do jogo/s. Próximo, se ainda < 30: pacing pelo
  flip rate do VideoOut (sceVideoOutSetFlipRate) no lugar do pacer por software.

### 1.09 — medição da thread do jogo
- 1.08: flips 55–60/s e "game presents" 60/s no uncapped, GPU ~1.5–2 ms,
  mas o usuário ainda vê < 30 estável: as apresentações repetem quadros do
  jogo. 1.09 imprime a cada 5 s (`[triaevum] game:`, oot3d_native_a32_window_frame.cpp
  sob __ORBIS__) guest frames/s, host frames/s e ms por quadro de cada fase
  (guest/AOT, PICA submit/plan/backend, DSP, áudio, input, visual, present,
  pacing). Profiler religado (/data/triaevum/profile).

### 1.10 — thread do jogo sem esperar o render
- 1.09 (vila, 30 fps): 27 quadros do jogo/s; por quadro: guest 17 ms,
  "frame start" 8.6 ms, resto ~1.5 ms. Profile: main 28% esperando no kernel,
  render 85% ocioso. Causa: o roteamento ORBIS do ImGui fazia
  `RendererRouting.Synchronous(NewImGuiFrame)` em todo quadro (Gui::StartDraw),
  o que espera a thread de render terminar o quadro anterior (GPU + flip). 1.10
  segue o desktop: síncrono só até o backend ImGui ficar pronto.
- Exceções C++ por quadro (~3% em libunwind): `--wrap=__cxa_throw`
  (src/ps4_throw_trace.cpp) registra tipo + 7 frames das 20 primeiras e de 1
  a cada 1000 (`[triaevum] throw #N`).

### 1.11 — 60 fps interpolado
- 1.10: "frame start" 8.6 -> 0.2 ms. Casa, modo 30: 30.0 quadros do jogo/s
  com folga (present ~15–20 ms = espera do pacer). Vila, uncapped: simulação
  31–35/s, ~43 apresentações/s; sem interpolação, as repetições irregulares
  parecem ~15–20 fps. Guest ~17.5 ms/quadro. Exceções: todas
  `Oot3dWholeAotObservableExit`, lançadas por
  `Oot3dWholeAotPluginObservableExit` a partir de
  `ApplyNativeWidescreenProjectionPolicy` (saída do AOT por design, ~3%).
- 1.11: removidos `--gameplay-timing native30_no_interpolation` e
  `--presentation-rate 30` do ramo __ORBIS__ (herdados do Switch): vale o
  padrão desktop (simulação 30 Hz interpolada, apresentação 60) e o Frame
  rate do F1 (30 / 60 / 90 interpolados / uncapped) passa a valer. A
  interpolação é do frontend do TriAevum (amostras visuais pelo mesmo
  PicaRenderBackend), sem trabalho no backend PS4.
- 1.11 crashava no boot: `std::array<char*, 27>` dos argumentos ficou com 4
  ponteiros nulos depois da remoção -> `strlen(NULL)` no main. 1.12 usa
  `std::to_array<char*>({...})` (tamanho deduzido).

### 1.14 — acesso à memória do jogo inline
- 1.12 (vila, 60 interpolado): ~29.5 quadros do jogo/s, só ~34
  apresentações/s; guest ~20 ms + present ~5 ms (espera do render nos
  quadros interpolados). Resolução menor baixou a GPU de 9.5 para 2 ms sem
  ganho de fps: limite de CPU da thread do jogo. Profile main: memória do
  guest ~17%, kernel ~17%, libc mem* 6%, frontend PICA 5.5%, exceções 5%,
  cauda longa do AOT ~40%.
- 1.14: `NativeA32Memory::ReadFast/WriteFast` com caminho rápido
  `always_inline` sob __ORBIS__ (`OOT3D_MEMORY_FAST_PATH`), caminhos raros
  `noinline` (`ReadFastChecked`, `WriteFastChecked`, `CheckTracedFastRead`).
  Nenhuma chamada a ReadFast/WriteFast sobra no binário (~500k chamadas só
  para os caminhos lentos). .text 167 MB. Build completo 8 min.
- Log `[triaevum] game:` ganhou áudio: starvations (fila vazia), frames que
  faltaram e amostras na fila.

### 1.15 — reserva de áudio
- 1.14: guest ~18.8 ms (era ~20), ~36 apresentações/s (era ~34). Áudio: fila
  SDL vazia em toda submissão ("0 buffered"; starvations não contavam porque
  a fila nunca atingia DesiredBuffered = 100 ms). O driver PS4 do SDL drena em
  períodos de 5 ms a 48 kHz e o jogo enfileira ~33 ms por quadro.
- 1.15: `SDLAudioPlayer` sob __ORBIS__ pausa a saída quando a fila esvazia e só
  retoma com DesiredBuffered na fila (abre pausado até a primeira reserva).
- Profiler: o .text passou de 64 MiB (janela "eboot" do sampler), endereços
  acima saem como "jit" absolutos; os scripts de análise usam o tamanho do
  eboot da lista de módulos.

### 1.16 — controle dinâmico de taxa do áudio
- 1.15 (escala 5x, GPU ~9.7 ms): ~28 quadros do jogo/s, nenhum interpolado;
  guest ~24.5 ms (era 18.8 ms em 1x: o AOT em si ficou ~35% mais lento com a
  GPU carregada - provável disputa de banda da GDDR5 compartilhada); present
  ~5.6 ms = espera pela thread de render (Flush síncrono com GPU de ~10 ms).
  Áudio: 12–13 esvaziamentos/5 s (simulação a ~93% => falta ~7% de amostras).
- 1.16: `SDLAudioPlayer::DoPlay` (__ORBIS__) reamostra cada envio por
  1 + 0.06·clamp((desejado − na fila)/desejado, −0.5, 1) (0.97–1.06), linear;
  a pausa de reserva continua como último recurso.
- Próximo: envio à GPU assíncrono (2 slots de cb/fence/anel/descritores) com
  flip só depois da fence do quadro (fila de flip atrasada ou thread de flip).

### 1.17 — velocidade real do jogo
- 1.16 (escala 1x, uncapped): ~29 quadros do jogo/s, mas o usuário sente
  câmera lenta forte; áudio "distorcido", 14–16 esvaziamentos/5 s.
- 1.17: o log `game:` mede `dsp frames/s` (tempo real = 32728/160 = 204.5:
  o relógio do DSP anda com o relógio do guest, então é a velocidade real do
  jogo) e `vblanks/s`. Taxa do reamostrador suavizada (5% por envio).

### 1.18 — velocidade certa: 2 refreshes por apresentação
- 1.17 mediu o jogo a METADE da velocidade: ~95–102 quadros de DSP/s contra
  204.5 em tempo real, com 1 vblank por refresh e ~30 refreshes/s. Um refresh
  é um vblank CTR de 60 Hz (1/60 s); a 30 Hz de apresentação são 2 por quadro.
  O port tinha posto __ORBIS__ junto do __SWITCH__ em
  `kMaximumGuestRefreshesPerPresentation = 1` (oot3d_native_frame_rate.h):
  essa foi a câmera lenta desde o produto 1.00 (e o áudio "sem amostras").
- 1.18: PS4 volta ao valor desktop (2). O custo de CPU por apresentação sobe
  (dois refreshes; o 3DS faz a lógica a cada 2 vblanks).

### 1.19 — PGO (coleta)
- 1.18 com velocidade certa: casa ~90% do tempo real, vila ~53% (thread do
  jogo 100% ocupada, ~51 ms de CPU por 1/30 s de jogo). PS4 Slim (sem modo
  Pro). Profile: cauda longa do AOT ~2/3, exceções 7% (~690/s), mem* 7%,
  frontend PICA 5%.
- `PGO=generate tools/build-product.sh` (build/product-pgo-gen,
  `-fprofile-generate` em todo TU, runtime de perfil do clang do host +
  src/ps4_pgo_runtime.c com os pontos de entrada só-glibc) grava
  /data/triaevum/pgo/product.profraw a cada 20 s. Pacote
  out/...-PGO-COLETA.pkg. Depois: `llvm-profdata merge` e
  `PGO=use PGO_PROFILE=... tools/build-product.sh` (build/product-pgo-use).
- Coleta 1: SIGBUS em `__llvm_write_binary_ids` (o fself não mapeia o
  cabeçalho ELF em __ehdr_start): ps4_pgo_runtime.c define um __ehdr_start
  sem program headers.
- Coleta 2: perfil "malformed" - os 29519 registros sem nome/hash/contadores.
  O script de link do OpenOrbis deixa __llvm_prf_* como órfãs depois da
  imagem em arquivo do segmento de dados (o oelf só carregava 0x11b88 bytes,
  o .data): começavam zeradas no console. Para PGO=generate o CMakeLists
  coloca as seções dentro de .data e define os __start_/__stop_.
- Coleta 3 válida: 468 MB de .profraw, 265671 funções, 1.22e11 contagens ->
  pgo/product-vila.profdata (55 MB, `llvm-profdata merge`).

### 1.20 — PGO + buffer de DMA
- `PGO=use PGO_PROFILE=pgo/product-vila.profdata tools/build-product.sh`
  (build/product-pgo-use, 15 min; seções .text.hot/.text.unlikely nos shards).
- O perfil pôs memcpy/memset no topo (bilhões de iterações de 16 bytes). O
  GSP RequestDma zerava um vector de 256 KiB a cada DMA, por menor que fosse;
  1.20 reaproveita um buffer thread_local (comportamento idêntico).
- O pacote normal agora é o PGO; o .profraw do console foi apagado (cópia
  local em scratchpad).
- Medido na vila: 1.18 ~53% do tempo real -> 1.20 ~72% (43 quadros de jogo/s
  de 60; ~37.6 ms de jogo por quadro do host, dos quais pica submit 6.6 e
  planos 4.4). Casa: 60/60, perfeita segundo o usuário.

### 1.21 — afinidade de threads + planos em paralelo (ideias do Dusklight/Dolphin)
- O Jaguar tem dois clusters de 4 núcleos com L2 próprio. Dusklight prende
  main/FIFO/render no primeiro cluster; DolphinPS4 põe CPU e vídeo nos
  núcleos 2 e 3. Agora `Fast::OrbisRegisterThread` (antes
  OrbisProfileRegisterThread) prende "main" no núcleo 2, "render" no 3 e os
  ajudantes "pica plan" em 0-1 (scePthreadSetaffinity).
  /data/triaevum/no_affinity desliga.
- Os planos Vulkan de uma passada de drenagem são independentes: com >= 16
  draws, dois ajudantes (cada um com seu cache de shader) montam junto com a
  thread do jogo; o laço consome em ordem como antes (PicaPlanWorkers em
  oot3d_native_a32_window_frame.cpp, só __ORBIS__).
- Console: com afinidade um núcleo fica 100% e o jogo roda pior; com
  no_affinity melhor. Vila na 1.21 (no_affinity): ~42.5 quadros de jogo/s,
  planos 4.4 -> 3.2 ms, total igual à 1.20 (a thread do jogo é o limite).

### 1.22 — afinidade desligada por padrão
- A fixação de núcleos só liga com /data/triaevum/affinity.
- Perfil 1.22 (vila -> Árvore Deku), thread do jogo: mem* 7.5%, kernel 7.3%,
  exceções 7.2%, acesso à memória do guest 4.4%, frontend PICA 4.1%; o resto
  é a cauda longa do AOT. Todas as ~740 exceções/s vêm do mesmo lugar:
  ApplyNativeWidescreenProjectionPolicy pedindo saída observável
  (Oot3dWholeAotPluginObservableExit -> throw).

### 1.23 — saídas observáveis sem exceção
- O código gerado já devolve sem exceção qualquer fluxo que não seja o seu
  retorno (é assim que o orçamento de blocos, BlockLimit, sai). No PS4 (uma
  só imagem, thread_locals compartilhados) o hook pede a saída
  (Oot3dWholeAotDeferObservableExit), Oot3dAotEnterBlock devolve false como
  no fim do orçamento, e ExecuteOot3dWholeAotFunction converte de volta na
  saída Branch com o snapshot do estado, igual ao catch do throw. Fora de
  um hook de bloco (interpretador etc.) continua o throw.
- Afinidade de núcleos (1.22): só com /data/triaevum/affinity.
- A mudança em Oot3dAotEnterBlock (inline em toda função gerada) muda o
  CFG de todas: "function control flow change detected (hash mismatch)"
  em todos os shards, ou seja, product-vila.profdata não vale mais para o
  código gerado. Qualquer mudança no runtime header/gerador pede nova
  coleta PGO. Pacote da 1.23 com perfil velho guardado como
  out/...-1.23-perfil-velho.pkg (não usar); nova coleta com PGO=generate.
- Coleta PGO da 1.23 (pgo/product-123.profdata, 1.0e11 contagens). Zero
  throws no log: as saídas observáveis já não usam exceção.
- Na coleta o jogo travou num pulo do Link: sem crash; o gravador de perfil
  seguiu vivo e as quatro threads (main, render, 2x pica plan) ficaram 100%
  no mesmo ponto de espera do libkernel por >1 min - um impasse.
  Suspeito: um ajudante de planos que morre numa exceção deixa a thread do
  jogo esperando para sempre. Agora: exceções nos ajudantes são capturadas
  e relançadas na thread do jogo; e um vigia (ps4_process_init.cpp) escreve
  "[triaevum] stall: ..." com a espera nomeada (OrbisWaitScope: ajudantes
  de plano, render worker, flip) quando 10 s passam sem quadro.

## 1.25 — sync com o upstream beta.1.2 (2026-10-23)

- O fork no GitHub estava 73 commits atrás do coccofresco/TriAevum. Merge
  feito (um conflito só, em `oot3d_native_game.cpp`: o init ORBIS e o novo
  diretório de DLLs do Windows ficam lado a lado) e a 1.24 (vigia de
  travamento), que só existia local, entrou como commit em cima.
- A árvore local `../TriAevum` agora é cópia exata do fork (LF; backup da
  anterior em `../TriAevum-antes-do-sync-2026-10-23.tar.zst`). O
  `triaevum-src.git` (baseline dd2de71) ficou obsoleto: o histórico das
  mudanças é o próprio fork, e `patches/triaevum-ps4-product.patch` agora é o
  diff contra o `main` do upstream.
- Closure regenerada (`tools/product/regen-closure.sh`, que antes era passo
  manual num scratchpad perdido): 43 alvos, 602 fontes (+17: menu in-game,
  photo mode, LOD/residência da grama, overlay de RomFS, Visions e seleção de
  arquivo Reframed do TopScreen, fonte do menu). Compilou sem nenhum erro.
- PKG `out/...-1.25-upstream-beta12.pkg` **sem PGO** (o perfil da 1.23 não
  vale mais para o runtime mudado): esperar menos velocidade que a 1.24 até a
  nova coleta. Testar no console: boot, título, vila; o upstream agora abre um
  menu in-game com R3 — conferir se não briga com o nosso L3+R3.
- Limpeza do fork: saem os ports/scripts de Android, Switch, AppImage/Flatpak/
  Windows, `.github` e as imagens/notas de release do PC
  (`sync/removed-paths.txt`; `tools/sync-upstream.sh` os remove de novo a cada
  merge). README do repositório reescrito só para o PS4.
- Próxima fase: preparação no console (`docs/FASE4-PREPARACAO-NO-CONSOLE.md`).

## 1.26 — R3 x L3+R3 (2026-10-23)

- Console na 1.25: o menu in-game novo do upstream abre com R3
  (`menu.Toggle = Esc || R3`), então L3+R3 abria os dois menus juntos.
  Agora no PS4: R3 sozinho abre o menu in-game novo ao **soltar** o R3, só
  se o L3 não foi apertado durante o toque; L3+R3 abre só a janela de
  configurações completa (o F1). `oot3d_native_a32_window_input.cpp`.
- Log da 1.25 (sem PGO): 23–45 quadros do jogo/s (35–70% do tempo real),
  CPU do jogo 22–50 ms por quadro. A 1.24 com PGO fazia ~72% na vila. Nova
  coleta PGO fica para depois que o runtime estabilizar.
