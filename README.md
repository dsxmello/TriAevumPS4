# TriAevum PS4 — Echoes of Time on PlayStation 4

A PlayStation 4 port of [TriAevum](https://github.com/coccofresco/TriAevum), the
native AOT recompilation of *The Legend of Zelda: Ocarina of Time 3D*. The game
code runs natively on the console's CPU and draws through Vulkan (Mesa RADV on
the PS4's GNM driver), with TriAevum's single-screen interface.

This repository is a fork that carries **only the PS4 port**: the TriAevum
runtime sources the PS4 build compiles, the title translator, and the port
itself in [`ps4/`](ps4/). The desktop packaging, Android and Switch material of
upstream are not kept here; follow upstream for those.

> **Status: experimental.** It boots to the title screen and plays on a
> jailbroken console, but performance is not yet full speed everywhere and
> builds still change often. Back up your saves.

## Requirements

- A PS4 running a homebrew-enabled firmware with **GoldHEN** (tested on 13.52)
  and its FTP server.
- Your own decrypted *Ocarina of Time 3D* ROM (`.3ds`/`.cci`) of a revision
  upstream supports (EUR or USA). No ROM, game assets or keys are included.

## How it works

1. Install the `.pkg` from [Releases](https://github.com/dsxmello/TriAevumPS4/releases)
   with the GoldHEN package installer.
2. Copy your ROM to the console over FTP, to `/data/triaevum/rom/`.
3. Launch **TriAevum PS4**. On the first start it prepares the game, like
   upstream's Forge does on PC: it checks that the ROM is a supported revision,
   extracts the title data, imports the single-screen (TopScreen) interface,
   precomputes the lighting tables and builds the shader pipelines for the
   console's GPU.
4. When preparation finishes it asks whether to delete the original ROM (the
   extracted data is all it needs from then on), and the game starts.

Later starts go straight to the game. The prepared data, settings and saves
live in `/data/triaevum/`.

> The first-start preparation (step 3) is the port's current milestone; until
> it lands, the data is prepared on a PC with upstream Forge and copied with
> `ps4/tools/stage-product-data.sh`. See [`ps4/docs`](ps4/docs/).

## Controls

The PS4 pad maps to the 3DS controls as in upstream's controller layout.
**L3 + R3** opens the port's menu (render scale, resolution, screen layout,
widescreen), and settings persist across launches.

## Building

Everything happens on a Linux x86_64 host: the PacBrew OpenOrbis SDK, a static
libc++ 21, Mesa 26.0.8 RADV with the PS4 winsys from DolphinPS4, glslang
compiled for the console, then the product itself. The steps, host
requirements and every version of the port are documented in
[`ps4/README.md`](ps4/README.md) and [`ps4/docs/`](ps4/docs/).

The release package contains the translated title logic, as upstream's
[precompiled release contract](docs/TRIAEVUM_PRECOMPILED_RELEASE.md) does for
the desktop: never the ROM, `code.bin`, RomFS, extracted assets or keys.

Keeping up with upstream: `ps4/tools/sync-upstream.sh` merges
coccofresco/TriAevum into this fork and drops the paths listed in
[`ps4/sync/removed-paths.txt`](ps4/sync/removed-paths.txt) again.

## Credits

All of TriAevum is the work of its upstream author and contributors; this fork
only adds the console port. Please read upstream's
[credits](https://github.com/coccofresco/TriAevum#credit-where-it-belongs):
**TopScreen / SingleScreen** by rlgcarrot, **OoT3D Vanilla UI/HUD** by
chubbycubby, **Ocarina Reframed** by OTPR26, and Azahar, Ship of Harkinian,
libultraship and the SDL community. And **Nintendo and Grezzo**, who made the
game.

The PS4 side builds on [DolphinPS4](https://github.com/iHaiDeeZ/DolphinPS4)
(the RADV-on-GNM Vulkan stack and its toolchain conventions), dusklight-ps4,
the [OpenOrbis](https://github.com/OpenOrbis) toolchain and its PacBrew
packaging, Mesa and glslang.

## License

GPL-3.0-or-later, like TriAevum. Third-party components keep their own
licenses: see [LICENSE_SCOPE.md](LICENSE_SCOPE.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Upstream's iMMERSE/MXAO
module is licensed to TriAevum only; the PS4 package does not contain it.

TriAevum PS4 is independent and is not affiliated with or endorsed by Nintendo
or Sony.
