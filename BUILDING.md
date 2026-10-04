# Building ImMidi

This file says how to build ImMidi from its sources on Windows, Linux and macOS. [README.md](README.md) says how to use it.

A build has the same two parts on every system. First, premake5 turns `premake5.lua` into project files for your system: a Visual Studio solution on Windows, makefiles on Linux and macOS. Then Visual Studio or make builds from those. Run every command here in the ImMidi folder, the one that holds `premake5.lua`.

The sources carry every library ImMidi uses in `third_party/`: Dear ImGui, GLFW 3.4, RtMidi and stb, and for Linux the Vulkan headers and the volk loader. A build downloads nothing.

## What gets built

| | Windows, in `bin\windows-Release` | Linux, in `bin/linux-Release` | macOS, in `bin/macosx-Release` |
|---|---|---|---|
| The player | `ImMidi.exe` | `ImMidi` | `ImMidi.app`, made from the `ImMidi` program beside it |
| ImMidi Bridge, the emulation for other programs | `ImMidiBridge.exe` | `ImMidiBridge` | `ImMidi Bridge.app`, made from the `ImMidiBridge` program beside it |
| ImMidi Conversion Editor, for the conversion tables | `ImMidiConvEditor.exe` | `ImMidiConvEditor` | `ImMidi Conversion Editor.app`, made from the `ImMidiConvEditor` program beside it |
| The command line tool | `immidi_cli.exe` | `immidi_cli` | `immidi_cli` |
| Instrument definitions and conversion tables | `insdef\` and `conversion\` | `insdef/` and `conversion/` | inside the apps (ImMidi Bridge.app holds only `conversion`), and beside the programs |

Debug builds go in `bin\windows-Debug`, `bin/linux-Debug` and `bin/macosx-Debug`.

Release builds carry no debug information in their programs. Visual Studio writes it to `.pdb` files beside them, which they don't need to run. On Linux and macOS the build leaves it out and strips the programs of their symbols. Debug builds keep everything for a debugger.

## Windows

### Prerequisites

- Windows 10 or 11, 64-bit.
- Visual Studio 2022 with the **Desktop development with C++** workload. The free Community edition works. It brings the compiler and the Windows SDK, which has Direct3D 11, WinMM and everything else ImMidi links with.
- premake5, version 5.0.0-beta8 or newer. Download `premake-5.0.0-beta8-windows.zip` from [premake's releases](https://github.com/premake/premake-core/releases/tag/v5.0.0-beta8) and put `premake5.exe` into a folder on the PATH, or into the ImMidi folder.

### 1. Generate the project files

In a Command Prompt in the ImMidi folder:

```bat
premake5 vs2022
```

It writes `build\vs2022\ImMidi.sln` and its projects. PowerShell doesn't run programs from the current folder by name, so there type `.\premake5 vs2022` if `premake5.exe` sits in the ImMidi folder.

### 2. Build

1. Open `build\vs2022\ImMidi.sln` in Visual Studio 2022.
2. Choose **Release** and **x64** in the toolbar.
3. **Build > Build Solution**, or Ctrl+Shift+B, builds ImMidi, ImMidi Bridge, ImMidi Conversion Editor and immidi_cli.

**Debug > Start Without Debugging**, or Ctrl+F5, starts ImMidi. A Developer Command Prompt builds without opening Visual Studio:

```bat
msbuild build\vs2022\ImMidi.sln /p:Configuration=Release /p:Platform=x64 /m
```

The build copies `insdef` and `conversion` next to `ImMidi.exe`. The programs link the C runtime statically, so `ImMidi.exe` runs on another PC with Windows 10 or 11 without installing anything, as long as those two folders go with it (`ImMidiBridge.exe` needs only `conversion`; `ImMidiConvEditor.exe` both). When the window opens, `d3dcompiler_47.dll`, which is part of Windows, compiles its shaders.

## Linux

### Prerequisites

Any current distribution works, on a 64-bit PC or an ARM computer such as a Raspberry Pi 4 or 5. The build needs GCC 9 or newer, or Clang, and these packages:

| What for | Debian, Ubuntu, Raspberry Pi OS | Fedora | RHEL, AlmaLinux, Rocky Linux | Arch Linux, Manjaro |
|---|---|---|---|---|
| Compiler and make | `build-essential` | `gcc-c++ make` | `gcc-c++ make` | `base-devel` |
| premake5 | none, see [below](#premake5-where-the-distribution-has-none) | `premake` | none, see [below](#premake5-where-the-distribution-has-none) | `premake` |
| MIDI through ALSA | `libasound2-dev` | `alsa-lib-devel` | `alsa-lib-devel` | `alsa-lib` |
| The window on X11 | `libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev` | `libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel` | the same as Fedora | `libx11 libxrandr libxinerama libxcursor libxi libxext` |
| The window on Wayland | `libwayland-dev libxkbcommon-dev` | `wayland-devel libxkbcommon-devel` | `wayland-devel libxkbcommon-devel` | `wayland libxkbcommon` |
| OpenGL, the fallback renderer | `libgl-dev` | `libglvnd-devel` | `libglvnd-devel` | `libglvnd` |

Vulkan, the renderer ImMidi uses on Linux, needs no package to build. Its headers and the volk loader are in `third_party/`, and ImMidi opens the system's Vulkan loader when it starts.

The Wayland support needs libwayland 1.20 or newer, which Debian 12, Ubuntu 22.04, RHEL 9 and later versions have. On an older system, or to leave Wayland out, generate the makefiles with `premake5 gmake --no-wayland` and skip the Wayland row. ImMidi then runs on X11 only.

#### Debian, Ubuntu and Raspberry Pi OS

Debian 12 or later, Raspberry Pi OS based on it, or Ubuntu 22.04 or later:

```sh
sudo apt install build-essential libasound2-dev libgl-dev \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev \
    libwayland-dev libxkbcommon-dev
```

These distributions have no premake5. Debian's `premake4` can't read `premake5.lua`. See [premake5 where the distribution has none](#premake5-where-the-distribution-has-none).

#### Fedora

```sh
sudo dnf install gcc-c++ make premake alsa-lib-devel libglvnd-devel \
    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel \
    wayland-devel libxkbcommon-devel
```

The `premake` package installs `premake5`. Check that `premake5 --version` shows 5.0.0-beta8 or newer, and use the download [below](#premake5-where-the-distribution-has-none) if it shows an older one.

#### RHEL, AlmaLinux, Rocky Linux and CentOS Stream

Version 9 or later. These distributions keep many development packages in the CRB repository, called CodeReady Linux Builder on RHEL, which is off until you turn it on:

```sh
sudo dnf config-manager --set-enabled crb                                               # AlmaLinux, Rocky Linux, CentOS Stream
sudo subscription-manager repos --enable codeready-builder-for-rhel-9-$(arch)-rpms      # RHEL 9 itself; use rhel-10 on RHEL 10
```

Then:

```sh
sudo dnf install gcc-c++ make alsa-lib-devel libglvnd-devel \
    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel libXext-devel \
    wayland-devel libxkbcommon-devel
```

They have no premake5, EPEL included. See [premake5 where the distribution has none](#premake5-where-the-distribution-has-none).

#### Arch Linux, Manjaro and EndeavourOS

```sh
sudo pacman -S --needed base-devel premake alsa-lib libglvnd \
    libx11 libxrandr libxinerama libxcursor libxi libxext wayland libxkbcommon
```

Arch's packages include their development files. Check that `premake5 --version` shows 5.0.0-beta8 or newer.

#### premake5 where the distribution has none

The commands below install premake5 5.0.0-beta8 into `~/.local/bin`. Most distributions add that folder to the PATH once it exists, so log in again, or type `~/.local/bin/premake5` until then.

- On a 64-bit Intel or AMD PC, take premake's ready-made one:

  ```sh
  curl -LO https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-linux.tar.gz
  tar -xzf premake-5.0.0-beta8-linux.tar.gz
  chmod +x premake5 && mkdir -p ~/.local/bin && mv premake5 ~/.local/bin/
  ```

- On a Raspberry Pi or another ARM computer, premake has no ready-made one, so build it from its sources. That takes about a minute on a Raspberry Pi 5 and needs the UUID library's development files:

  ```sh
  sudo apt install uuid-dev          # Debian, Ubuntu, Raspberry Pi OS
  sudo dnf install libuuid-devel     # Fedora, RHEL, AlmaLinux, Rocky Linux
  curl -L https://github.com/premake/premake-core/archive/refs/tags/v5.0.0-beta8.tar.gz | tar -xz
  make -C premake-core-5.0.0-beta8 -f Bootstrap.mak linux
  mkdir -p ~/.local/bin && cp premake-core-5.0.0-beta8/bin/release/premake5 ~/.local/bin/
  ```

- With Homebrew on Linux, `brew install premake` works on both.

#### What ImMidi needs when it runs

- Something to play the MIDI: a sound module through a USB MIDI interface, or a software synthesizer on the ALSA sequencer such as FluidSynth.
- For Vulkan, the Vulkan loader and a driver for the graphics chip. Desktop installations usually have both. On Debian and Ubuntu they are `libvulkan1` and `mesa-vulkan-drivers`, on Fedora and RHEL `vulkan-loader` and `mesa-vulkan-drivers`, and on Arch `vulkan-icd-loader` with the driver for your chip, such as `vulkan-radeon`, `vulkan-intel` or NVIDIA's `nvidia-utils`. Without them ImMidi draws with OpenGL and prints why.
- For the system's file dialogs, `zenity` or `kdialog`. Without either, ImMidi uses its own file browser.
- For Japanese song titles and lyrics, a CJK font. [README.md](README.md#japanese-and-other-cjk-text) lists the fonts it finds.

### 1. Generate the makefiles

```sh
premake5 gmake
```

It writes the makefiles in `build/gmake`. Add `--cc=clang` to build with Clang, and `--no-wayland` for an X11-only build.

### 2. Build

```sh
make -C build/gmake config=release -j$(nproc)
```

This builds ImMidi, ImMidiBridge, ImMidiConvEditor and immidi_cli into `bin/linux-Release` and copies `insdef` and `conversion` next to them. `-j$(nproc)` compiles on every core. `make -C build/gmake config=release ImMidi -j$(nproc)` builds only the player, and `config=debug` makes a Debug build in `bin/linux-Debug`. The Release link strips the programs with `-s`, so `file bin/linux-Release/ImMidi` shows it as "stripped".

Start it with:

```sh
./bin/linux-Release/ImMidi
```

### 3. Add ImMidi to the application menu

This step is optional:

```sh
sh tools/linux-desktop.sh install
```

The script installs a desktop entry and the icon for the current user, in `~/.local/share/applications` and `~/.local/share/icons`, and the same for ImMidi Bridge and ImMidi Conversion Editor when `ImMidiBridge` and `ImMidiConvEditor` are beside the player. The entries start the programs in `bin/linux-Release` by their full paths, so run the script again after moving the programs, or name another player with `sh tools/linux-desktop.sh install /path/to/ImMidi`. `sh tools/linux-desktop.sh uninstall` removes the files again.

The entries also give the windows their icons on Wayland desktops that can't take them from the windows, such as GNOME and KDE Plasma before 6.3. They find the entries by the windows' app IDs, `org.immidi.ImMidi`, `org.immidi.ImMidiBridge` and `org.immidi.ImMidiConvEditor`. On X11 and on KDE Plasma 6.3 or later, the windows show their icons without the entries.

## macOS

### Prerequisites

macOS 10.15 or later, on Apple silicon or an Intel Mac. The build makes programs for the kind of Mac it runs on.

1. Apple's command line tools, for the compiler, make, `strip` and `codesign`. Xcode brings them. Without Xcode:

   ```sh
   xcode-select --install
   ```

2. premake5, version 5.0.0-beta8 or newer. `premake5 --version` shows the version.
   - With [Homebrew](https://brew.sh):

     ```sh
     brew install premake
     ```

   - Or premake's ready-made one, `premake-5.0.0-beta8-macosx.tar.gz` for Apple silicon, `premake-5.0.0-beta8-macosx-x64.tar.gz` for an Intel Mac:

     ```sh
     curl -LO https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-macosx.tar.gz
     tar -xzf premake-5.0.0-beta8-macosx.tar.gz
     chmod +x premake5 && sudo mkdir -p /usr/local/bin && sudo mv premake5 /usr/local/bin/
     ```

     A copy downloaded with a web browser carries macOS's download mark, and macOS won't run it until `xattr -d com.apple.quarantine premake5` removes the mark.

Nothing else is needed. CoreMIDI, Metal and the rest of what ImMidi uses come with macOS.

### 1. Generate the makefiles

```sh
premake5 gmake
```

It writes the makefiles in `build/gmake`.

### 2. Build

```sh
make -C build/gmake config=release -j$(sysctl -n hw.ncpu)
```

`-j$(sysctl -n hw.ncpu)` compiles on every core, and `config=debug` makes a Debug build in `bin/macosx-Debug`. After linking a Release build, `tools/macos-app.sh` strips each program with Apple's `strip` and signs it again ad hoc, as the change voids the signature the linker gave it. Every build then assembles `ImMidi.app`: the Info.plist from `packaging/macos/Info.plist.in`, the icon, `insdef` and `conversion` in `Contents/Resources`, and an ad-hoc signature over the whole app. `ImMidi Bridge.app` is made the same way, from `packaging/macos/ImMidiBridge.plist.in`, with `conversion` only, and `ImMidi Conversion Editor.app` from `packaging/macos/ImMidiConvEditor.plist.in`, with both folders.

Start it with:

```sh
open bin/macosx-Release/ImMidi.app
```

To install it, drag `ImMidi.app` (and `ImMidi Bridge.app` and `ImMidi Conversion Editor.app`) to Applications.

## After updating the sources

- Run premake5 again, step 1 for your system, as a newer version may add or remove source files.
- make rebuilds what changed in the sources, not what changed in how they are built. After an update that changes that, such as Release builds becoming stripped, or after switching `--no-wayland` on or off, clean first with `make -C build/gmake config=release clean`, then build again. A plain `make -C build/gmake clean` cleans the Debug build. In Visual Studio, **Build > Rebuild Solution** does the same.

## Checking a build

`immidi_cli selftest` drives the player without a MIDI device and checks the results: pedals, overlapping notes, effect mirroring, playlists, file formats and more. Run it in the ImMidi folder, as it plays the songs in `example-midis`:

```sh
./bin/linux-Release/immidi_cli selftest       # Linux
./bin/macosx-Release/immidi_cli selftest      # macOS
bin\windows-Release\immidi_cli.exe selftest   # Windows
```

It prints `ok` or `FAIL` for each check and ends with `PASSED (0 failures)`. Its exit code is 0 when everything passes.

## The project's files

The project files come from `premake5.lua`, and the sources don't include them. After adding or removing source files, or changing `premake5.lua`, run premake5 again.

| Folder or file | Contents |
|---|---|
| `premake5.lua` | What premake5 makes the project files from: the programs, their sources and their settings on each system. The version number is at its top. |
| `src/core/` | The player without its window: MIDI files and the other song formats, playback, the synth state, standards and emulation, the playlist, settings |
| `src/ui/` | The window: Dear ImGui views, the renderers for Direct3D 11, Metal, Vulkan and OpenGL, file dialogs, drag and drop. `src/main.cpp` starts it all. |
| `src/bridge/` | ImMidi Bridge's window. It uses the renderers, fonts and widgets of `src/ui/`, and the conversion itself is in `src/core/` (`MidiBridge`), shared with the player. |
| `src/editor/` | ImMidi Conversion Editor's window, with the renderers, fonts and widgets of `src/ui/`. The tables are read and written by `src/core/` (`ConversionDoc` keeps their layout; `ConversionTables` checks them as ImMidi loads them). |
| `third_party/` | Dear ImGui, GLFW 3.4 with its pre-generated Wayland protocol headers, RtMidi, stb, the Vulkan headers and volk |
| `insdef/`, `conversion/` | Instrument definitions and the instrument conversion tables, copied next to the programs by every build |
| `reference/` | Material that is neither built nor shipped: SMF Knife's tables, which the conversion tables were made from, and openmidiproject's MU1000/MU2000 instrument definition, which `tools/gen_ins.py` and `tools/gen_ins_tmidi.py` take the XG drum kits' note names from |
| `packaging/` | The Windows resource files and icons, the Mac apps' Info.plist templates and icons, and the Linux desktop entries and icons, for ImMidi, ImMidi Bridge and ImMidi Conversion Editor |
| `tools/` | `immidi_cli.cpp` is the command line tool, `macos-app.sh` the Mac's strip and bundle steps after linking, and `linux-desktop.sh` adds ImMidi, ImMidi Bridge and ImMidi Conversion Editor to a Linux desktop's application menu. The Python scripts generated tables and icons that are in the sources; the builds only use what they made. |
| `example-midis/` | Songs for trying ImMidi out, which `immidi_cli selftest` plays |
