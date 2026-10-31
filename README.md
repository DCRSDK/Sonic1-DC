# **Sonic the Hedgehog (2013) for SEGA Dreamcast**
A Sonic 1/RSDKv4 port for SEGA Dreamcast based on the RSDKv4 Decompilation, using the mobile version as base.

![Alt Text](/header.png)

+ Requires ffmpeg for asset generation, as well as an existing KallistiOS and mkdcdisc install for compiling the .elf and building the CDI.
+ Compatible with GCC 14.4.1 / KOS 2.3.0 or 2.2.0. Build with anything else at your own risk.
+ Build in Linux or MacOS environments, or Windows via DreamSDK or WSL (Debian or Ubuntu based preferably)
+ Build from your own KallistiOS setup by cloning this git and following the instructions in BUILD_DREAMCAST.md or use the autobuilder [HERE](https://colab.research.google.com/drive/1Fyjuh7ztAhe9oJs2oCw01hN1d9cTL54U) 
+ Bring your own Sonic 1 Data.rsdk (MUST be the 2013 mobile remaster; Origins is untested and presumed unsupported).

NO ASSETS ARE PROVIDED IN THIS RELEASE.

## Asset generation

Music and global sound effects have to be converted before they will play — the
Dreamcast build links no Vorbis decoder, so nothing on the disc may still be a
container. Everything else streams out of Data.rsdk untouched.

```sh
cd dreamcast
./generate_assets.sh /path/to/Sonic1Data [stage_dir]
```

`stage_dir` defaults to `../build-dc`. See `dreamcast/README.md` for the format
contract and the verifier — a headerless file carries no sample rate, so a
wrong conversion is a silent pitch shift rather than an error.

## Running it

**From a disc or GDEMU** — the default build. Package with mkdcdisc as
described in BUILD_DREAMCAST.md.

**Over dcload-ip** — configure with `-DDC_DATA_ROOT=/pc/` in its own build
directory and serve the `build-dc` contents as `/pc/`. The port detects this at
build time and reads music in small chunks, because every dcload call runs with
interrupts disabled and a large read stalls the whole machine. You can also
boot over dcload while reading `/cd/` from a disc in the drive, which keeps the
console output without putting game data on the link.

**What Works:**
+ Sonic 1 full game including Special Stages
+ The 2013 remaster menus, Time Attack and the ending sequence
+ Saves and achievements/leaderboard data on VMU, including a custom icon in the Dreamcast BIOS save menu
+ Music and sound effects streamed as raw PCM (no Vorbis decoding on-console)

**What doesn't work:**
+ 2P VS and anything else needing networking — disabled
+ Mod loader — disabled
+ Leaderboard uploads (local scores are kept, there is nowhere to send them)
+ The end credits music runs out before the sequence finishes. The credits are frame-timed while the music is wall-clock, so eighteen stage loads stretch a 128s sequence to around 156s against a 121s track. Ending.ogg is chained onto Credits.ogg to cover the final demo; the last screens still play out quiet.
+ Anything else I may have missed or overlooked in testing (you tell me)

Dev menu is enabled by default and is accessed by pressing the Y button. When selecting stages in the Dev menu press Start button for regular play, or the A button for debug mode. You can turn off Dev menu by toggling it off in the settings.ini and including it at the root of the disc when you go to build your CDI.

**Special Thanks**
+ Rubberduckycooly & everyone involved with the RSDKv4 decompilation - without that none of this would have been possible
+ SF94 & everyone involved with the Sonic Mania Dreamcast port - this port was largely inspired by it, you guys rock!
+ Everyone who helped me behind the scenes - you're all the best!

**Notice regarding AI use:**

This was almost entirely created using Claude AI. I do not consider myself a coder, more a project director. If you have any views regarding AI use that would otherwise prevent you from trying this port out, that's entirely understandable, enjoy your life as you see fit.

Sonic the Hedgehog (C) SEGA

Original RSDK author: Christian "Taxman" Whitehead

Decompilation authors: Rubberduckycooly and st×tic
