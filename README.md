# ImMidi

A MIDI file player for Windows, Linux and macOS with a Dear ImGui interface. It drives external or software sound modules (GM, GM2, Roland GS and Yamaha XG) and shows all 16 channels at once, each with its own keyboard and controls. Its emulation of the standards also comes as a separate realtime tool for other MIDI programs, [ImMidi Bridge](#immidi-bridge).

## AI/LLM usage disclosure

This project has been made with the assistance of an AI/LLM.

## Features

Playback

- Play, pause, stop, previous/next track, seek bar with loop markers.
- Playlists: add files or whole folders (optionally with subfolders), select several tracks (click, Ctrl+click, Shift+click, Shift+arrows, Ctrl+A) to remove or move them together, drag to reorder with an insertion mark between the rows, sort by a column by clicking its header (click again for the reverse order; text sorts naturally, "Track 2" before "Track 10"), and import, append or export `.m3u`/`.m3u8` lists (exported with relative paths when possible). The playlist and current track are remembered between sessions.
- Song details in the playlist work like foobar2000's: the title, length and format of added files are read in the background (a light pass that keeps no notes, about 2 s for a 100 MB black MIDI file on a Raspberry Pi 5), saved with the playlist, and read again only when a file is opened. The Title column shows the song's title, or the file name when the song has none. Right-click the column headers to show the File name and Path columns (Path is shown in the Playlist tab). File info in a track's context menu (or Alt+Enter) shows the title, file name, folder, length, format, size and date, with the full path to select or copy.
- Files open in the background with a progress bar, so the window stays responsive even for very large files.
- Play modes: sequential, repeat all, repeat one, shuffle, single (stop after the track).
- Global tempo in percent (10 to 400 %) and global transpose in semitones. Transpose skips drum parts.
- Loop points: `loopStart`/`loopEnd` markers, Apogee EMIDI CC#116/117 (and 118/119), RPG Maker CC#111. The repeat count is configurable (or forever).
- EMIDI track designation (CC#110/111): tracks written only for other sound cards are left out.
- Seeking re-sends ("chases") programs, controllers, RPN/NRPN and SysEx up to the new position, so the device state matches the song.
- A reset message goes out before each song: automatic by target device, or GM On, GM2 On, GS Reset, SC-88 Mode Set + GS Reset, XG System On, or none. The wait after the reset is adjustable.

Channels view

- 16 channel rows per MIDI port with mute/solo, instrument, bank/program, level meter, volume, expression, pan, pitch bend, modulation, reverb, chorus, delay/variation, sustain/sostenuto/soft pedals, polyphony and a keyboard that shows the playing notes. Notes that the sustain or sostenuto pedal keeps sounding after their key was released stay on the keyboard in a greyed shade of the channel colour until the pedal is released (they count in the polyphony too).
- The line under the transport shows the song's format, resolution, key, standard and ports, and its polyphony: the notes sounding now on each port and in all.
- Every control works with the mouse. Drag or use the wheel to change a value, double-click to reset it. Clicking the keyboard plays notes on that channel. Hovering a drum key shows the drum instrument name.
- A value you change is locked (orange outline) and the song can no longer overwrite it. Right-click the control, or use the lock button, to follow the song again.
- Click the instrument name to pick an instrument or drum kit from the instrument definitions (`insdef/*.ins`, Cakewalk format). Search works across all banks.
- The detail panel under the channel list edits the selected part: attack, decay, release, vibrato rate/depth/delay, filter cutoff/resonance, HPF, part EQ (bass/treble gain and frequency), key shift, bend range, drum part mode and insertion EFX assignment.
- Drum parts are detected automatically: GS "use for rhythm part" SysEx (including the SC-88 `50 xx xx` addresses for the second part group), XG bank MSB 126/127 and part mode SysEx, GM2 bank MSB 120.
- XG and GM2 songs can play several drum kits at once; a Sound Canvas has two drum maps (one drum set each). With emulation, channel 10 uses drum map 1 and the other drum parts map 2, or map 1 when they play channel 10's set, so the same set does not take up the second map (a part that shares it moves to map 2 when channel 10 changes its set). GS messages in such songs that name a drum map follow the same rule; GS songs keep their own drum maps.
- Multi-port songs (for example 32-part SC-88Pro songs using ports A and B) show all ports stacked with compact rows and a scroll bar. Other view modes put ports side by side or in tabs.
- Mirror effects to every port (Device menu, or Settings > MIDI outputs; off by default), for SC-88/SC-88Pro 32-part songs played on one single-port synth per port, for example two Roland Sound Canvas VA instances. The SC-88 shares its effects (reverb, chorus, delay, EQ, insertion EFX) and master settings between its 32 parts, so songs usually send them once, on port A, and set the parts of port B with `50 xx xx` addresses. With mirroring on, those shared settings go to every port, and the `50 xx xx` messages go to port B as its own `40 xx xx` addresses. A real SC-88, SC-88Pro or SC-8850 does this itself; songs for the SC-88's double module mode, which has separate effects per port, are left as they are.
- The M and S buttons next to "Loop points" in the transport bar light up while any part is muted or soloed; clicking one unmutes or unsolos every part at once (also in the Playback menu).
- View > Mini player (Ctrl+Shift+M) shrinks the window to the transport and the song line. The channel view is not drawn meanwhile, which saves processor and graphics time. Choosing a tab, the Sound Canvas display or the virtual keyboard brings the full window back at its previous height.

Sound Edit tab

- A 16-part matrix of the part parameters above. Values go out as NRPN on GS and XG devices and as GM2 sound controllers (CC#71 to 78) on GM2 devices. Parameters the target device does not support are greyed out.

Effects tab

- GS: reverb and chorus macros with all parameters; delay and 2-band EQ on SC-88 and later; the SC-88Pro/SC-8850 insertion effect (EFX) with all 64 types, every parameter shown with its name and real value (ms, Hz, dB, ...), EFX sends and the parts that use it.
- XG: reverb, chorus and variation types, variation connection (insertion/system) and part, returns, pans and sends, raw effect parameters, insertion effects 1/2 (MU100 and later), Multi EQ.
- GM2: global reverb and chorus parameters.
- Master volume, key shift, pan and fine tune (0.1 cent steps: GS/XG MASTER TUNE, GM2 Master Fine Tuning).

Black MIDI

- Files with tens of millions of notes load in a streaming pass (about 2 s for 25 M events on a Raspberry Pi 5) and play without freezing the window. When the output device can't keep up, note-ons that are later than a set limit (default 50 ms) are dropped, so playback stays in time. The transport bar shows how many notes were dropped. An optional minimum velocity drops quiet notes as well. See Settings > Performance.

Emulation

- Emulation is off by default, so songs reach the module exactly as written. Turn it on in Settings > Sound module or the Device menu. The Sound Canvas tone map forcer works either way.
- Pick the sound module you own (GM, GM2, SC-55, SC-88, SC-88Pro, SC-8850, XG) and ImMidi converts songs written for another standard. Examples: XG on SC-88Pro or SC-8850, GS on XG, GM2 on GS. It maps bank selects and drum kits, drum part setup, part parameters, reverb/chorus/variation types to GS macros or EFX types (and back), GM2 sound controllers to NRPN, and reset messages. "Auto" instead assumes a module that understands the song's own standard.
- Instrument conversion tables (`conversion/*.json`): for GS songs on XG modules, XG songs on Sound Canvases, and SC-88Pro or SC-88 songs on older Sound Canvases, they give the closest voice and drum kit, a volume correction, drum note changes and controller scaling. A sound that the song's own map lacks comes from the next map that has it (songs for an SC-88 are often taken for SC-55 songs), and SC-55 songs on later Sound Canvases use the SC-55 map. Other songs play each module's own sounds: where SMF Knife picked an SC-55 map sound for the SC-88 (mostly the General MIDI sounds, such as Violin, Trumpet or Flute, and some drum sets), the SC-88 and later play their own version of it. XG songs on an SC-88Pro use the SC-88Pro map's drum sets and voices, including SC-88Pro sounds that come closer to the XG voice than SMF Knife's SC-88 choice (303 Sqr Bs, Jazz Organ, Brass sfz, Pulse Lead, Vox Lead, Tine Pad, ...). XG's leads are GS's waves (Saw Lead is Saw Wave, Square Lead Square Wave, Sine Lead Sine Wave, which the SC-88Pro and SC-8850 maps call 2600 Sine). On an SC-8850 they use the SC-8850 map: its versions of those sounds and drum sets, and its own sounds where they come closer, such as Hammer, EP Legend, Jazz Man, Dist Rtm GTR or the Bass, 6th and Euro hits (with a forced tone map, the sounds of that map). A sound can also get an attack time correction: XG's door squeak plays the Sound Canvas's Door Creaking with an attack 10 shorter, as it fades in there (the song's own attack changes keep the correction, and the next sound gets the song's attack back). Settings > Sound module can turn the tables off and shows what is in use; [ImMidi Conversion Editor](#immidi-conversion-editor) edits them.
- Drum notes follow each kit's own layout: a note keeps its key when the other kit has the same kind of sound there, and otherwise moves to the key with the closest sound. For example, the Symphony Kit's hand cymbals play the Orchestra set's concert cymbals (A3, B3) and its hi-hats the Orchestra set's hi-hats (D#1-F1), the Orchestra set's timpani play the Symphony Kit's toms, XG's Open Rim Shot plays the tight snare (E2, or D#7 on the Standard sets of the SC-88Pro and SC-8850 maps), the Symphony Kit's Gran Cassa (B1) plays the Orchestra set's Concert BD (C2), and the SFX kits' sounds find the same sound effects in the other standard's SFX set. Notes without a counterpart, and notes the song's kit has no sound for, are not played.
- The tables are plain JSON that can be checked and edited by hand: every entry names its sounds, program numbers (`pc`) are 1-128 as in the manuals and in ImMidi (the program change carries `pc` - 1), and `immidi_cli selftest` checks every name against the instrument definitions, so a number that is off by one shows up.
- Source module detection: the song's SysEx and bank selects decide which SC model a GS song needs (tone map, EFX, delay, two ports). A module named in the song's text (`SC-88Pro`, `ＳＣ－５５対応`, `MU80`, ...) refines the guess, as TMIDI Player does. It never goes below what the messages need, and for a song whose messages show nothing beyond General MIDI (no reset) the named module decides.
- Song made for (Device menu, the playlist's right-click menu for several songs, File Info): when a song's messages do not tell the module it is made for (an SC-8850 song without a GS reset is otherwise taken for a General MIDI song), choose it there. The emulation then converts from that module and the Auto device follows it; "As detected" goes back to the analysis. ImMidi remembers the choice per file in `song-modules.txt` in its configuration folder.
- Without conversion tables (or for kits they do not have), XG drum notes 13 to 34, which differ from the GS layout, are moved to their GS equivalents on GS and GM modules, or silenced when there is none (TMIDI Player's XG to GS drum patch; the Open Rim Shot plays the tight snare).
- Sound Canvas tone map forcer (Device menu or Settings): make an SC-88, SC-88Pro or SC-8850 use the SC-55, SC-88, SC-88Pro or SC-8850 map for every part. It sets TONE MAP-0 NUMBER after each reset, sends the forced CC#32 with every program change and rewrites the song's own tone map SysEx.

Virtual keyboard

- View > Virtual keyboard (Ctrl+K) plays one channel of any port: with the mouse (glissando; optionally louder when clicking lower on a key), with the computer keyboard while the window has the focus (Z S X D C ... for one octave, Q 2 W 3 E ... for the next, like a tracker; Left/Right octave, Up/Down velocity, PgUp/PgDn program, Space sustain), and with a MIDI input (keyboard, controller or loopback port) while the window is open. The input plays on the window's channel, or keeps its own channels. The window also sets the velocity, the instrument (bank and program, from the instrument definitions), pitch bend and modulation.

Sound Canvas display

- View > Sound Canvas display opens an LCD in the style of the SC-55: part, instrument, level, pan, reverb, chorus, key shift and MIDI channel of the selected part, and the 16x16 dot matrix with the level of the 16 parts. Songs that draw on the SC-55's screen (dot pictures, SysEx `10 01 00`; text, `10 00 00`) show their pictures and messages there, for example `example-midis/Star Games.MID`. Click a column to select that part; right-click for the size and colors: Large is the native 724 x 300 (the size of the Nuked SC-55 emulator's window), Medium and Small are scaled down with filtering so the matrix stays even.
- Colors (right-click, or Settings > Display): presets for the Sound Canvas's orange LCD (the default), the yellow-green LCD of Yamaha's MU modules and other common LCD colors (green, blue with white dots, gray, amber, red, fluorescent cyan, white on black), or your own backlight, dot and unlit dot colors. The level matrix can have colors of its own: one color, the top row (or the top dot of each bar) in another color, or a gradient from the bottom row to the top row through the hues between two colors (green to red like a meter, or blue to red from cold to warm).

Other

- Lyrics tab with karaoke highlighting that follows each syllable. It reads FF 05 lyric events and `.KAR` text events, and shows the song in lines and verses: as the song marks them (KAR's `/` and `\`, XF's `/` and `<`, line breaks in lyric events), a line for each lyric event in songs that put a whole phrase in each, or at the singer's pauses in songs that mark nothing. Verses the lyrics don't mark start at the song's section markers (such as "Verse", "Chorus", "A", "B"; a line that starts up to a beat early belongs to the new section) or after long pauses. Long lines wrap between words (Japanese text between characters).
- File Info tab: SMF format, track list, PPQN resolution and time division (including SMPTE and the raw header value), tempo map, time and key signatures, detected standard, drum channels, loop points, all text events and a log of the SysEx sent to the devices.
- Text encoding is detected per file (UTF-8, Shift-JIS/CP932, Latin-1) and can be overridden. Japanese titles and lyrics need a CJK font, see below.
- Reads `.mid`, `.midi`, `.kar`, `.rmi` (RIFF MIDI), `.smf` (SMF formats 0, 1 and 2), `.mds` (Microsoft MIDI stream files, RIFF `MIDS`) and `.rcp` (Recomposer 2.0 songs from the NEC PC-9801 era, `RCM-PC98V2.0`). Recomposer loops, "same measure" repeats, Roland/Yamaha parameter events, user and track exclusives are expanded. An endless loop plays twice and becomes loop points.
- Reads XMIDI (`.xmi`, the Miles Sound System format of many DOS games). XMIDI's fixed 120 Hz timing is mapped through the song's tempo map, so it plays at the exact speed and shows the right tempo and bars; the FOR/NEXT loop controllers become loop points. A file with several songs plays the first.
- File > Export as Standard MIDI File saves the loaded song (also RCP, XMIDI, MDS or RMI) as a `.mid` file. A loop is marked the RPG Maker way: CC#111 at the loop start, and the song ends at the loop end (notes still sounding there are released). For Apogee EMIDI songs the export holds what ImMidi plays: tracks meant for other sound cards are left out.
- File > Export with emulation for (the device in use) saves the song as ImMidi sends it to that module with emulation, whether or not emulation is on: an XG song exported for an SC-88Pro holds GS resets, SC-88Pro map instruments and drum sets, moved drum notes and GS effects, and plays the same way on the module from any player. The song's tracks, timing, tempo and texts stay, and a loop is marked as above.
- File dialogs are the system's own: the Windows file dialog, the macOS open/save panels, and zenity or kdialog on Linux. The built-in ImGui browser is the fallback, and you can choose it in Settings.
- Settings and the playlist are saved when something changes (after a click, a key or a playlist edit), never on a timer. Files are written atomically, so a crash or forced close cannot leave a half-written file.
- Graphics: the window is drawn with the system's native graphics API, Direct3D 11 on Windows, Metal on macOS and Vulkan on Linux (X11 and Wayland), and is only redrawn while something changes: at the display's refresh rate while the channel view or the karaoke lyrics are shown during playback (and while you use the mouse or keyboard), at 20 to 30 frames per second when only slower things move (the transport on the other tabs and in the mini player, the Sound Canvas display), and not at all when nothing changes. OpenGL is deprecated: it is used only when the native API cannot start (the reason is printed), or when chosen in Settings > Graphics or with `--renderer opengl`. Settings > Graphics and Help > About show the API in use.

## Building

[BUILDING.md](BUILDING.md) has the prerequisites and the steps for Windows, Linux and macOS, with the packages to install on Debian, Ubuntu, Fedora, RHEL and Arch. In short, premake5 5.0.0-beta8 or newer makes the project files, and Visual Studio 2022 or make builds from them:

```sh
premake5 vs2022                                            # Windows: then build build\vs2022\ImMidi.sln
premake5 gmake && make -C build/gmake config=release -j4   # Linux and macOS
```

Everything ImMidi uses is in `third_party/`, so the build downloads nothing. The programs (ImMidi, [ImMidi Bridge](#immidi-bridge), [ImMidi Conversion Editor](#immidi-conversion-editor) and `immidi_cli`) go to `bin/<os>-<config>/` with a copy of `insdef/` and `conversion/`. Release builds carry no debug information in the programs: Visual Studio keeps it in `.pdb` files beside them, and on Linux and macOS the programs are stripped.

## Platform notes

### Windows

MIDI output uses WinMM, so every output device shows up (Microsoft GS Wavetable Synth, VirtualMIDISynth, loopMIDI ports, hardware interfaces). The window is drawn with Direct3D 11 (a flip-model DXGI swap chain with vsync); Dear ImGui's shaders are compiled at start by `d3dcompiler_47.dll`, which is part of Windows 8.1 and later. `ImMidi.exe` is linked against the static runtime, so it runs without extra DLLs. The icon and version information come from `packaging/windows/ImMidi.rc` (the `.ico` is made by `tools/gen_icon.py`, like the macOS icon).

### Linux

The window is drawn with Vulkan. The Vulkan headers and the volk loader come from `third_party/`, and the system's Vulkan loader (`libvulkan.so.1`) and driver (Mesa, NVIDIA, ...) are opened at run time, so building needs no Vulkan package; where Vulkan is missing, ImMidi starts with OpenGL (deprecated) instead.

ImMidi runs natively on Wayland and on X11. In a Wayland session it uses Wayland (the Wayland libraries are loaded at run time, so the same binary also runs on X11-only systems); `--platform x11` or `--platform wayland` overrides the choice. `premake5 gmake --no-wayland` builds for X11 only.

MIDI output uses ALSA. The native file dialogs use `zenity` (GNOME and most desktops) or `kdialog` (KDE) when installed.

The window carries its icon, which X11 desktops and KDE Plasma 6.3 or later show (the patched GLFW in `third_party/glfw` sends it with Wayland's xdg-toplevel-icon protocol). Other Wayland desktops, such as GNOME, take the icon from the desktop entry named after the window's app ID, `org.immidi.ImMidi`: `sh tools/linux-desktop.sh install` installs it, together with an application menu entry, and those of ImMidi Bridge and ImMidi Conversion Editor (see [BUILDING.md](BUILDING.md)).

ImMidi's virtual MIDI port gets a new ALSA address each time it is opened. Programs running under Wine list the ALSA ports only when they start, so start them after ImMidi has opened the port; native Linux programs find it at any time.

### macOS

The window is drawn with Metal (a `CAMetalLayer` on the window). Every build assembles `bin/macosx-<config>/ImMidi.app` (`tools/macos-app.sh`): the Info.plist from `packaging/macos/Info.plist.in`, the icon (`packaging/macos/ImMidi.icns`, made by `tools/gen_icon.py`), `insdef/` and `conversion/` in `Contents/Resources`, and an ad-hoc code signature. ImMidi Bridge and ImMidi Conversion Editor become `ImMidi Bridge.app` and `ImMidi Conversion Editor.app` the same way. Drag the apps to Applications to install them. The version number is set at the top of `premake5.lua`.

MIDI output uses CoreMIDI. The output list also offers "Apple DLS Synth (built-in)", the General MIDI / GS synthesizer that ships with every Mac, played through the default audio output; it is picked at first start when no other MIDI device is connected. Settings > MIDI outputs can give it a SoundFont 2 (`.sf2`) or DLS (`.dls`) sound bank instead of the built-in sounds; ImMidi then plays through Apple's AUMIDISynth, which loads the instruments a song uses when the song is opened (and any others as they are selected). If the bank cannot be loaded, the built-in sounds play and the reason is shown.

### All systems

On Linux and macOS a virtual output port "ImMidi Out" is also offered, which other programs can connect to. ImMidi looks for instrument definitions next to the executable (in `Contents/Resources` inside the macOS app), in the working directory and in `<config dir>/insdef`. The conversion tables come from `<config dir>/conversion` when you have your own copy there (see [ImMidi Conversion Editor](#immidi-conversion-editor)), else from `conversion` next to the executable or in the working directory; ImMidi Bridge and the editor find them the same way. The config dir is `%APPDATA%\ImMidi` on Windows, `~/Library/Application Support/ImMidi` on macOS and `~/.config/ImMidi` on Linux.

## Using it

- Open files with File > Open, by dragging them onto the window, or on the command line. Files dropped onto the playlist (the side pane, or the list in the Playlist tab) are inserted without interrupting playback, before or after the entry under the mouse (an insertion mark shows where while dragging). Files dropped anywhere else in the window are added at the end and the first one plays at once; Settings > Playlist and files can turn that off. Command line: `ImMidi song.mid folder/ list.m3u8 --play`.
- Settings > MIDI outputs sets the output for each MIDI port of the song. Device names are shown without the index numbers the system adds (two devices with the same name become "Name" and "Name (2)"). Loopback ports (ALSA's Midi Through, the macOS IAC bus, loopMIDI) are listed like any other device. Songs with two ports (A/B) need two outputs to sound right, for example both MIDI IN connectors of an SC-88Pro.
- Settings > Sound module sets the target device, emulation and the reset message.

Playlist keys (after clicking the list): Up/Down, Home/End, Page Up/Down move the focus, with Shift to extend the selection; Ctrl+A selects everything, Delete removes the selection (on a Mac also Cmd+Backspace), Enter plays the focused track, Alt+Enter shows its file info, Ctrl+Up/Down moves the selection.

Keyboard shortcuts: Space play/pause, `.` stop, Ctrl+K virtual keyboard, Ctrl+Shift+M mini player, PgUp/PgDn (or B/N) previous/next, Left/Right seek 5 s, `+`/`-` transpose, Esc panic (all notes off), Ctrl+O open, Ctrl+1 to Ctrl+7 switch tabs.

Command line options: `--play`, `--seek <seconds>`, `--device gm|gm2|sc55|sc88|sc88pro|sc8850|xg`, `--out "<output name>"`, `--tab channels|soundedit|effects|lyrics|playlist|info|settings`, `--view 0-3`, `--size WxH`, `--screenshot out.png [--frames N]`, `--frame-stats` (print UI frame times on exit), `--platform wayland|x11` (Linux), `--renderer d3d11|metal|vulkan|opengl` (graphics API for this start; the default is the platform's native API, OpenGL is deprecated).

Settings live in `~/.config/ImMidi` (Linux), `~/Library/Application Support/ImMidi` (macOS) or `%APPDATA%\ImMidi` (Windows).

### Japanese and other CJK text

ImMidi uses a system CJK font when it finds one: Meiryo/MS Gothic/Yu Gothic on Windows, Hiragino on macOS, Noto Sans CJK, Droid Sans Fallback, Takao, VL Gothic or IPA fonts on Linux (`sudo apt install fonts-noto-cjk`). You can also drop any `.ttf`/`.otf`/`.ttc` font into a `fonts` folder next to the executable.

## ImMidi Bridge

ImMidi Bridge (`ImMidiBridge`, `ImMidiBridge.exe` on Windows, `ImMidi Bridge.app` on macOS) is built next to ImMidi. It is ImMidi's emulation as a small program of its own that works in real time: it converts what a MIDI input receives and sends the result to a MIDI output. Sequencers, games and other players can then play songs written for one module on another, for example XG songs on an SC-88Pro or Sound Canvas VA, or GS songs on an MU module or S-YXG50.

- **Input** is where the other program's messages arrive. Let the program send to a loopback port and choose that port here: loopMIDI on Windows, an IAC Driver bus on macOS, Midi Through on Linux. On Linux and macOS the bridge also offers its own virtual ports: programs can send to "ImMidi Bridge In", and "ImMidi Bridge Out" can be the output (connect a synth to it).
- **Output** is the sound module. Loopback ports are listed like any other device. A warning shows when the input and output are the same device, as a loopback port would send every message back in.
- **Source** is the standard or module the incoming messages are written for: GM, GM2, SC-55, SC-88, SC-88Pro, SC-8850 or XG. With **Follow resets** (on by default), a GM, GM2, GS or XG reset in the stream switches it, so a player that sends a reset before each song gets each song converted from its own standard. A GS reset switches to the GS module last chosen as the source, or to the SC-55 when none was, which is also what ImMidi assumes for a GS song that gives no other hint. The bridge sees the messages one at a time, so it cannot tell the SC model from the whole song the way ImMidi does: choose it as the source.
- **Destination** is the module at the output. The conversion is the player's: the same emulation, and the same conversion tables (`conversion/*.json`: your own copy in the config dir, else the ones beside the program or inside the app) with their voices, drum kits, drum notes, volumes and controller scaling. **Use the instrument conversion tables** turns the tables off, and the **Tone map forcer** works as in ImMidi for the Sound Canvases with several maps. The line below says what converts, or that the messages pass unchanged (the same standard on both sides).
- Notes, aftertouch and pitch bend pass straight through, except that drum notes can move to another key or change velocity, as the tables say. Bank selects, program changes, controllers and SysEx are converted. Clock, start/stop and other system messages pass unchanged. Nothing is buffered: each message goes out as soon as it arrives.
- **Send reset to the module** sends the destination's reset (and the forced tone map), and **All notes off** silences every channel. The lights beside the input and output show the messages going through, with counts.
- Changing a setting starts the conversion over, as a reset does. Restart the song (or let it send its reset again) to hear it fully converted.

The bridge keeps its settings in `bridge.ini` in ImMidi's configuration folder and saves them when one changes. It draws with the graphics API set in ImMidi's settings, or with `--renderer`; `--platform wayland|x11` and `--size WxH` work as in ImMidi.

## ImMidi Conversion Editor

ImMidi Conversion Editor (`ImMidiConvEditor`, `ImMidiConvEditor.exe` on Windows, `ImMidi Conversion Editor.app` on macOS) is built next to ImMidi. It edits the conversion tables that ImMidi and ImMidi Bridge convert songs with, for example to add a sound that has no entry yet, to choose another sound for one, or to correct a volume, an attack time or a drum note. ImMidi and the bridge read the tables when they start: restart them after saving.

- The three tables are tabs: **XG to Sound Canvas** (`xg-to-gs.json`), **Sound Canvas to XG** (`gs-to-xg.json`) and **Sound Canvas maps** (`gs-maps.json`, songs for a newer Sound Canvas on an older one). Each has its voices, drum kits, note maps, controller rules and its description.
- **Voices** and **Drum kits** list every sound that has an entry, with the sound each module plays for it; search by name or number. A dimmed sound is another module's: a module without a sound of its own plays the next older module's (the SC-8850 the SC-88Pro's, and so on), as ImMidi picks them; the SC-55 plays the SC-88's only when it is on the SC-55 map. The entry's page shows the source sound and, for each module, **Its own sound**: map, bank and program (PC 1-128, as in the manuals; XG sounds have MSB and LSB), the name, a volume and attack correction, the drum kit's note map and a note. **Choose...** lists a module's sounds by map, from the instrument definitions (Up, Down and Enter work in the list), and **Play** sounds it on the module chosen in the **Audition** menu (a voice on channel 1, a drum kit's notes on channel 10). **Add a voice...** and **Add a drum kit...** add a sound that has no entry yet: its sounds start as those of the same program's bank 0 sound, which ImMidi used for it until then.
- **Note maps** show, for each drum note of the source kit, whether it plays the same key, is not played, or plays another key of the target kit (with a velocity change and a comment), with the sounds of both kits by name; click a sound to hear it. Renaming a note map renames it in the drum kits that use it.
- **Rules** are the controller scaling sets (value * scale / 100 + offset), as ImMidi picks them by the modules.
- While you edit, the editor loads the tables as ImMidi does and says whether ImMidi can load them; an error names the entry, which **Show** opens. Tables ImMidi cannot load are only saved after a warning. Sound names are checked against the instrument definitions, which also name a sound when its numbers change; **Edit > Check the names** lists the ones that do not match and can correct them.
- **Ctrl+Z** and **Ctrl+Y** undo and redo, **Ctrl+S** saves. Saving keeps the tables' layout (one entry per line), so a change is a changed line in a diff, and keeps each table's previous file as `<name>.json.bak` (at the first save of a session).
- The editor opens the tables ImMidi reads, and the line under the folder says when the open folder is another one. The copy a build puts in `bin/` is replaced by the next build: **Open the source folder** opens the repository's `conversion/` instead, which the build copies. On macOS the tables inside an app cannot be changed (an update replaces them, and a change breaks the app's signature): **Save as your own copy** saves them, with your changes, to `conversion` in the config dir, which ImMidi, ImMidi Bridge and the editor read from then on. Delete that folder to go back to the tables that come with ImMidi.

The editor keeps its settings (the folder and the audition outputs) in `convedit.ini` in ImMidi's configuration folder. It draws with the graphics API set in ImMidi's settings, or with `--renderer`; `--platform wayland|x11` and `--size WxH` work as in ImMidi.

## Command line tool

`immidi_cli` is built next to the GUI (examples use the Linux path):

```sh
./bin/linux-Release/immidi_cli info example-midis/*.mid          # format, PPQN, ports, standard, drum channels, loops, lyrics
./bin/linux-Release/immidi_cli lyrics song.mid                   # every lyric line with its time, verses apart, as the Lyrics tab shows them
./bin/linux-Release/immidi_cli ports                             # list MIDI outputs
./bin/linux-Release/immidi_cli play song.mid --device sc88pro --seek 30 --seconds 20 --speed 1.5 --transpose 2 [--emulate]
./bin/linux-Release/immidi_cli emulate song.mid --device sc8850 [--force-map 1] [--tables conversion]  # show converted messages
./bin/linux-Release/immidi_cli convert song.xmi song.mid         # save any song as a Standard MIDI File (RPG Maker loop; --raw: as converted)
./bin/linux-Release/immidi_cli convert song.mid out.mid --emulate sc88pro --tables conversion  # ... as sent to an SC-88Pro with emulation
./bin/linux-Release/immidi_cli selftest example-midis            # drives the player and checks locks, chase, loops, emulation
```

## Where the data comes from

- GS SysEx addresses, NRPNs and the SC-88Pro insertion effect list (types, parameters, value tables) come from the Roland SC-88Pro owner's manual (`SC-88PRO_OM.pdf`). `tools/gen_gs_efx.py` turns the manual's appendix into `src/core/GsEfxData.inc`.
- XG addresses, NRPNs and effect type lists come from the Yamaha MU128 "Sound List & MIDI Data" document. The XG voice and drum kit names in `insdef/Yamaha XG.ins` are generated from the Yamaha QY100 data list by `tools/gen_ins.py`, with the SFX kits' notes checked against the PLG100-XG drum list. The note names of the other drum kits (each kit's own sounds, such as the Symphony Kit's hand cymbals) come from the YAMAHA MU1000/MU2000 instrument definition by kuzu / openmidiproject (2008), made from the MU1000/MU2000 List Book and kept for reference in `reference/openmidiproject` (not built or shipped); `tools/xg_drum_lists.py` drops the list book's model marks and fixes a few typos.
- `insdef/General MIDI.ins` and `insdef/General MIDI Level 2.ins` are generated from the GM2 sound set tables by `tools/gen_ins.py`. All other `.ins` files are the Roland definitions shipped with the project; `Roland SC-88 Pro.ins` has six SC-55 and SC-88 map sounds added and some names corrected from the SC-88Pro tone list, and the SC-88Pro map drum sets of `Roland SC-88 Pro Drumsets.ins` follow the drum set list of the SC-88Pro owner's manual (the electronic sets had only their own notes, and the Jazz, Brush and Orchestra sets named notes that have no sound). In `Roland SC-8850 Drums.ins`, the SC-8850 map's Standard 1 named the notes of a user set, and a few note names were misspelled; they follow the drum set list of the SC-8850 owner's manual now.
- `insdef/Yamaha MU2000.ins` (3,287 voices in 161 banks and 50 drum/SFX kits of the MU2000/MU1000/MU128) is extracted from the voice table inside TMIDI Player 3.8.6 (`TMIDI.EXE`) by `tools/gen_ins_tmidi.py`. The XG device profile uses it and falls back to `Yamaha XG.ins`.
- The conversion tables in `conversion/` were made from the free data files of SMF Knife by Toshikazu Sugama, which are kept for reference in `reference/smfknife` (not built or shipped). Every entry was checked against the tone lists of the SC-88Pro and QY100 manuals and the instrument definitions. Wrong entries (programs and drum kits off by one, wrong variations or banks, sounds the modules do not have, sections that used a capital tone for a sound another section maps exactly) are corrected, and each corrected entry says in its `note` what SMF Knife had. Gaps are filled from the same sound on another map, using the tone list's marks for sounds that are identical on several maps. The drum kits' note maps are ImMidi's own, made from the drum set lists (SC-88Pro owner's manual; MU1000/MU2000 List Book) by sound, keeping SMF Knife's velocity corrections where its notes agree; some sounds are matched by ear (S-YXG50 against Sound Canvas VA): XG's Submarine plays GS's Stream (in the SFX kits and as the SFX voice), and the A1 kicks of the Electro and Symphony kits play the GS sets' B1 kick. The SC-8850 targets of `xg-to-gs.json` come from the instrument list and drum set list of the SC-8850 owner's manual (`SC-8850_OM.pdf`): where the SC-8850 map has the same sound as the SC-88Pro, SC-88 or SC-55 map (the list's [Pro], [88] and [55] marks) or its own version of it, and its own sounds where they are closer (each with a `note`).
- The module keywords and the XG to GS drum note patch come from TMIDI Player's built-in module definitions.
- `src/core/SjisTable.inc` (CP932 to Unicode) is generated by `tools/gen_sjis.py`.

## Limitations

- ImMidi sends MIDI to devices; it does not contain a synthesizer. Use a hardware module or a software synth (FluidSynth, VirtualMIDISynth, Munt, SC-55/SC-88 emulators, ...).
- Recomposer 3 (`.g36`, `.r36`) files are not supported yet, and only the first song of a multi-song XMIDI file plays. Recomposer's CM-64 / GS setup files (`.cm6`, `.gsd`) named in an RCP header are not loaded.
- Emulation maps equivalents where they exist. Instrument variations without a counterpart fall back to the capital (bank 0) tone, and effect parameters are not converted between XG and GS, only effect types, levels and sends.
- XG effect parameters are shown as raw values.
- The part receive channel (GS/XG "Rx. channel") is assumed to be the default.

## Licenses

`third_party/glfw` has a small ImMidi addition (a drag-over callback for X11 and macOS), described in `third_party/glfw/IMMIDI_PATCHES.md`.

ImMidi uses Dear ImGui (MIT), RtMidi (MIT-style), GLFW (zlib), stb_image_write (public domain / MIT), on Linux the Vulkan headers (Apache 2.0 or MIT) and volk (MIT), and the Roboto font (Apache 2.0), embedded in `src/ui/RobotoMedium.inc`.
