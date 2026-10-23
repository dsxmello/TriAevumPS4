# TriAevum PS4 (Vulkan) — build root

Port do TriAevum (recomp AOT de *Ocarina of Time 3D*) para PS4 usando a stack
**Mesa RADV-on-GNM** herdada de [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)
e [dusklight-ps4](https://github.com/) (patches + convenções). Vulkan no PS4:
RADV estático linkado no eboot → winsys GNM (`ac_ps4_drm.c`) → `libSceGnmDriver` →
VideoOut. **Sem Piglet, sem Shacc, sem libs em `/data/self/`.**

## Este repositório

Só o código do port: scripts, patches, o runtime PS4 (`ports/`) e a
documentação de cada versão (`docs/FASE3-PRODUTO.md`). **Não contém e nunca
vai conter** a ROM, o `code.bin`, o C++ gerado do jogo (`title-content/`), o
SPIR-V do título, perfis PGO nem pacotes `.pkg` — tudo isso vem da cópia do
próprio usuário. Também ficam fora o que é baixado ou compilado (`sdk/`,
`src/`, `sysroot/`, `build/`, `out/`) e a árvore do DolphinPS4 (`refs/`,
reconstruída a partir do upstream + `patches/dolphinps4-ps4-runtime.patch`).

No GitHub este diretório é a pasta `ps4/` do repositório, e as mudanças no
TriAevum já estão aplicadas na árvore dele (commit "PS4: runtime changes").
Os scripts esperam o TriAevum ao lado, em `../TriAevum`, e o C++ gerado do
usuário em `../title-content/aot-cpp`:

```sh
git clone https://github.com/dsxmello/TriAevumPS4 TriAevum
ln -s TriAevum/ps4 triaevum-ps4-vk     # build root do port
# title-content/aot-cpp: gerado pelo TriAevum a partir da cópia do usuário
```

O patch equivalente também fica em `patches/triaevum-ps4-product.patch`
(contra o commit-base dd2de71 da árvore do TriAevum), aplicadas em
`../TriAevum`. O produto (TRIA00005) sai de `tools/build-product.sh`
(`PGO=generate|use` para builds com PGO).

Licença: GPLv3 (`LICENSE`), a mesma do TriAevum. Os patches de terceiros
(Mesa, DolphinPS4, dusklight-ps4) seguem as licenças dos projetos de origem.

O plano completo está em `~/.opencode/plan/triaevum-ps4-vulkan.md`; o histórico do
port GLES2 (fases 1–3.2) está em `triaevum-ps4-port/docs/TRIAEVUM_PS4_PORT.md`.

## Layout

| Caminho | O que é |
|---|---|
| `tools/` | scripts de fetch/build/package (adaptados do dusklight/DolphinPS4) |
| `patches/` | herdados: `mesa-26.0.8-ps4.patch` (WSI videoout!), `dolphinps4-ps4-runtime.patch`; nosso: `mesa-26.0.8-triaevum.patch` (dep_elf stub PS4) |
| `ports/ps4-vk-probe/` | Fase 0: probe Vulkan (TRIA00003, identidade plain `gd`) — **validado em console** |
| `ports/ps4-vk-game/` | jogo (TRIA00004): closure AOT + **renderer Vulkan** (`src/ps4_vk_pica_backend.*`, Fase 2) — libc++ 21, VideoOut/scePad |
| `ports/ps4-vk-game/hosttest/` | harness de PC: o mesmo backend no Vulkan do Linux + validation layers, com os shaders reais do corpus |
| `src/glslang-15.4.0` | glslang (GLSL→SPIR-V no console, modelo DolphinPS4); instalado em `sysroot/glslang` |
| `refs/DolphinPS4` | runtime de console (patch de runtime aplicado + `ac_ps4_drm.c` as-built em `port/mesa/`) |
| `src/mesa` | Mesa 26.0.8 (tag `mesa-26.0.8`) com o patch aplicado |
| `src/libdrm` | headers 2.4.125 (o PS4 não tem DRM; só satisfaz o meson) |
| `src/llvm-project-21.1.6.src/` | runtimes p/ libc++ 21 (SHA-256 verificado contra a API do GitHub) |
| `sdk/` | PacBrew OpenOrbis SDK (baixado, SHA-256 conferido) |
| `sysroot/` | libc++ 21 estática + libc-overlay FreeBSD |
| `build/`, `out/`, `downloads/`, `mesa-venv/` | produtos |

## Passos (host Linux x86_64, uma vez)

```sh
tools/fetch-openorbis.sh      # SDK PacBrew → sdk/  (SHA-256 contra o db)
# src/mesa (tag mesa-26.0.8) + patches/mesa-26.0.8-ps4.patch já aplicado
# src/libdrm (libdrm-2.4.125), LLVM 21.1.6 tarball verificado
tools/build-libcxx.sh         # libc++ 21 → sysroot/
tools/build-mesa.sh           # RADV/ACO estático → build/mesa
tools/build-vk-probe.sh       # probe Vulkan → out/…TRIA00003….pkg
tools/build-glslang.sh        # glslang 15.4.0 estático → sysroot/glslang (SHA-256 conferido)
tools/build-game.sh           # jogo + renderer Vulkan → out/…TRIA00004….pkg
VVL_ROOT=… ports/ps4-vk-game/hosttest/run.sh   # opcional: backend no PC com validation layers
# (title-content/aot-cpp — os shards gerados — ficam fora do repo, como sempre)
```

Requisitos do host: clang (o reference build usava LLVM 21; este host usa 23 via
`tools/host-bin/`), ld.lld, cmake, ninja, meson/mako/pyyaml/packaging (`mesa-venv/`),
glslangValidator, bison, flex, git, curl.

### Peculiaridades deste host (Arch) vs WSL Ubuntu das referências

- `host-compat/libxml2.so.2` → link para a `libxml2.so.16` do sistema: o `ld.lld`
  do SDK linka contra libxml2.so.2, e o `ps4-env.sh` procurava o substituto no
  caminho Debian (`/usr/lib/x86_64-linux-gnu`), inexistente aqui — com
  `set -euo pipefail` o script morria **silenciosamente**. O `env.sh` exporta o
  `LD_LIBRARY_PATH` com o link privado (receita love-ps4).
- Python 3.14 não tem `distutils`: o venv do Mesa precisa de `packaging`.
- O Arch instala `libelf.so` no host (WSL Ubuntu das referências não tinha
  libelf-dev): o meson achava libelf e compilava `ac_rtld.c`, cujos símbolos
  quebravam o link do eboot. `tools/build-mesa.sh` injeta
  `-Wl,--sysroot,$OPENORBIS` (via `MESA_EXTRA_LINK`) para o linker só enxergar
  as libs do SDK — Mesa então constrói o RADV no caminho upstream
  `#if !defined(USE_LIBELF)`, a configuração que roda nos consoles de referência.

## Testar o probe (Daniel)

1. Instale o `IV0000-TRIA00003_00-TRIAEVUMVKPROBE0.pkg` (GoldHEN Package Installer).
2. Abra o app. Esperado: **triângulo girando** por ~5 s na TV (Vulkan → RADV → GNM).
3. Logs via FTP: `/data/triaevum/vk-probe.log`, `vk-probe-stderr.log`, `mesa.log`.
   Reporte o conteúdo dos três + o que apareceu na tela.
