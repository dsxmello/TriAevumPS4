# Fase 0 — Probe Vulkan (TRIA00003): RESULTADO EM CONSOLE

Data: 2026-10-22. Console: PS4 do Daniel (desbloqueado, GoldHEN, firmware 13.52).
Build testado: `IV0000-TRIA00003_00-TRIAEVUMVKPROBE0.pkg`
(sha256 `350bdc53cf3874d3bf1d0b9f5dc65780831f84cd6dfd15e8d5afee0f25bcba1c`,
25.165.824 bytes — **conferido idêntico ao entregável registrado no plano**).

## Veredito

**MARCO DA FASE 0 ATINGIDO.** A stack Vulkan inteira funciona no console, na
ordem projetada: instância (RADV estático no eboot) → device via winsys GNM →
**GPU executando de verdade** (fill + readback) → pipeline ACO (SPIR-V→GCN) →
framebuffers lineares registrados no VideoOut → flip a cada vblank por 5 s
(300 frames), encerramento limpo (`Vulkan probe done`), sem crash.

Log arquivado: `docs/logs/2026-10-22-FASE0-vk-probe.log`
(md5 `870349dec1d9465aaf52eb36285b11c7`, 68 linhas; original restaurado em
`/home/mello/vk-probe.log` depois de um apagamento acidental pós-desligamento
do PC — foi recuperado da lixeira, íntegro).
`vk-probe-stderr.log` e `mesa.log` ainda não foram coletados (ver "Pendências").

## Checklist do FASE0-TESTE.md — todas as linhas-chave presentes

| Linha esperada | Valor no log | Interpretação |
|---|---|---|
| `instance API 1.x` | `instance API 1.4.335` | RADV estático anuncia Vulkan 1.4 no nível de instância |
| `GPU: AMD Liverpool (PS4)` | `GPU: AMD Liverpool (PS4) (RADV BONAIRE), API 1.3.335, driver 0x6800008, device id 0x9920` | winsys GNM respondeu; device anuncia 1.3 |
| `memory type N: flags …` | tipos 0–6, heaps 0 e 1 | onion (heap 0) e garlic (heap 1) mapeados |
| `step 2: first 0x12345678, last 0x12345678` | idem | **GPU executou o `vkCmdFillBuffer`** (PM4 via libSceGnmDriver), readback confirmado |
| `pipeline compiled in N us` | `pipeline compiled in 2304 us` | ACO compilou SPIR-V no console (sem Shacc, sem Piglet) |
| `frame 0: flip 0, …` | frames 0–299, `flip 0x0` em todas as amostras | `sceVideoOutSubmitFlip` = SCE_OK; evento de flip entregue no equeue |

## Evidência matemática da rasterização (leitura de pixel do framebuffer)

O probe lê de volta a memória do framebuffer **registrado no VideoOut**:

- **corner (pixel 0,0)** = clear `{0.05, 0.05, 0.1+0.2·pulse, 1.0}` → R8G8B8A8
  esperado `0xff{B}0d0d` com B ∈ 0x19–0x4c. Amostras: 0x33, 0x38, 0x2b, 0x3f,
  0x24, 0x44 — todas dentro do pulso; frame 0 (pulse=0.5 → B=0x33) deu
  `0xff330d0d` **exato**.
- **centre (pixel 960,540)** = `0xff5f6f73/74`. Interpolação baricêntrica das
  cores dos vértices (vermelho (1,0.1,0.1), verde (0.1,1,0.1), azul (0.1,0.3,1))
  no centro da tela: pesos (0.3913, 0.3043, 0.3043) → (0.452, 0.435, 0.374) →
  bytes (0x73, 0x6f, 0x5f) — **exato** (±1 LSB nas amostras com o triângulo
  rodado).

Ou seja: rasterização, interpolação de varyings e saída pro scanout estão
corretos pixel a pixel. O triângulo girou na TV por ~5 s conforme projetado.

## Números úteis para as próximas fases

| Medida | Valor | Nota |
|---|---|---|
| Queue | family 0, flags 0xf (GFX+COMPUTE+TRANSFER+SPARSE), 1 queue | confirma o risco vigiado "única queue GFX" — planejar sync na family 0 |
| API | instância 1.4.335, device 1.3.335 | Vulkan 1.3 no device: features 1.2 (timeline semaphores etc.) tendem a existir; verificar por-feature no caminho NRI |
| Memória | 7 tipos / 2 heaps; staging em tipo 2 (HOST_VISIBLE\|HOST_COHERENT, heap 0 = onion); framebuffers em tipo 3 (DEVICE_LOCAL\|HOST_VISIBLE\|HOST_COHERENT, heap 1 = garlic) | garlic é CPU-mapeável: o probe leu pixels direto. Endereços 0x43ff400000/0x43fea00000, align 256 |
| Framebuffer | 1920×1080×4 = 8.294.400 B, pitch 7680 (linear, sem padding) | como projetado p/ scanout VideoOut |
| `sceVideoOutOpen` | `0x4e110100` (handle válido) | **não matou o processo em fw 13.52** com identidade plain (risco do FASE0-TESTE.md descartado) |
| `sceVideoOutRegisterBuffers` | 0 | 2 buffers lineares registrados |
| Pipeline ACO | 2304 us no console | prewarm/cache da Fase 2 vale o custo (~2 ms) |
| render+wait | média cumulativa 1970 us → 8760 (frame 59, warmup) → 2586 us (frame 299); **instande no último minuto: ~1.1 ms/frame** | orçamento vblank 16,6 ms → GPU custa ~7% |
| Relocations | self-test do probe: 61/72 slots de trampoline setados, todos os lookups `unchecked == trampolines` | `R_X86_64_RELATIVE` aplicados pelo loader; RADV sem loader externo OK |

## Pendências (não bloqueiam a Fase 1)

1. ~~`vk-probe-stderr.log` e `mesa.log`~~ **Fechadas (2026-10-22, FTP):**
   `vk-probe-stderr.log` coletado e arquivado (`docs/logs/…-vk-probe-stderr.log`)
   — **0 bytes**: o RADV inteiro subiu sem um único diagnóstico no stderr.
   `mesa.log` nunca foi criado (`MESA_LOG_FILE` não produziu arquivo — sem
   mensagens do Mesa). Nada a reportar além do vk-probe.log, que já era
   conclusivo por si.
2. Confirmar verbalmente o visual: triângulo colorido girando ~5 s (a leitura
   de pixel já prova, mas o protocolo pede o relato).

## Conclusão

A direção RADV-on-GNM está validada de ponta a ponta no console. Seguir para a
**Fase 1**: host novo sobre `port/` do DolphinPS4 (`ps4_crt1.S` malloc-replace +
dlmalloc, `ps4_crashlog.cpp`, sysmodules no boot) + recompilação do closure AOT
da fase 2 (256 shards + módulo + serviços + ctr host + bridges) na libc++ 21,
input `scePad`, tela de status via VideoOut — marco: título headless ~30 fps
com heap estável e crash.log funcionando.
