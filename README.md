# Unreal Tournament (v400) for PlayStation 5

A PS5 homebrew port of Unreal Tournament 99, the original Unreal Engine 1 game. It runs the
[ut99dc](https://github.com/Andiweli/UT99-Android) source (a Unreal Engine 1 C++ port, as used by the UT99-Android
project) with a Vulkan renderer on RADV, through the
[PS5 Vulkan](https://github.com/mihawk-99/PS5_Vulkan) stack. It plays with the DualSense, at 2560x1440.

It needs a PS5 that already runs homebrew, and your own copy of the game (the [retail v400 CD](https://archive.org/download/ut-99_202512/UT99.iso)).
**No game data, Epic Games source code or Sony software is included in this repository.**

## Status

Tested on one console, 2026-11-21 to 2026-11-22. "Seen" means it ran there and I watched it, or read it in the
console's log; nothing else is claimed. There is no CI.

| Area | Seen on the console | Not yet verified |
| --- | --- | --- |
| Start-up and exit | Boots to the menu, plays the intro level, quits cleanly to the home screen | |
| Rendering | Deathmatch with bots at 2560x1440 on a 59.94 Hz display; the display lists 3840x2160, 2560x1440 and 1920x1080 and the title picks the 1440p mode; a full match with no stall | 3840x2160; 120 Hz; the video menu's resolution list |
| Menus | The stock menus with the pad as a cursor; Preferences > Video with the port's **Show Statistics** switch | |
| Input | DualSense sticks, buttons, triggers; a button map that needs no setup | Rebinding in the controls menu; a USB mouse or keyboard; several controllers |
| On-screen keyboard | Opens on the player name field and on the multiplayer menu's text fields | The chat line during a match (no keyboard there) |
| Sound | 48 kHz stereo effects: weapons, pickups, voices, taunts | **Music**: the build has no module-music (`.umx`) player, so there is none |
| Statistics | Frame rate and a frame-time graph, from the video menu | |
| Field of view | Changed in play with L3 + D-pad left or right, kept across launches | |
| Network | | Any online or LAN match; name lookup is a stub, so a server has to be given by its address |

Known limits:

- No music (see above). The loudest mixes can clip.
- Text typed during a match (chat) does not open the on-screen keyboard.
- Online play and server lookup by name are untested; the console's name lookup is a stub.
- The port runs one player per console.

## Install

You need a PS5 that already runs homebrew: an enabler such as etaHEN, ShadowMountPlus (it shows titles in
`/data/homebrew` on the home screen) and an FTP server. Setting that up is not covered here.

1. **Get the title.** Download `UT99-PS5-v1.0.0.zip` from the
   [latest release](../../releases/latest) and unzip it. It holds one folder, `PPSA99781`.
2. **Get the game data** (not included): the folders `System`, `Maps`, `Textures`, `Sounds` and `Music` from your own
   Unreal Tournament, version 400, for example the retail CD. Epic has released the game for free. Copy them into `PPSA99781/UnrealTournament/`.
   The `.exe`, `.dll` and `.icd` files are not needed.
3. **Keep the patched menus.** The zip already holds the two patched menu packages `UMenu.u` and `UTMenu.u`
   (from the [UT99-Android project](https://github.com/Andiweli/UT99-Android/tree/main/app/src/main/assets/ut99_patches/System))
   in `PPSA99781/UnrealTournament/System/`. They hold the font-size option and the widescreen FOV switch the
   port's menus use. If your copy step asks to overwrite them, keep the ones from the zip.
4. **Copy the folder to the console** over FTP, so that it becomes `/data/homebrew/PPSA99781/`:

   ```text
   /data/homebrew/PPSA99781/eboot.bin
   /data/homebrew/PPSA99781/sce_sys/...
   /data/homebrew/PPSA99781/sce_module/libc.prx
   /data/homebrew/PPSA99781/port/System/UnrealTournament.ini
   /data/homebrew/PPSA99781/UnrealTournament/{System,Maps,Textures,Sounds,Music}
   ```
5. **Launch "Unreal Tournament"** from the home screen. If the tile does not appear, let ShadowMountPlus rescan.

The first launch creates `PPSA99781/data/` (your settings, saved games and `ut99.log`; the previous run's log is
`ut99.prev.log`) and sets the defaults below. Your own changes in the menus stay after that.

To update, close the game and replace `eboot.bin` and `sce_sys/`; the game's files and `data/` stay. If the home screen
keeps an old icon, remove the title from the home screen and let it be added again.

## Defaults

Applied once, on the first launch of a version that has them: a 2560x1440 frame, the stock brightness, the menu's font
size on **Double** (`GUIScale=2`), and a field of view of 95. All of them are in
`data/System/UnrealTournament.ini` and the in-game menus.

## Controls

| DualSense | In play | In menus |
| --- | --- | --- |
| Left stick | Move | Cursor |
| Right stick | Look | Cursor |
| Cross | Jump | Click |
| Circle | Crouch | Back |
| Square | Use item | Enter |
| Triangle | Next weapon | Back |
| R2 / L2 | Fire / alternate fire | |
| L1 | Translocator | Page up |
| R1 | Next weapon | Page down |
| D-pad | Arrow keys (the stock bindings) | Arrow keys |
| R3 | Look straight ahead | |
| L3 + D-pad left or right | Field of view, 5 degrees a step (70 to 130) | |
| Touch pad click | Scores | |
| Options | Menu | Back |

**On-screen keyboard.** Click a text field with Cross. Left stick or D-pad to move, Cross types a key, Square deletes,
Triangle adds a space, L1 switches between capitals and small letters, R1 or Options confirms (the text replaces the
field's), Circle cancels.

**Show Statistics.** Preferences > Video, right below Use Dynamic Lighting. It draws the frame rate and a 420-frame
frame-time graph with its minimum and maximum, at the left edge. It is kept across launches.

## How it is put together

The engine is Unreal Engine 1 as ported by the ut99dc project. Its source is Epic Games', not mine, and is **not in this
repository**. What is here:

| Path | Does |
| --- | --- |
| `ps5/engine/ut99-ps5.patch` | The changes to the engine source (under `UT99_PS5_PAD`): the pad as an SDL game controller and its keys, the on-screen keyboard and the Show Statistics switch, audio and file-system fixes for the console. A `git diff` against UT99-Android at `4aa596a` |
| `ps5/engine/NVulkanDrv/` | The Vulkan render device |
| `ps5/glue/` | The title's own code: `ps5_launch.cpp` (start-up, ini defaults, the log), `ps5_present.cpp` (frames to the display), `ps5_io.cpp` (the pad and sound through the console's libraries), `ps5_keyboard.cpp` (the on-screen keyboard and the statistics drawing) |
| `ps5/patches/sdl2/` | The changes that make SDL2 2.28.5 build for the console, with virtual joystick and audio |
| `tools/` | `make_ini.py` (the first-run ini), `make_kbfont.py` (the keyboard's font), `package-release.sh`, the build steps that run in the container |
| `docker/` | The Linux build image |
| `art/` | The home-screen art (see Licences) |

The engine's file writes to the console's storage were found to stall the game for about three seconds; its own log file
is therefore not written, and every log line goes to `data/ut99.log`.

## Building

Not yet reproducible from this repository alone: the engine source has to come from elsewhere (see
[`LICENSING.md`](LICENSING.md)). With it:

1. Check out [UT99-Android](https://github.com/Andiweli/UT99-Android) at `4aa596a`, put the ut99dc source in its
   `third_party/ut99dc` and SDL2 2.28.5 in `third_party/SDL2`, as that project describes.
2. In that checkout run `git apply /path/to/ps5/engine/ut99-ps5.patch` and copy `ps5/engine/NVulkanDrv/` to
   `third_party/ut99dc/Source/NVulkanDrv/`.
3. Edit `UT99_SRC_REL` in `tools/pins.env` to point at it, then, with Docker running:

```bash
./build.sh title      # engine + SDL2 + RADV + glue linked into dist/PPSA99781 (the launcher's own link failing is expected)
./build.sh package    # your game files and the port ini into the title (GAME_DIR=<the CD's files>)
tools/package-release.sh 1.0.0   # dist/UT99-PS5-v1.0.0.zip: the title without game files
```

It also needs the PS5 stack (`PS5_Vulkan` with RADV, `PS5_PayloadSDK`) that `tools/pins.env` pins; `build.sh` fetches it
into a Docker volume on first use.

Home-screen art: `icon0.png` (512x512) and `pic0.dds` / `pic1.dds` (3840x2160) in `sce_sys/`.

## Credits

- **Epic Games** and **Digital Extremes** made Unreal Tournament.
- **The ut99dc project** ported the Unreal Engine 1 source to SDL and Dreamcast, and **Andiweli** (UT99-Android) built the
  Android version this port started from, including the patched `UMenu.u` and `UTMenu.u`.
- **Mihawk** ([mihawk-99](https://github.com/mihawk-99)): PS5_Vulkan, PS5_Mesa (RADV on the console) and the PS5_PayloadSDK
  fork, which this port is built on.
- **Mesa** and its RADV developers; **ps5-payload-dev** for the payload SDK; **SDL**.

## Licences

See [`LICENSING.md`](LICENSING.md). Unreal Tournament, its engine, its game data and artwork belong to Epic Games.

This is an independent homebrew project, not affiliated with or endorsed by Epic Games or Sony Interactive
Entertainment. Running homebrew needs a modified console, which may void its warranty or breach the platform's terms of
service.
