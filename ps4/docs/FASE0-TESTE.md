# Fase 0 — Probe Vulkan (TRIA00003): instruções de teste

> **RESULTADO (2026-10-22): MARCO ATINGIDO** — ver `FASE0-RESULTADO.md`.
> Este doc fica como registro do protocolo de teste.

Data: 2026-10-22. Build: Mesa 26.0.8 + patches dusklight (WSI videoout) sobre
RADV estático, winsys GNM as-built, toolchain PacBrew + clang 23 (host) + libc++ 21.
Identidade **plain** (CATEGORY `gd`, APP_TYPE 1, PAID `0x3800000000000035`):
sem Piglet, sem Shacc, **não usa nada de `/data/self/`**.

## Resultado (2026-10-22, console do Daniel — fw 13.52): **MARCO ATINGIDO**

Triângulo girando ~5 s, saída limpa. Análise completa, veredito e log arquivado
em **`FASE0-RESULTADO.md`** (+ cópia em `logs/2026-10-22-FASE0-vk-probe.log`):
GPU executou o fill (PM4 via GNM), ACO compilou o pipeline no console,
300 flips sem erro, readback de pixel bate com a matemática (centre =
baricêntrica exata; corner = clear pulsante), ~1,1 ms/frame de render+wait.
`sceVideoOutOpen` não matou o processo na identidade plain (risco descartado).
Pendências de log fechadas (2026-10-22, FTP): `vk-probe-stderr.log` coletado
— **0 bytes** (RADV subiu sem nenhum diagnóstico no stderr; cópia em
`logs/`); `mesa.log` nunca foi criado. Falta só o relato verbal do visual.

## Entregável (verificado no build)

| Item | Valor |
|---|---|
| PKG | `triaevum-ps4-vk/out/IV0000-TRIA00003_00-TRIAEVUMVKPROBE0.pkg` |
| Tamanho | 25.165.824 bytes |
| SHA-256 | `350bdc53cf3874d3bf1d0b9f5dc65780831f84cd6dfd15e8d5afee0f25bcba1c` |
| eboot.bin | 23 MB (RADV inteiro linkado estático, check-oelf OK: 1 PT_LOAD RW) |
| SFO | APP_TYPE=1, ATTRIBUTE=0, CATEGORY=gd, SYSTEM_VER=0, APP_VER=01.00 |
| sce_module | **vazio** (sem Piglet); `sce_sys/about/right.sprx` presente |
| Composição | `libvulkan_radeon.a` 116 MB/183 objetos, contém `wsi_common_videoout.c.o` + `vk_instance.c.o`; `ac_rtld`/`USE_LIBELF` fora (config de console das referências) |

## O que este PKG é

O "hello triangle" da stack nova (Fase 0 do plano Vulkan). Ele valida, em ordem:

1. `vkCreateInstance` sem extensões (RADV estático linkado no eboot);
2. enumerate device → GPU "AMD Liverpool (PS4)" via winsys GNM;
3. `vkCreateDevice` + `vkCmdFillBuffer` (o GPU de verdade executa PM4 via GNM);
4. pipeline com SPIR-V (compilado no console pelo ACO) → triângulo girando
   em 2 imagens lineares registradas no VideoOut, flip a cada vblank por ~5 s.

## Como testar (Daniel)

1. Instale `IV0000-TRIA00003_00-TRIAEVUMVKPROBE0.pkg` com o Package Installer do GoldHEN.
2. Abra o app pela XMB. Ele fecha sozinho após ~5 s de triângulo.
3. Resultado esperado: **triângulo colorido girando** em fundo quase-preto.
4. Pegue via FTP e mande os três logs:
   - `/data/triaevum/vk-probe.log` (passo a passo com resultados de cada chamada)
   - `/data/triaevum/vk-probe-stderr.log` (diagnósticos do RADV)
   - `/data/triaevum/mesa.log` (log do Mesa, `MESA_DEBUG=1`)

## Cenários e o que reportar

| Tela | Significado | O que mandar |
|---|---|---|
| Triângulo girando ~5 s | **MARCO ATINGIDO**: stack Vulkan inteira OK | os 3 logs (para registrar a evidência) |
| Tela preta, app fecha | algum passo falhou antes do render | os 3 logs — a última linha de `vk-probe.log` antes do fim nomeia a chamada |
| Crash imediato (CE-xxxxx) | morte no boot do RADV/winsys | os 3 logs (o que existir) + tela de erro |
| Nada acontece / app abre e fecha | identidade PKG? | relatar comportamento exato |

## Linhas-chave a procurar no vk-probe.log

- `instance API 1.x` — versão Vulkan anunciada pelo RADV
- `GPU: AMD Liverpool (PS4)` — winsys GNM respondeu
- `memory type N: flags ...` — heaps onion/garlic mapeados
- `step 2: first 0x12345678, last 0x12345678` — **GPU executou de verdade**
- `pipeline compiled in N us` — ACO compilou o SPIR-V no console
- `frame 0: flip 0, centre 0x..., ...` — framebuffers no VideoOut

## Notas técnicas (para quem continuar)

- O eboot linka o RADV **inteiro** (estático, `--whole-archive`): ~60-80 MB esperado.
- O probe usa a identidade plain porque ela dá 4,5 GiB de direct memory e o
  VideoOut próprio; a identidade gde (Piglet/retroarch) era do app GLES antigo
  (TRIA00001/2 continuam instaláveis para A/B).
- Se `sceVideoOutOpen` matar o processo em firmware 13.52, reportar — o doc do
  DolphinPS4 registrou esse sintoma apenas sob identidade retroarch/composição
  do shell, nunca na plain.
