# Building and contributing

This page is for people who want to build Mosaik themselves, fork it or add something to it. To install it, see the [README](README.md) and the [wiki](https://github.com/diapolon/mosaik_cfw/wiki).

Most of the comments in the code and the developer notes are in Italian. The data formats (emulators, themes, filters, languages) and the controls are described in [frontend/README.md](frontend/README.md), the SD image and the hardware tests in [m2/README.md](m2/README.md).

## What you need

- macOS or Linux with Docker (the console build runs in containers: `linux/amd64` for the TrimUI kit, `linux/arm64` for the tests and some programs).
- For the Mac version of the launcher and runner: SDL2, SDL2_ttf, SDL2_image, SDL2_mixer (for example from Homebrew).
- The files below in `deps/`. None of them is in the repository: ROMs, BIOS files and the TrimUI firmware must never be committed.

## Dependencies (`deps/`)

| Folder | What | Where |
|---|---|---|
| `trimui-sdk/dl` | TrimUI kit: `aarch64-linux-gnu-7.5.0-linaro.tgz`, `SDK_usr_tg5040_a133p.tgz`, `SDL2-2.26.1.GE8300.tgz` | https://github.com/trimui/toolchain_sdk_smartpro |
| `firmware/tg5040/dl` | Smart Pro firmware `trimui_tg5040_20251128_v1.1.1.zip` | https://github.com/trimui/firmware_smartpro |
| `firmware/tg4040/dl` | Brick Pro firmware `trimui_tg4040_20260717_v1.1.1.7z` | https://github.com/trimui/firmware_brickpro |
| `ffmpeg` | `ffmpeg-9.0.2.tar.xz` (Video app) | https://ffmpeg.org/releases |
| `freedoom` | `freedoom-0.13.0.zip` | https://github.com/freedoom/freedoom/releases |
| `devilutionx` | `devilutionx-src-fully-vendored-1.5.5.tar.xz` | https://github.com/diasurgical/devilutionX/releases |
| `deadcast` | `DC-DEADCAST-1.2.5.pak.zip` (`toolchain/fetch-deadcast.sh`) | https://github.com/tyrannotorus/emulator-dreamcast-deadcast |

Emulators and games, cloned with git at these commits (`git clone URL folder && git -C folder checkout COMMIT`):

| Folder | Repository | Commit |
|---|---|---|
| `fbneo` | https://github.com/libretro/FBNeo | d15071abf141 |
| `mame2003-plus` | https://github.com/libretro/mame2003-plus-libretro | d41198a39d33 |
| `mame2010-libretro` | https://github.com/libretro/mame2010-libretro | dff8aadd1c3f |
| `libretro-fceumm` | https://github.com/libretro/libretro-fceumm | e0f3086a8104 |
| `snes9x` | https://github.com/libretro/snes9x | fae2fea08f74 |
| `Genesis-Plus-GX` | https://github.com/libretro/Genesis-Plus-GX | b9c231644197 |
| `gambatte-libretro` | https://github.com/libretro/gambatte-libretro | 77f2567d718b |
| `mgba` | https://github.com/libretro/mgba | 7a12d6d4b9ac |
| `beetle-pce-fast-libretro` | https://github.com/libretro/beetle-pce-fast-libretro | 2de1ce1c3f23 |
| `beetle-ngp-libretro` | https://github.com/libretro/beetle-ngp-libretro | a50d5ac288a8 |
| `pcsx_rearmed` | https://github.com/libretro/pcsx_rearmed | 7b0e8f8fadfb |
| `stella2014-libretro` | https://github.com/libretro/stella2014-libretro | 7d1361e407e6 |
| `prosystem-libretro` | https://github.com/libretro/prosystem-libretro | 8a88014287c7 |
| `Gearcoleco` | https://github.com/drhelius/Gearcoleco | 2805e70fd56c |
| `FreeIntv` | https://github.com/libretro/FreeIntv | ef3e0fe322be |
| `libretro-handy` | https://github.com/libretro/libretro-handy | bc55d462f0b2 |
| `beetle-wswan-libretro` | https://github.com/libretro/beetle-wswan-libretro | 4b01295838ea |
| `PokeMini` | https://github.com/libretro/PokeMini | 132111b76343 |
| `beetle-vb-libretro` | https://github.com/libretro/beetle-vb-libretro | d5ae54c8e23b |
| `fuse-libretro` | https://github.com/libretro/fuse-libretro | e997e2bc32c8 |
| `blueMSX-libretro` | https://github.com/libretro/blueMSX-libretro | e3086eb5d36d |
| `vice-libretro` | https://github.com/libretro/vice-libretro | 76cde0b2cc4e |
| `dosbox-pure` | https://codeberg.org/schelling/dosbox-pure | 9cccfa21cb11 |
| `libretro-uae` | https://github.com/libretro/libretro-uae | d366f77c60e5 |
| `scummvm` | https://github.com/scummvm/scummvm | 7d2709da5136 |
| `ppsspp` | https://github.com/hrydgard/ppsspp (with submodules) | fa50bb197606 |
| `flycast` | https://github.com/flyinghead/flycast (with submodules) | 72b8e67141d8 |
| `mupen64plus/mupen64plus-core` | https://github.com/mupen64plus/mupen64plus-core | daae68e2cc87 |
| `mupen64plus/mupen64plus-ui-console` | https://github.com/mupen64plus/mupen64plus-ui-console | c8ac4862a019 |
| `mupen64plus/mupen64plus-audio-sdl` | https://github.com/mupen64plus/mupen64plus-audio-sdl | 9fbc24c80902 |
| `mupen64plus/mupen64plus-input-sdl` | https://github.com/mupen64plus/mupen64plus-input-sdl | 842c39e89749 |
| `mupen64plus/mupen64plus-rsp-hle` | https://github.com/mupen64plus/mupen64plus-rsp-hle | 4cdc3051efba |
| `mupen64plus/mupen64plus-video-rice` | https://github.com/mupen64plus/mupen64plus-video-rice | 19c72e64aa70 |
| `mupen64plus/mupen64plus-video-glide64mk2` | https://github.com/mupen64plus/mupen64plus-video-glide64mk2 | 3d323245282e |
| `crispy-doom` | https://github.com/fabiangreffrath/crispy-doom | fdeabe4c23a8 |
| `mrboom-libretro` | https://github.com/libretro/mrboom-libretro | 40ac32020b54 |

Mosaik's changes to these projects are in `toolchain/patches/`; the build scripts apply them. `build-info.txt`, attached to each release, lists the exact commits of that build, and the source archives attached to the releases are these clones at those commits.

## Build

From the root of the repository:

```bash
# once: the TrimUI kit image and the build volume
docker build --platform linux/amd64 -t trimui-tg5040 -f toolchain/Dockerfile deps/trimui-sdk/dl
docker volume create trimui-build

# once per model: extract the firmware (into /build/fw/<model>)
docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/toolchain/analyze-firmware.sh deps/firmware/tg5040/dl/trimui_tg5040_20251128_v1.1.1.zip tg5040
docker run --rm --platform linux/arm64 -v trimui-build:/build -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/toolchain/analyze-firmware.sh deps/firmware/tg4040/dl/trimui_tg4040_20260717_v1.1.1.7z tg4040

# libraries, programs and cores
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro trimui-tg5040 sh /trimui/toolchain/build-deps.sh
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui -w /trimui/frontend trimui-tg5040 make -f Makefile.tg5040 check
docker run --rm --platform linux/amd64 -v trimui-build:/build -v "$PWD":/trimui:ro trimui-tg5040 sh /trimui/toolchain/build-cores.sh
```

The standalone emulators, the Video app libraries and the Linux games have their own scripts, each with the command to run in its first lines: `toolchain/build-ppsspp.sh`, `build-flycast.sh`, `build-mupen64plus.sh`, `build-ffmpeg.sh`, `build-devilutionx.sh`, `build-linux-games.sh`.

Then:

```bash
sh m2/build-m2.sh              # dist/arcade-system.squashfs and the SD images dist/m2-smartpro.img, dist/m2-brickpro.img
python3 toolchain/make-site.py # dist/site: compressed images, Wi-Fi update files, Store packages and catalogue, build-info.txt
```

`m2/build-m2.sh` only packs what is already built in the volume: rebuild the programs first. `make-site.py` signs the update list and the Store catalogue with the key described below.

## Tests

Each test is a script run in a container, for example:

```bash
docker run --rm --platform linux/arm64 -v "$PWD":/trimui:ro alpine:3.20 sh /trimui/m2/test-store.sh
```

The scripts are `m2/test-*.sh`; each file starts with a comment that says what it covers. Loop mounts, `switch_root`, the power chip and the Wi-Fi can only be checked on a console.

## Your own key (forks)

The consoles accept only updates and Store catalogues signed with the Mosaik key, which is not public. A fork needs its own key and its own address:

```bash
mkdir -p ~/.config/arcade-update
openssl ecparam -name prime256v1 -genkey -noout -out ~/.config/arcade-update/update-key.pem
{ echo 'static const char UPDATE_PUBKEY[] ='; openssl ec -in ~/.config/arcade-update/update-key.pem -pubout | sed 's/.*/"&\\n"/'; echo ';'; } > frontend/src/launcher/update_pubkey.h
echo "https://github.com/YOUR-NAME/YOUR-REPO/releases/latest/download" > UPDATE_URL
```

Then rebuild everything. Keep the key safe: consoles installed with your images accept updates only from it.

## Adding an emulator

1. A JSON file in `frontend/emulators/<id>.json` (fields in `frontend/src/launcher/games.h`, format in `frontend/README.md`): name, core, ROM folder, extensions, and `launch` for a standalone program.
2. Build it in `toolchain/build-cores.sh` (libretro cores) or in a script of its own (standalone programs).
3. Pack it in `toolchain/package.sh`; licenses go with the files.
4. Its state in `frontend/package/store-stato.json`: `experimental` until it has been tried on a console, `models` if it works only on some models, `min_system` if it needs a newer system.
5. `python3 toolchain/make-store.py` builds the packages and the signed catalogue.
6. Every new text in all the files of `frontend/lang/`.
7. Add the component to `licenses/THIRD-PARTY.md`.

## Rules

- No ROMs, BIOS files or firmware in the repository.
- Commits use a noreply address; no SSIDs, IP or MAC addresses in files, issues or notes.
- The internal names `Arcade/`, `arcade-system.squashfs` and `/etc/arcade-version` must not change: existing cards and updates depend on them.
- The system must never write to the console's internal memory.

The code written for Mosaik is under the LGPL v3 (`LICENSE-NOTICE`); third-party components are listed in `licenses/THIRD-PARTY.md`.
