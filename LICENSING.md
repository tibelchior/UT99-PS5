# Licences

The port's own code and scripts are free for anyone to use, change and share. The author does not claim ownership of
this work and does not attach a licence that restricts it. Nobody is asked for credit or permission.

That only covers what the author wrote. Everything below belongs to its owners and stays under their terms.

## What this repository contains

| Path | Is | Terms |
| --- | --- | --- |
| `ps5/glue/`, `tools/`, `docker/`, `build.sh`, `ps5/patches/sdl2/` | Original code of this port; parts of the build scripts follow the other PS5 ports using the same stack | Free to use; no restrictions from the author |
| `ps5/engine/NVulkanDrv/`, `ps5/engine/ut99-ps5.patch` | A render device and the changes to the UT99-Android / ut99dc source. They are built against Unreal Engine 1 and the patch quotes a few lines of its source | The author's changes are free; the engine code around them is Epic's |
| `art/`, `ps5/title/`, the icon and pictures in a built title | Home-screen art showing Unreal Tournament's logo and artwork | Epic's artwork; kept as the title's artwork |

## What is not here, and where it comes from

| Part | Project | Licence |
| --- | --- | --- |
| The engine (Unreal Engine 1 as in Unreal Tournament 436 / v400) | Epic Games, through the ut99dc source and UT99-Android | Epic's. The source is not part of this repository |
| `UMenu.u`, `UTMenu.u` (patched menu packages) | UT99-Android, `app/src/main/assets/ut99_patches` | Epic's scripts, compiled by that project; shipped in the release zip |
| The Vulkan driver | RADV from [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), linked by [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) | MIT (Mesa); the stack's tools GPL-3.0 |
| The platform layer and SDK | [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK) | GPL-3.0 |
| SDL2 2.28.5 | libsdl.org | zlib |
| Game data (`System`, `Maps`, `Textures`, `Sounds`, `Music`) | Epic Games, who released Unreal Tournament for free | Never part of this repository or a release; each player adds their own copy |

## The release zip

The release zip holds a built `eboot.bin` so that players do not have to build anything. It contains the compiled
Unreal Engine 1 code (Epic's) and links the GPL-3.0 platform layer, whose source is the PS5_PayloadSDK project linked
above. The zip contains no game data. If Epic or another rights holder objects to it, the release is withdrawn.
