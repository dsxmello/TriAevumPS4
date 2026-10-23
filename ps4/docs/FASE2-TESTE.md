# Fase 2 — Renderer Vulkan (TRIA00004 0.92): instruções de teste

Data: 2026-10-22. Primeiro build com o **renderer de verdade**: os draws do
PICA deixam de ser só contados e passam a ser rasterizados pela GPU do PS4
via RADV-on-GNM, e a imagem do jogo é apresentada nos framebuffers do
VideoOut.

## Resultado do 0.91 em console (fecha a Fase 1)

Relato do Daniel: **status estável** na TV. Log (`/home/mello/triaevum_boot.log`):
`fb0=/fb1=` alternando `0xff181010` (fundo) e `0xff60ff60` (barra) nos dois
buffers — escrita de CPU funciona em garlic **e** em onion. A hipótese
"garlic invisível para CPU" do 0.90 está **descartada**; o preto do 0.90
teve outra causa (não reproduzida). Marco da Fase 1 atingido: título
headless ~23→15 fps, heap plano em 512 MiB, sem crash.

Achado de brinde no mesmo log: `scePadOpen = 0x80920005` =
`ORBIS_PAD_ERROR_NOT_INITIALIZED` — faltava `scePadInit()`. Corrigido no
0.92 (o controle nunca tinha aberto nos 0.90/0.91).

## O que mudou no 0.92

```
guest GSP → PICA host (defer=true, como o desktop)
          → TriAevumOot3dPicaRenderer → plan → bridge   (já existiam)
          → Ps4VkPicaRenderBackend  (ports/ps4-vk-game/src/ps4_vk_pica_backend.*)
              GLSL canônico 4.50 → glslang NO CONSOLE → SPIR-V → ACO → GCN
              RADV estático → winsys GNM → VideoOut (imagens lineares do probe)
```

- **Sem tradução de shader**: o GLSL que o gerador emite (o mesmo do produto
  desktop Vulkan) compila direto. Validado no PC: 40/40 shaders do corpus
  (`oot3d_pica_boot_title_kokiri_effective.json`) compilam com glslang.
  Toda a saga Shacc/GLES (0.16–0.26) não se aplica: ACO compila qualquer
  tamanho de programa.
- **glslang 15.4.0 estático** no eboot (`tools/build-glslang.sh`, tarball com
  SHA-256 conferido) — o mesmo modelo do DolphinPS4.
- **Caches em disco** (primeira sessão paga, as seguintes não):
  `/data/triaevum/spv-cache/*.spv` (SPIR-V por identidade de fonte) e
  `/data/triaevum/vk-pipeline-cache.bin` (salvo a cada 10 s quando há
  pipeline novo).
- **Apresentação**: os dois framebuffers do VideoOut agora são as imagens
  lineares `R8G8B8A8` do backend (receita exata do probe que funcionou). A
  GPU desenha o display transfer com `Present` (transpose retrato→paisagem do
  desktop, escala para caber em 1920×1080, centralizado); a CPU escreve por
  cima só uma **faixa de HUD** de 14 px no topo (fps, draws, shaders,
  pipelines). A página de status completa volta sozinha se o jogo ficar 2 s
  sem apresentar.
- **Sincronização v1**: uma submissão por frame, espera síncrona (simples e
  correto; otimizar depois).

## Validado no PC antes do console

`ports/ps4-vk-game/hosttest/` roda **o mesmo `ps4_vk_pica_backend.cpp`** no
Vulkan do Linux (Intel Xe) com as validation layers da Khronos: 117 draws
com os 39 fragment shaders reais + o vertex real, índices 8/16 bits,
atributo em formato sem suporte (conversão), binding per-instance, fan,
stencil, blend, logic op, scissor, fills de cor/depth, raw copy materializada
e present. **Zero erros de validação, inclusive com validação de
sincronização.** O harness já pegou um bug real (atributos ausentes para as 16
entradas declaradas — Input-07904), corrigido antes do PKG. Um segundo bug
saiu no link: o glslang tinha sido compilado contra a libstdc++ do host
(passar `-DCMAKE_CXX_FLAGS` apaga as flags do toolchain); o
`build-glslang.sh` agora usa `CMAKE_*_FLAGS_RELEASE` e a lib sai 100% libc++.

O que o PC **não** prova: comportamento do RADV/GNM no Liverpool (gfx7),
tempo de compilação no Jaguar, e se a orientação da imagem na TV está certa.

## Entregável

| Item | Valor |
|---|---|
| PKG | `triaevum-ps4-vk/out/IV0000-TRIA00004_00-TRIAEVUMVKGAME00.pkg` |
| Tamanho | 123.404.288 bytes (eboot 121,5 MB: closure AOT + RADV + glslang) |
| SHA-256 | `db3e485e8b268a3f8915673348ea2278359d09e1af56e9e51087659885801a8b` |
| SFO | APP_VER **00.92** (mesmo content id: instala como update do 0.91) |
| Conteúdo | o mesmo `/data/triaevum/game/` — nada a refazer |

## Como testar (Daniel)

1. Instale o PKG 00.92 e abra.
2. **Primeira abertura vai engasgar**: cada shader novo compila no console
   (glslang + ACO). Os primeiros segundos do título podem ficar lentos. Deixe
   rodar ~2 min, feche, **abra de novo** e compare (a segunda sessão usa os
   caches).
3. Teste o controle (agora com `scePadInit`): cruz/círculo/options no título.
4. Mande via FTP:
   - `/data/triaevum_boot.log`
   - `/data/triaevum/shader_fail_*.txt` (se existirem)
   - `/data/DolphinPS4/crash.log` (se morrer)
   - foto da TV, se possível.

## O que olhar no log

- `vk-backend ready: AMD Liverpool ... depth=129` → RADV subiu no processo do
  jogo (129 = `D24_UNORM_S8_UINT`; 130 = `D32_SFLOAT_S8_UINT` também serve).
- `vk-backend: scanout 0/1 at 0x... pitch 1920` + `sceVideoOutRegisterBuffers = 0 (vulkan scanout images ...)`.
- `vk-backend: compiled vertex/fragment ... in N ms` — custo do glslang no
  Jaguar (na 2ª sessão essas linhas somem; aparecem só no contador `disk=`).
- `vk-backend: render target WxH ...` e `display image WxH ...` — o que o
  título renderiza.
- linha `vk draws=... skip=...[...] shaders=... pipes=... pres=... vkerr=... last=...`
  a cada segundo, ao lado do `heartbeat`.

## Cenários

| TV | Leitura | Próximo passo |
|---|---|---|
| **Tela de título do OoT3D** (mesmo torta/espelhada) | **MARCO DA FASE 2** | orientação/cor se preciso; performance |
| Imagem de cabeça para baixo / espelhada / girada 90° | rasterização ok, convenção de present | ajuste de flip/transpose (1 linha) |
| Status page com `VK PHASE 2 - NO VULKAN` | RADV não subiu no jogo | linhas `vk-backend:` do boot log |
| Status page "WAITING FOR THE FIRST FRAME" para sempre | backend ok, nenhum `Present` chegou | linha `vk ... pres=0 xfail=...` + `last=` |
| Fundo quase preto + HUD verde no topo, sem jogo | present acontece mas o conteúdo está vazio | `skip=[...]`, `shader fail`, dumps |
| Crash | — | crash.log + boot log |
| fps muito baixo só no começo | compilação de shader | comparar com a 2ª sessão |

## Resultado do 0.92 em console (2026-10-22): **MARCO DA FASE 2**

Tela de título do OoT3D desenhada pelo RADV na TV (foto do Daniel). Log:
30,0 fps cravados, `skip=0 fail=0 vkerr=0`, 55 pipelines, 83 texturas,
SPIR-V vindo do cache em disco (`disk=26`, `glslang_ms=0`), `gpu_wait` ≈ 1,5
ms/frame. O controle abriu (`scePad opened`). Alvos: topo 480×400 (o AA 2×
horizontal do 3DS, reduzido para 240×400 no transfer), baixo 240×320.

Problemas relatados: (1) resolução baixíssima (renderização 1×, esticada
4,5×); (2) "algumas shaders erradas", com serrilhado e cintilação; causa
provável: só o nível base das texturas era enviado, sem mipmaps; (3) sem o
HUD single screen.

Diagnóstico do (3): o HUD do TopScreen **não é desenhado pelo jogo**. O
módulo só aplica os patches de layout ("TopScreen profile active"); corações,
itens etc. são reconstruídos pelo host desktop
(`Oot3dNativeUiLifecycleBridge` → primitivas de UI → renderer, ~20 mil
linhas em `native_game_runtime/oot3d_top_screen_*`,
`oot3d_native_ui_*`, `ui_contract/`) com as texturas do pacote TopScreen que
o Forge baixa. É um port à parte (próxima etapa).

## 0.93

- **Mipmaps**: cadeia inteira decodificada como no desktop
  (`NativePicaMipLevelCount`/`EncodedMipSize`), sampler com
  `minLod/maxLod` = faixa do guest e `mipmapMode` do PICA.
- **Resolução interna** (render scale do desktop): targets em N×, viewport e
  scissor escalados, transfers em N×, materialização reamostra para o nativo
  (`EncodePicaFramebuffer`). Padrão **4×** (tela de cima 1600×960).
- **Composição de telas**: o backend guarda a última imagem de cada tela e
  compõe no fim do frame. Layouts: só cima / cima + baixo lado a lado / baixo
  no canto. Com isso dá para jogar com o HUD nativo da tela de baixo enquanto
  o single screen não chega.
- **Menu L3+R3** (salvo em `/data/triaevum/ps4_settings.json`): resolução
  1–6×, layout, esticar 16:9, filtro suave/nítido, HUD de desempenho. O jogo
  recebe controle neutro enquanto o menu está aberto.
- Harness de PC: mips + escala 3× + lado a lado, 0 erros de validação.
- PKG 00.93: `a6d3d6b36a522d70d52b1e5326415a972d23af957e566b1289dc8a1204dee429`.
