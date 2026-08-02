# MKWii Launcher — a Dolphin fork

A fork of [Dolphin](https://dolphin-emu.org/) rebuilt as a single-purpose
launcher for playing **Mario Kart Wii (CTGP Revolution, `RMCE01`) over a LAN,
with proximity voice chat**.

It is not a general-purpose emulator any more. The game list is gone, most of
the toolbar is gone, and booting anything other than the one game it is set up
for is not something it tries to do well. Everything below is what was added or
changed; upstream Dolphin's own README follows after it.

Licensed under the GPLv2+, same as upstream. Not affiliated with the Dolphin
project — please do not report problems with this build to them.

## What is different from upstream Dolphin

### A private network for the emulated console

The largest change. Dolphin does not emulate a Wii NIC: IOS's socket API is
normally a thin shim over host sockets, so the console inherits every adapter
the PC has, talks on whichever one the OS picks, and reports an address that may
belong to none of them.

This replaces the shim. While a lobby is up, a socket the console opens is
backed by nothing on the host at all — no descriptor, no bind, no packet leaving
through a real adapter. Datagrams and streams are carried between Dolphin
instances over the lobby's own link, and the console is told it lives on
`10.13.37.0/24` with an address the lobby assigned it.

What the console can do on that subnet is deliberately not limited to what Mario
Kart happens to need: UDP, TCP, broadcast, ICMP echo and unrecognised protocols
are all carried. Everything is traced to `User\Logs\VirtualNet.log`,
unconditionally — a fault that only shows up with two consoles talking to each
other is not reproducible on demand, so the trace has to already be there when
it happens.

Lobbies are a star: one instance hosts, the rest connect to it, and the host
forwards. A dropped client can reconnect and keep its address, so the console
carries on rather than having to be restarted.

*`Source/Core/Core/Lobby/`, `Source/Core/Core/IOS/Network/`*

### Proximity voice chat

Everyone in a lobby hears everyone else. In menus that is a flat voice channel
with a per-person volume; in an online race it becomes positional.

No positions are ever sent. Every instance can already read every kart's
position and state out of its own copy of the game, so the only thing
transmitted is identity — each instance reports its active licence's Mii name,
which is matched against the racer names read out of memory. From then on a
lobby member and a kart on the track are known to be the same person, and the
listener decides everything else from what is happening on its own screen.

* **Distance** attenuates, out to a configurable range.
* **Doppler** follows the rate of change of the distance between the two karts,
  so it is relative by construction: driving alongside someone at the same speed
  shifts nothing, however fast you are both going.
* **State changes the voice.** Mega and Bullet Bill go deep, being shocked or
  squashed goes high, a star adds echo.
* Proximity applies only while both people are in an online race and neither has
  finished. Crossing the line puts a racer back on the flat channel.

Audio is Opus over the lobby's own link on a dedicated ENet channel, sent
unreliable and unsequenced on purpose — a retransmitted 20 ms frame arrives long
after the moment it belonged to. The **Voice Chat** button on the toolbar opens
mute, deafen, device pickers, microphone gain, master volume, a noise gate,
Doppler intensity, hearing range, bitrate, and a row per person with a level
meter painted inside their volume slider. Only the host's bitrate applies; it is
pushed to everyone.

*`Source/Core/Core/Lobby/Voice/`, `Source/Core/DolphinQt/Lobby/VoiceChatWindow.*`*

### A memory inspector

A reverse-engineered map of Mario Kart Wii's memory, read from inside the
emulator on the CPU thread — scene and race state, and per racer the full
transform, live race position, identity, item slot and status flags. Proximity
voice chat is built on it.

It can also serve a live dashboard over HTTP on localhost — a top-down track
map, standings, raw watch cards, and item and state injection — which is
**compile-time optional** and off by default:

```sh
cmake -B build -DMKW_MEMORY_INSPECTOR_WEB=ON
```

With it off, the server and the embedded page are not built at all.

The addresses were derived empirically from one build. CTGP Revolution ships a
patched DOL, so published stock-MKWii addresses do not necessarily apply and
vice versa.

*`Source/Core/Core/MemInspect/`*

### First-run setup and a lobby home screen

A wizard on first launch collects a nickname and microphone, installs the Wii
system software and the LAN Play Module, and takes a disc image — accepting only
`RMCE01`, because that is what the module patches. In place of the game list,
the main window shows the lobby settings, and Play boots through Brainslug so
the module is applied.

*`Source/Core/DolphinQt/Setup/`, `Source/Core/DolphinQt/Lobby/`*

### Branding, isolation and defaults

* **Unconditionally portable.** The user directory is always `<exe dir>\User`.
  `-u`, `portable.txt`, the registry keys, Documents and AppData are all
  ignored, so this build cannot read or corrupt the config of a stock Dolphin
  install on the same machine.
* **Analytics are compiled out**, not merely disabled — `ENABLE_ANALYTICS` is a
  plain `set(OFF)`, so `-DENABLE_ANALYTICS=ON` cannot bring them back.
* **Different graphics defaults**, chosen for the one game this plays: auto
  internal resolution, 2x MSAA, the widescreen hack, and shaders compiled before
  starting with drawing skipped rather than stuttering.

## Building

Standard Dolphin build, plus the submodules — this fork vendors
[Opus](https://opus-codec.org/) for voice chat:

```sh
git submodule update --init --recursive
```

Then follow upstream's instructions for your platform, linked below. Developed
and tested on Windows with MSVC and Ninja; the other platforms are inherited
from upstream and have not been tried.

## Status

Verified live on two machines: both consoles see each other's rooms and can join
in either direction with no host adapter involved at any point, and voice chat
works in the menus and positionally during a race.

Not verified: more than two consoles in one lobby, reconnecting mid-race rather
than in the menus, and more than two people talking at once.

---

# Dolphin - A GameCube and Wii Emulator

*Upstream's README follows, unchanged.*

[Homepage](https://dolphin-emu.org/) | [Project Site](https://github.com/dolphin-emu/dolphin) | [Buildbot](https://dolphin.ci/) | [Forums](https://forums.dolphin-emu.org/) | [Wiki](https://wiki.dolphin-emu.org/) | [GitHub Wiki](https://github.com/dolphin-emu/dolphin/wiki) | [Issue Tracker](https://bugs.dolphin-emu.org/projects/emulator/issues) | [Coding Style](https://github.com/dolphin-emu/dolphin/blob/master/Contributing.md) | [Transifex Page](https://app.transifex.com/dolphinemu/dolphin-emu/dashboard/) | [Analytics](https://mon.dolphin-emu.org/)

Dolphin is an emulator for running GameCube and Wii games on Windows,
Linux, macOS, and recent Android devices. It's licensed under the terms
of the GNU General Public License, version 2 or later (GPLv2+).

Please read the [FAQ](https://dolphin-emu.org/docs/faq/) before using Dolphin.

## System Requirements

### Desktop

* OS
    * Windows (10 1903 or higher).
    * Linux.
    * macOS (11.0 Big Sur or higher).
    * Unix-like systems other than Linux are not officially supported but might work.
* Processor
    * A CPU with SSE2 support.
    * A modern CPU (3 GHz and Dual Core, not older than 2008) is highly recommended.
* Graphics
    * A reasonably modern graphics card (Direct3D 11.1 / OpenGL 3.3).
    * A graphics card that supports Direct3D 11.1 / OpenGL 4.4 is recommended.

### Android

* OS
    * Android (7.0 Nougat or higher).
* Processor
    * A processor with support for 64-bit applications (either ARMv8 or x86-64).
* Graphics
    * A graphics processor that supports OpenGL ES 3.0 or higher. Performance varies heavily with [driver quality](https://dolphin-emu.org/blog/2013/09/26/dolphin-emulator-and-opengl-drivers-hall-fameshame/).
    * A graphics processor that supports standard desktop OpenGL features is recommended for best performance.

Dolphin can only be installed on devices that satisfy the above requirements. Attempting to install on an unsupported device will fail and display an error message.

## Building

You may find building instructions on the appropriate wiki page for your operating system:

* [Windows](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Windows)
* [Linux](https://github.com/dolphin-emu/dolphin/wiki/Building-for-Linux)
* [macOS](https://github.com/dolphin-emu/dolphin/wiki/Building-for-macOS)
* [Android](#android-specific-instructions) <!-- TODO: Create a "Building for Android" wiki page and link it here -->
* [OpenBSD](https://github.com/dolphin-emu/dolphin/wiki/Building-for-OpenBSD) (unsupported)

Before building, make sure to pull all submodules:

```sh
git submodule update --init --recursive
```

### Android-specific instructions

These instructions assume familiarity with Android development. If you do not have an
Android dev environment set up, see [AndroidSetup.md](AndroidSetup.md).

If using Android Studio, import the Gradle project located in `./Source/Android`.

Android apps are compiled using a build system called Gradle. Dolphin's native component,
however, is compiled using CMake. The Gradle script will attempt to run a CMake build
automatically while building the Java code.

## Uninstalling

On Windows, simply remove the extracted directory, unless it was installed with the NSIS installer,
in which case you can uninstall Dolphin like any other Windows application.

Linux users can run `cat install_manifest.txt | xargs -d '\n' rm` as root from the build directory
to uninstall Dolphin from their system.

macOS users can simply delete Dolphin.app to uninstall it.

Additionally, you'll want to remove the global user directory if you don't plan on reinstalling Dolphin.

## Command Line Usage

```
Usage: Dolphin.exe [options]... [FILE]...

Options:
  --version             show program's version number and exit
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path
  -m MOVIE, --movie=MOVIE
                        Play a movie file
  -e <file>, --exec=<file>
                        Load the specified file
  -n <16-character ASCII title ID>, --nand_title=<16-character ASCII title ID>
                        Launch a NAND title
  -C <System>.<Section>.<Key>=<Value>, --config=<System>.<Section>.<Key>=<Value>
                        Set a configuration option
  -s <file>, --save_state=<file>
                        Load the initial save state
  -d, --debugger        Show the debugger pane and additional View menu options
  -l, --logger          Open the logger
  -b, --batch           Run Dolphin without the user interface (Requires
                        --exec or --nand-title)
  -c, --confirm         Set Confirm on Stop
  -v VIDEO_BACKEND, --video_backend=VIDEO_BACKEND
                        Specify a video backend
  -a AUDIO_EMULATION, --audio_emulation=AUDIO_EMULATION
                        Choose audio emulation from [HLE|LLE]
```

Available DSP emulation engines are HLE (High Level Emulation) and
LLE (Low Level Emulation). HLE is faster but less accurate whereas
LLE is slower but close to perfect. Note that LLE has two submodes (Interpreter and Recompiler)
but they cannot be selected from the command line.

Available video backends are "D3D" and "D3D12" (they are only available on Windows), "OGL", and "Vulkan".
There's also "Null", which will not render anything, and
"Software Renderer", which uses the CPU for rendering and
is intended for debugging purposes only.

## DolphinTool Usage
```
usage: dolphin-tool COMMAND -h

commands supported: [convert, verify, header, extract]
```

```
Usage: convert [options]... [FILE]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FILE, --output=FILE
                        Path to the destination FILE.
  -f FORMAT, --format=FORMAT
                        Container format to use. Default is RVZ. [iso|gcz|wia|rvz]
  -s, --scrub           Scrub junk data as part of conversion.
  -b BLOCK_SIZE, --block_size=BLOCK_SIZE
                        Block size for GCZ/WIA/RVZ formats, as an integer.
                        Suggested value for RVZ: 131072 (128 KiB)
  -c COMPRESSION, --compression=COMPRESSION
                        Compression method to use when converting to WIA/RVZ.
                        Suggested value for RVZ: zstd [none|zstd|bzip|lzma|lzma2]
  -l COMPRESSION_LEVEL, --compression_level=COMPRESSION_LEVEL
                        Level of compression for the selected method. Ignored
                        if 'none'. Suggested value for zstd: 5
```

```
Usage: verify [options]...

Options:
  -h, --help            show this help message and exit
  -u USER, --user=USER  User folder path, required for temporary processing
                        files.Will be automatically created if this option is
                        not set.
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -a ALGORITHM, --algorithm=ALGORITHM
                        Optional. Compute and print the digest using the
                        selected algorithm, then exit. [crc32|md5|sha1|rchash]
```

```
Usage: header [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -b, --block_size      Optional. Print the block size of GCZ/WIA/RVZ formats,
then exit.
  -c, --compression     Optional. Print the compression method of GCZ/WIA/RVZ
                        formats, then exit.
  -l, --compression_level
                        Optional. Print the level of compression for WIA/RVZ
                        formats, then exit.
```

```
Usage: extract [options]...

Options:
  -h, --help            show this help message and exit
  -i FILE, --input=FILE
                        Path to disc image FILE.
  -o FOLDER, --output=FOLDER
                        Path to the destination FOLDER.
  -p PARTITION, --partition=PARTITION
                        Which specific partition you want to extract.
  -s SINGLE, --single=SINGLE
                        Which specific file/directory you want to extract.
  -l, --list            List all files in volume/partition. Will print the
                        directory/file specified with --single if defined.
  -q, --quiet           Mute all messages except for errors.
  -g, --gameonly        Only extracts the DATA partition.
```
