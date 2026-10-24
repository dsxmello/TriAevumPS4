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

## 1.33 no console (2026-10-24)

- Extração e adaptação rodaram (~60 s), mas o RomFS adaptado não bateu
  (`Incompatible execution data: romfs`). Comparando o arquivo do console com
  o do PC: só os bytes de `0x234E8` (fim da tabela de arquivos, o último
  patch gravado por cima da cópia com `pwrite`) até `0x28000` (32 KiB)
  diferem, com bytes de heap no lugar dos dados. O patch da tabela QM, mais
  adiante, saiu certo.
- `std::filesystem::remove_all` não funciona no PS4: o libc++ usa
  `openat`/`fdopendir`/`unlinkat`, e o `unlinkat` do SDK é um stub. O
  `.work` (470 MB) ficou para trás.

## 1.34 — tela do Forge e escrita sequencial

- Todo arquivo de saída é escrito uma vez, do começo ao fim (`write`), sem
  regravar faixas: os patches do RomFS adaptado são aplicados no buffer da
  cópia. O RomFS extraído é conferido em disco (hash do arquivo escrito).
- `RemoveTree`/`RemoveFile`/`CopyFile` próprios, só com caminhos absolutos
  (`RemoveTree` recusa caminhos relativos ou rasos).
- Tela do Forge (ImGui, layout do Forge do PC: ROM, dados privados, idioma,
  texturas HD, mods opcionais, status, barra de progresso, "Prepare and
  install", "Launch game"), com backend Vulkan próprio. Abre com ROM nova,
  sem jogo, ou segurando L1+R1 ao abrir o app; observa a pasta (a ROM pode
  chegar por FTP com a tela aberta). A preparação roda numa thread. "Launch
  game" reinicia o app (`sceSystemServiceLoadExec`) para o jogo subir num
  processo limpo. Texturas HD, Vanilla+ e idioma aparecem desabilitados
  ("not available yet on PS4") até as fases 2, 3 e 5.
- Fonte DroidSans (Apache 2.0, do Dear ImGui vendorizado) em `/app0/forge`.

## 1.34 no console / 1.35 (2026-10-24)

- 1.34: preparação no console funcionou (USA -> canônica EUR verificada) e o
  "Launch game" reiniciou o app direto no jogo. O controle não funcionava na
  tela do Forge: o subsistema de controle do SDL era ligado antes de
  `sceUserServiceInitialize` (`PS4_JoystickDetect ... 0x80960002`) e o loop
  do Forge nunca chamava `SDL_PumpEvents`, que é onde o SDL detecta o pad.
- 1.35: user service antes do SDL, `SDL_PumpEvents` a cada quadro (e até
  300 ms antes de checar L1+R1), foco na janela no primeiro quadro (a
  navegação por controle age na janela focada). Depois de instalar, um
  diálogo pergunta se a ROM deve ser apagada de `/data/triaevum/forge/`
  ("Delete ROM" / "Keep ROM").

## 1.36 (2026-10-24)

- 1.35 achava o DualShock no boot mas lia L1+R1 soltos: os primeiros
  estados depois de abrir o pad vêm vazios. A checagem acompanha o pad por
  até 1 s e registra o que viu (`[forge] L1+R1: ...`).
- Confirmado no console (Daniel): L1+R1 abre o Forge, o controle navega a
  tela, a preparação conclui, o diálogo de apagar a ROM aparece e "Launch
  game" abre o jogo. Fase 1 concluída.
