# Fase 6 — Forge no console (plano e registro, 2026-10-24)

Objetivo (Daniel): mandar só a ROM decriptada para o PS4 por FTP; o console
gera os arquivos do jogo, aplica os mods escolhidos (Vanilla UI/HUD HD,
Vanilla+) e prepara as pipelines de GPU, como o TriAevum Forge do PC.

O AOT do jogo já está no eboot: o console não traduz código. Só são aceitas
as revisões cujo código de execução é o compilado (`code_sha256 16a6b0aa`):
as três receitas do catálogo da release 0.8.0-beta.1.2 (EUR base, EUR
No-Intro/Rev 1 e USA via adaptador para a canônica EUR).

## Fases

1. **ROM -> jogo (1.33).** Extração, receita, adaptação USA, verificação,
   manifesto do processo; pack do TopScreen e config padrão do PKG.
2. **Texturas HD (Vanilla UI/HUD 3.2).** Download no console (HTTPS pelos
   módulos de sistema `libSceHttp`/`libSceSsl`), extração do zip (libzip),
   pack ordenado por idioma, `--ui-texture-pack` e o atlas HD do TopScreen.
3. **Vanilla+ (Link Remodel 1.9).** O arquivo é `.rar`: libarchive com RAR
   para o PS4; overlay O3RO, `--romfs-overlay`.
4. **Pipelines de GPU.** Pré-compilar as pipelines do backend Vulkan do PS4
   a partir do corpus de shaders, com progresso.
5. Opções na interface (menu do jogo): mods liga/desliga, idioma.

## 1.33 — fase 1

- `ports/ps4-product/src/forge/forge_core.{h,cpp}`: port em C++ portátil de
  `ctr_rom.py`, `forge_gui.match_extracted_recipe`, `input_copy_adapter.py`,
  `oot3d_region_assets.py`, `romfs_identity.py`, `data_compatibility.py` e
  `build_process_manifest.py`; `forge_sha256.cpp` (o console não tem
  biblioteca de hash).
- `src/ps4_forge.cpp` (`Fast::OrbisPrepareGame`, chamado no `main` do ORBIS
  antes dos argumentos): a `.3ds`/`.cci` mais nova em
  `/data/triaevum/forge/` que ainda não foi preparada (nome, tamanho e
  mtime em `game/forge-state.json`) vira `/data/triaevum/game/inputs` +
  `process-manifest.json`. Tudo é montado em `forge/.work` e só substitui o
  jogo quando verificou. Progresso e erros: notificações do sistema e boot
  log; erro também em `forge/forge-error.txt` (o jogo anterior continua).
  Sem jogo nenhum, a notificação diz como enviar a ROM e o app fecha.
- Do PKG (`/app0/forge`), quando faltam: `atlas_overrides.o3tu` (gerado no
  build pelo `build_texture_pack` do Forge; idêntico ao do PC),
  `zelda_link_ext.zar` e `topscreen_ui.json` (`topscreen_ui.example.json`).
- Receitas em `ps4/forge/recipes` (da release 0.8.0-beta.1.2; o adaptador
  USA só tem operações de cópia, nenhum byte do jogo).
- Teste no PC: `tools/test-forge-host.sh ROM` compila o núcleo para o host,
  prepara o jogo e compara com o Forge em Python. ROM USA do Daniel: receita
  `oot3d-usa-input-canonical-eur-ef210566`, inputs = os da receita (code
  `16a6b0aa`, exheader `dbe5fa01`, RomFS `dae62037`), manifesto idêntico
  byte a byte, fingerprint do RomFS idêntico; 6 s no PC. O caminho EUR (sem
  adaptador) não foi testado com ROM real.
