# Release estável — lista de tarefas (aberta em 2026-11-04)

Estado de partida: build 1.75 (commit e96c7551), PGO da 1.65
(`pgo/product-165.profdata`), testado só em GoldHEN 13.52. Meta de
desempenho da fase 5: ~16 ms de CPU e de GPU por quadro apresentado.

## Metas do produto (Daniel, 2026-11-04)

1. Instala o `.pkg`, manda a ROM por FTP e o próprio console prepara tudo
   para o jogo rodar (Forge no console).
2. Texturas HD e mods (tradução e outros) instaláveis pelo console.
3. Savestates.
4. Shaders e pipelines compiladas antes da primeira vez que o jogo roda,
   para não haver travadas de shader durante a gameplay.
5. Estabilidade.

Legenda: `[ ]` aberto, `[x]` feito, **(decidir)** = depende do Daniel.

## 1. Repositório e infraestrutura

- [x] Repositório próprio `dsxmello/TriAevumPS4` (não-fork, histórico
      completo, público), criado em 2026-11-04.
- [x] README/`ps4/README.md`/`sync-upstream.sh` sem a palavra "fork".
- [ ] Descrição, topics e link do upstream na página do repositório.
- [ ] Remote `upstream` (coccofresco/TriAevum) continua no clone; testar
      `ps4/tools/sync-upstream.sh` no repositório novo.
- [ ] Consertar o host test `build/tri-host` (`nlohmann/json_fwd.hpp` fora
      do include path) para os testes de PC rodarem da árvore git e não só
      da cópia em `triaevum-ps4-vk`.
- [ ] CI mínima no GitHub Actions: testes de PC da fila de render, do
      worker GSP (`oot3d_native_pica_gsp_worker_tests.cpp`) e do DSP
      com ASan/UBSan/TSan (sem SDK do PS4).
- [ ] `tools/env.sh`: IP padrão do console está velho (192.168.0.15);
      tirar o padrão ou ler de variável.
- [ ] As duas árvores (`TriAevumPS4-git` e `../TriAevum`) idênticas, e o
      `triaevum-ps4-product.patch` regenerado no commit da release.

## 2. Desempenho

- [ ] Medir a 1.75: Market, mesma rota, com e sem
      `/data/triaevum/frames_in_flight_2`; comparar `present` e fps no
      mesmo número de draws. **(decidir)** o padrão (1 ou 2 quadros em voo;
      2 = +1 quadro de latência de input).
- [ ] Market pesado a 720p: thread do jogo ~10,8 ms por quadro do jogo a
      294 draws; cena 77 a 13,7 ms. Próximo alvo de CPU do jogo.
- [ ] Quedas em combate (CPU do jogo 9 -> 14 ms por quadro do jogo):
      remedir depois do worker GSP (1.70+) e da 1.75.
- [ ] GPU na escala 6 (Market limitado pela GPU, ~3,2 ms de espera):
      **(decidir)** a escala de render padrão (A/B 3 / 4 / 6 nunca feito).
- [ ] Sobra de CPU na `render`: interpolação visual ~12%, submit do
      DrawPlan 5,5%, memcpy/memset 8%, malloc 4%.
- [ ] Remedir tudo com texturas HD ligadas (memória e GPU).
- [ ] PGO final: nova COLETA com o código da release (o profile da 1.65
      invalida se o código gerado ou o header do runtime mudar) e conferir
      "pgo: profile written" no boot log antes de baixar.

## 3. Estabilidade

- [ ] Auditoria das esperas em cv: toda espera nova com timeout e
      reconferência (perda de notificação no console, 1.57). Listar as que
      ainda não têm.
- [ ] Sessão longa (2 h+) sem travar e sem memória crescendo; watchdog sem
      entradas em `/data/triaevum_hang.log`.
- [ ] Zerar o jogo (ou pelo menos todas as dungeons e áreas abertas) numa
      build candidata; anotar cenas com problema visual ou de fps.
- [ ] Saves e savestates: gravar, fechar, reabrir; savestate depois de
      trocar de cena (o cache de geometria é limpo no `ResetPicaState`).
- [ ] Modo de repouso do PS4 (suspender/retomar) e sair pelo botão PS.
- [ ] Áudio: estalos/dessincronia com o worker DSP em cenas pesadas.
- [ ] Testar em pelo menos mais um firmware/GoldHEN além do 13.52.

## 4. Primeiro início (meta 1)

- [ ] Fluxo completo nas três receitas aceitas (EUR base, EUR Rev 1, USA
      via adaptador) partindo de console limpo (`/data/triaevum` apagado).
- [ ] Mensagens de erro claras: sem ROM, ROM de revisão não suportada,
      ROM encriptada, falta de espaço.
- [ ] Perguntar se apaga a ROM ao terminar; retomar se o console desligar
      no meio da preparação.
- [ ] README: a preparação no console existe desde a 1.33; atualizar o
      aviso de "milestone" e o passo a passo (pkg -> FTP -> abrir).
- [ ] Menu (L3+R3): revisar as opções; tirar ou esconder as de diagnóstico.

## 5. Shaders e pipelines antes do jogo (meta 4; fase 4 do Forge)

- [ ] Corpus de shaders/pipelines do jogo: registrar as combinações vistas
      numa sessão longa (zerar o jogo) e guardar no repositório/pkg.
- [ ] Etapa do Forge que compila todas as pipelines do corpus com barra de
      progresso, e cache de pipelines Vulkan gravado em `/data/triaevum`.
- [ ] Invalidar o cache quando a versão do pkg ou do Mesa mudar.
- [ ] Medir: contar pipelines criadas durante a gameplay (meta: zero
      depois da preparação) numa linha do log.

## 6. Texturas HD e mods (meta 2)

- [ ] Voltar o código guardado na 1.45: `src/forge/forge_textures`,
      `src/ps4_forge_net` (download HTTPS), `tools/forge_catalogs.py`,
      `SetTextureReplacements` do backend.
- [ ] Pack HD do jogo inteiro (HenrikoMagnifico) além do Vanilla UI/HUD:
      memória (o pack de UI já tinha 451 MB zipado), formato comprimido
      para a GPU do PS4, tempo de carga.
- [ ] Mods de tradução: overlay de RomFS (`--romfs-overlay`) e fontes; que
      formatos/tradutores suportar **(decidir)**.
- [ ] Vanilla+ (Link Remodel, `.rar` via libarchive).
- [ ] Instalação: baixar no console ou copiar por FTP para
      `/data/triaevum/mods/`; menu para ligar/desligar e ordenar mods.
- [ ] Mods que mudam o código do jogo não entram (o AOT está no eboot);
      deixar isso claro no README.

## 7. Savestates (meta 3)

- [ ] Ver o que o runtime já tem (o backend trata o load de savestate no
      `ResetPicaState`) e expor no menu: salvar/carregar, slots.
- [ ] Gravar em `/data/triaevum/savestates/` com versão do build; recusar
      savestate de outra versão com aviso.
- [ ] Testar com os workers ligados (GSP, DSP, plan helpers): o estado
      precisa ser lido com eles drenados.

## 8. Build de release

- [ ] Esquema de versão **(decidir)**: builds internas 1.xx; a estável como
      `v1.0.0` (ou outra) com tag no git.
- [ ] Padrões de release: "Diagnostic logs" desligado, profiler
      (`/data/triaevum/profile`) desligado, flags A/B documentadas ou
      removidas (`gsp_worker_off`, `frames_in_flight_2`).
- [ ] Guardar o `llvm-nm -C` da build de release (para mapear dumps de
      travamento enviados por usuários).
- [ ] `.pkg` assinado com `make-pkg.sh`, hash SHA-256 publicado.
- [ ] Notas de release / CHANGELOG (resumo da 1.24 até a estável).
- [ ] Modelo de issue para bug report (versão, firmware, revisão da ROM,
      `/data/triaevum_boot.log`, `/data/triaevum_hang.log`).

## 9. Licenças e conteúdo

- [ ] O `.pkg` não leva ROM, `code.bin`, RomFS, assets extraídos nem
      chaves; só a lógica traduzida (contrato de release pré-compilada do
      upstream). Conferir o conteúdo do pkg final.
- [ ] Módulo iMMERSE/MXAO fora do pkg (licenciado só para o TriAevum).
- [ ] `THIRD_PARTY_NOTICES.md` e `SOURCE_OFFER.md` cobrindo o que o pkg
      linka: Mesa/RADV, winsys do DolphinPS4, glslang, libc++, SDL2,
      libzip, OpenOrbis.
- [ ] Créditos do README (upstream, rlgcarrot, chubbycubby, OTPR26,
      DolphinPS4, OpenOrbis/PacBrew).

## Depois da estável

- Gravação Vulkan paralela na `render`.
