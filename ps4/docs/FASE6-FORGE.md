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

## 1.37 — fase 2: texturas HD (Vanilla UI/HUD 3.2)

- `src/forge/forge_textures.{h,cpp}`: port de `ui_texture_pack.py`. O zip do
  autor é conferido pelo contrato das receitas (451.655.008 bytes,
  `5ff478db`), só as 76 texturas catalogadas são extraídas (libzip), cada uma
  conferida por tamanho e SHA-256 em disco, no layout do Forge do PC
  (`/data/triaevum/mods/ui-textures/vanilla_ui_hud-3.2/{common,languages}`,
  `source.json`). O catálogo (`/app0/forge/ui_texture_pack.json`) é gerado no
  build por `tools/forge_catalogs.py` a partir do `VANILLA_UI_HUD` do Forge
  do PC. Teste no PC (`FORGE_TEXTURES_ZIP=... tools/test-forge-host.sh`):
  76 de 76 arquivos idênticos ao pack do Forge do PC.
- `src/ps4_forge_net.cpp`: download por `libSceHttp` (redirecionamentos
  seguidos: gamebanana.com/dl -> files -> filecache), conferido por tamanho e
  SHA-256 antes de ficar com o arquivo. Tenta com verificação TLS; se o
  repositório de certificados do console recusar o servidor (erros
  0x8095xxxx/0x804350xx), repete sem ela: o SHA-256 é o que prova o arquivo.
  Vai para `/data/triaevum/downloads/`; um zip mandado por FTP para
  `/data/triaevum/forge/` também serve.
- Tela: seção HD como a do PC ("Download HD textures", "Look for the .zip",
  checkbox "baixar com Prepare and install", ligado por padrão, em
  `/data/triaevum/config/forge.json`) e status (instalado / guardado / não
  guardado). "Prepare and install" faz a ROM quando é nova e as texturas
  quando estão guardadas ou podem ser baixadas, em passos.
- Jogo: com o pack pronto, `--ui-texture-pack` e `--topscreen-ui-atlas-hd`
  (o atlas HD do pack) entram nos argumentos do PS4
  (`Fast::OrbisForgeLaunchArguments`).

## 1.37 no console / 1.38 (2026-10-24)

- 1.37: o download falhou com `sceHttpSendRequest = 0x80431068`
  (`SCE_HTTP_ERROR_TIMEOUT`, pela tabela do vitasdk: os erros de DNS são
  `0x80436xxx`). Nada respondeu no endereço resolvido. Suspeita: DNS do
  console bloqueando a internet (comum em consoles mantidos fora da PSN).
- 1.38: o Forge resolve o host antes (`sceNetResolverStartNtoa`) e registra
  IP e tempos; IP 0.0.0.0, loopback ou de rede local vira erro explicando o
  DNS bloqueado; timeout no modo verificado também repete sem a verificação
  TLS (handshake travado); a mensagem de erro sugere o teste de conexão do
  PS4 ou o zip por FTP. A varredura da tela não lança mais exceção a cada
  2 s quando o pack não existe.

## 1.39 no console / 1.40 (2026-10-24)

- 1.39: o download funcionou (Daniel: um pouco lento) e o Forge instalou o
  pack, mas o jogo não mostrou as texturas HD. Duas causas:
  - `AzaharTexturePackRuntime` só liga depois de um `Configure()`, que no
    desktop o renderer Vulkan chama a cada revisão das configurações
    gráficas (`gfx_vulkan_frame.cpp`). O renderer do PS4 nunca chamava: o
    `SetBaseDirectories` do Forge ficava guardado e toda consulta respondia
    "Disabled" (nem o HUD do TopScreen, que usa esse runtime, mudava).
  - O backend Vulkan do PS4 não consultava o pack ao enviar texturas PICA
    (o desktop faz em `gfx_vulkan_pica_textures.cpp`).
- 1.40:
  - `gfx_orbis_rendering.cpp` chama `Configure()` com as configurações de
    texture pack a cada revisão, como o desktop.
  - `Ps4VkPicaRenderBackend::SetTextureReplacements`: no primeiro envio de
    uma textura o nível base vai para `ResolveOrQueue`; pendente, é
    consultado (`PollQueued`) nos usos seguintes; pronto, a imagem nativa é
    trocada por uma RGBA8 de um nível com a HD. O backend continua sem
    depender do runtime de texturas (callbacks do produto).
  - Menu do jogo (touchpad), aba Game: seção "Mods and textures" com o
    status do pack e "Open TriAevum Forge", que grava
    `/data/triaevum/forge/open-forge` e reinicia o app direto no Forge.
  - O Forge também escreve em `/data/triaevum/forge/forge.log` (o boot log é
    sobrescrito no reinício para o jogo); o download registra MB/s.

## 1.40 no console / 1.41 (2026-10-24)

- 1.40 fechou ao iniciar logo depois de detectar o controle, antes da linha
  `[forge] L1+R1`, a primeira que passa pelo `ForgeLog` novo (que formatava
  a hora com `localtime`). Sem registro no crash.log.
- 1.41: `ForgeLog` sem `localtime` (segundos desde a época) e com `fflush`
  a cada linha, para o boot log mostrar até onde o app chegou.

## 1.41 no console / 1.42 (2026-10-24)

- 1.41 abriu normalmente (a falha da 1.40 era o `localtime`) e a
  substituição de texturas passou a funcionar, mas as maiores falharam:
  `HD texture 4096x4096 exceeds the upload ring` (64 MiB, o ring inteiro);
  na falha a substituição era abandonada e o jogo ficava com a original.
- O "Open TriAevum Forge" reinicia o jogo em segundos: o marcador de
  lançamento (`TriAevum.json.launching`, vive 1 minuto) fazia o próximo
  início achar que o anterior tinha falhado e voltar tela, resolução e
  anti-aliasing aos padrões (`TRIAEVUM_SAFE_PRESENTATION`).
- 1.42: HD acima de 2048 de lado é reduzida (caixa 2x2) até caber: a saída
  é 1920x1080, e 4096x4096 custava 64 MiB cada; o envio vai em faixas de
  16 MiB, cada uma com sua reserva no ring. O "Open TriAevum Forge" apaga o
  marcador antes de reiniciar.

## 1.42 no console / 1.43 (2026-10-24)

- 1.42: algumas texturas HD aparecem e somem; no HUD do TopScreen os
  botões C viram quadrados cinza-claro opacos (foto do Daniel). Sem erros
  no log. Orientação descartada (o runtime entrega no layout do decoder
  nativo, o mesmo do PS4); o cache do runtime não tem descarte por memória.
  Suspeita: alfa perdido em algum formato/caminho. Pendente.
- 1.43:
  - Configurações do console em `/data/triaevum/config/forge.json`
    (leitura e escrita por chave; antes cada gravação apagava as outras):
    `hd_textures_enabled` e `logs_enabled` (ambas ligadas por padrão),
    além de `download_hd_textures_with_prepare`.
  - Menu do jogo, "Mods and textures": "HD UI textures" (só com o pack
    instalado) e "Diagnostic logs", valendo no próximo início; a tela do
    Forge tem os mesmos checkboxes. Com as texturas HD desligadas, o jogo não
    recebe `--ui-texture-pack`/`--topscreen-ui-atlas-hd`.
  - Logs desligados: `OrbisProcessInit` lê a chave antes de abrir o boot
    log e manda stdout/stderr para `/dev/null` (Mesa incluído); as linhas
    periódicas `perf`/`game`/`geometry cache` não são montadas
    (`Fast::gOrbisDiagnosticLogs`).
  - Diagnóstico: cada substituição HD aparece uma vez no boot log (arquivo
    do pack, tamanho e formato da textura nativa, tamanho final).
