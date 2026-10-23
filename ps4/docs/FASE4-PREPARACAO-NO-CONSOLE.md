# Fase 4 — Preparação no console (o "Forge do PS4")

Objetivo (pedido do Daniel, 2026-10-23): o usuário instala o PKG dos
Releases, manda a ROM por FTP e o próprio jogo faz o que o Forge faz no PC —
prepara a ROM, gera os pipelines, configura o TopScreen — e depois pergunta se
pode apagar a ROM original e começa a jogar. Sem PC no meio.

## Por que é viável

O upstream adotou em 2026-09-05 o modelo dos recomps de Xbox 360
(`docs/TRIAEVUM_PRECOMPILED_RELEASE.md`): o pacote leva a lógica do título já
traduzida e compilada, e o Forge do usuário **não compila nada** — só
verifica/extrai a ROM, confere os hashes contra a receita da revisão e ativa o
módulo pronto. No PS4 o título já é linkado estaticamente no eboot, então o
equivalente é exatamente isso: o PKG traz o eboot com o título; o que falta é a
parte de dados, que hoje é feita no PC e copiada com `stage-product-data.sh`.

Continua fora do repositório e do PKG o que o contrato exclui: ROM,
`code.bin`, ExHeader, RomFS, assets extraídos, chaves, saves. O C++ gerado
continua fora do repositório (só o binário compilado vai no PKG, como a DLL do
desktop).

## Fluxo no console

1. Boot: se `/data/triaevum/game/ready.json` existe e confere com a versão do
   eboot → jogo direto.
2. Senão, tela de preparação (ImGui sobre o backend Vulkan já existente):
   procura `/data/triaevum/rom/*.3ds|*.cci` (ou `/data/triaevum/*.3ds`). Sem
   ROM: instrução com o caminho do FTP e o IP do console, e reprocura a cada
   poucos segundos.
3. Etapas, cada uma com barra de progresso e retomável (marcador por etapa):
   - **extract** — porte em C++ de `ctr_rom.py` (NCSD/NCCH, ExeFS com
     descompressão do `.code`, ExHeader, RomFS/IVFC; só ROMs decriptadas,
     como no desktop) para `/data/triaevum/game/inputs/`.
   - **verify** — SHA-256 de code/ExHeader/RomFS contra a receita embutida
     (`recipes/precompiled-titles.json` → tabela C++ gerada no build) e
     família de conteúdo; revisão não suportada = erro claro, nunca build.
   - **adapt** — adaptador de entrada da receita (USA → módulo EUR), porte de
     `input_adapters.py`/`input_copy_adapter.py` quando a receita pedir.
   - **prepare** — `process-manifest.json` (porte de `_build_process_manifest`
     de `forge.py`) e idiomas do jogo (`--game-language-info` já existe em C++).
   - **topscreen** — SingleScreen 3.0/3.1 já vem no pacote
     (`runtime/triaevum_resources/topscreen/`, desde o beta.1.2); gerar o
     `atlas_overrides.o3tu` e o pack de fonte (porte de
     `build_topscreen_texture_override_pack` / `prepare_qbf_font_pack`) e a
     config `topscreen_ui.json`. O pack HD (CC BY-NC-ND) é opcional: só se o
     usuário mandar o zip por FTP; o console não baixa nada.
   - **interior sun** — o modo `--prebuild-interior-sun` já é C++ do runtime:
     chamar a função direto (tabela de máscaras do release + RomFS do usuário).
   - **pipelines** — compilar no glslang do console todo o corpus portátil de
     shaders e as receitas de pipeline observadas, gravando o
     `VkPipelineCache` em `/data/triaevum/cache/` (resolve também os ~750 ms
     por pipeline compilado durante o jogo).
4. Pergunta: "Apagar a ROM original? (os dados preparados bastam)" — Sim /
   Não. Só então grava `ready.json` e entra no jogo.

## Ordem de implementação

1. Extração + verificação + manifest (sem isso nada roda); validar no host
   comparando byte a byte com o que o Forge do PC gera da mesma ROM.
2. Tela de preparação + pergunta de apagar a ROM.
3. TopScreen e interior sun.
4. Pré-compilação de pipelines.
5. `stage-product-data.sh` vira só ferramenta de desenvolvedor.
