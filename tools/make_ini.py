#!/usr/bin/env python3
"""Writes the UT99 ini for a port: usage make_ini.py <DCUtil.ini> <game_dir> <out.ini> [renderer]
Paths point at the (read-only) game folder; save/cache stay relative to the writable working directory."""
import re, sys
src, game, out = sys.argv[1:4]
renderer = sys.argv[4] if len(sys.argv) > 4 else "NVulkanDrv.NVulkanRenderDevice"
s = open(src, newline="").read().replace("\r\n", "\n")
s = re.sub(r"^(GameRenderDevice|WindowedRenderDevice|RenderDevice)=.*$", lambda m: f"{m.group(1)}={renderer}", s, flags=re.M)
s = re.sub(r"^AudioDevice=.*$", "AudioDevice=Audio.GenericAudioSubsystem", s, flags=re.M)
for ext, folder in (("u", "System"), ("unr", "Maps"), ("utx", "Textures"), ("uax", "Sounds"), ("umx", "Music")):
    s = s.replace(f"Paths=../{folder}/*.{ext}", f"Paths={game}/{folder}/*.{ext}")
# The menu's look and feel and UI scale live in the ini (the Android app seeds them the same way); without them the
# UWindow root window gets an empty class name and every menu item draws as "Accessed None".
s = s.rstrip("\n") + """

[UWindow.UWindowRootWindow]
GUIScale=2.000000
LookAndFeelClass=UMenu.UMenuBlueLookAndFeel

[UMenu.UMenuRootWindow]
GUIScale=2.000000
LookAndFeelClass=UMenu.UMenuBlueLookAndFeel

[Engine.PlayerPawn]
DefaultFOV=95.000000
DesiredFOV=95.000000

[Audio.GenericAudioSubsystem]
UseFilter=True
UseSurround=False
UseStereo=True
UseCDMusic=False
UseDigitalMusic=True
UseSpatial=False
UseReverb=False
Use3dHardware=False
LowSoundQuality=False
ReverseStereo=False
Latency=40
OutputRate=44100Hz
Channels=32
MusicVolume=192
SoundVolume=224
AmbientFactor=0.700000
"""
open(out, "w").write(s)
print("wrote", out)
