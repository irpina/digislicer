# digislicer

A slice editor for the Digitakt (mk1) SLICE machine. Slice a sample where
you want, auto-slice it on its transients or on an equal grid, and play
each slice from the sequencer and the trig keys. There is no need to save
a sliced copy to the +Drive first.

It is an [elekloader](https://github.com/irpina/elekloader) mod for OS
1.53. elekloader builds a custom OS file on your own machine, from your
stock OS file and the mods you pick; nothing from Elektron is distributed.

## What it does

- **The slice editor.** Hold YES for about a second on a SLICE track's SRC
  page. The editor opens on the track's sample.
  - It shows the waveform, the slices and the selected slice's start.
  - It opens on the sample's own slices if it has some, and on AUTO's
    otherwise.
- **Your slices play everywhere.** A sample with its own slices plays them
  on every SLICE track that uses it, whatever GRID says. SLICE, keyboard
  slice mode and slice locks pick from them.
- **They are kept.** A second after you close the editor, the slices are
  saved to the +Drive per sample (by content, so they follow the sample into
  every project). They live in `/cfw/slices.a` and `/cfw/slices.b`; a `/cfw`
  folder appears in the sample browser.
- **GRID = AUTO.** GRID goes one past 64 to AUTO: slices on the sample's
  transients, for samples with no slices of their own. A kit saved with AUTO
  plays the 64 grid on stock firmware.

## The editor

| control | |
|---|---|
| knob A | select a slice (one per notch, as the SAMP knob steps) |
| knob B | move its start |
| knob C | move its start by single samples |
| knob D | zoom horizontally, smoothly, around the cursor: turn it faster to zoom faster |
| knob H | zoom vertically, up to 64 times taller, to see quiet parts |
| LEVEL | move the cursor; the waveform scrolls with it |
| LEFT / RIGHT | previous / next slice, played while held |
| trig keys | select slice 1-16 and play it while held (UP / DOWN: 17-32, 33-48, 49-64) |
| FUNC + LEFT / RIGHT | previous / next sample slot (the track's SAMP moves with it) |
| FUNC + NO | delete the selected slice |
| FUNC + YES | AUTO SLICE |
| **YES** | **the slice menu** |
| NO | close (the slices are kept) |
| PLAY, STOP | work as ever |

**The cursor** works as the Octatrack's waveform marker. It is an I-beam
on the waveform.
- It sits on the selected slice's start, and jumps there whenever you
  select a slice or move its start.
- LEVEL moves it anywhere: a view column at a time when turned slowly,
  faster when turned faster. The waveform scrolls to keep it an eighth of
  the screen from the edge.
- Knob D zooms around it.
- YES then offers **ADD SLICE HERE**.

The screen:
- **The two bottom rows** name what the knobs do, laid out as the knobs
  are, like the stock parameter pages:
  - `A:SEL  B:MOVE  C:FINE  D:ZOOM X` on the upper row;
  - `LVL:CURSOR` with the cursor's position, and `H:ZOOM Y` under D, on
    the lower row.
- **The top row** shows the selected slice and how many there are, the
  sample slot, and the trig keys' page (P2-P4).
- **The line under the top row** is the whole sample, as on the
  Octatrack. The solid part of it is the part in view.
- **The slider right of the waveform** fills as knob H makes the waveform
  taller, as on the Octatrack.

**The slice menu** (YES): UP / DOWN or knob A choose, YES does it, NO
backs out.

- **ADD SLICE HERE**: a new slice starts at the cursor, and is selected.
  As on the Octatrack, it is listed only when a slice can start there: not
  on or within 64 samples of a start or of the end, and not with 64
  slices already.
- **SPLIT SLICE**: cuts the selected slice at its middle.
- **DELETE SLICE**: removes it; its start joins the slice before.
- **AUTO SLICE**: slices on the sample's transients.
- **CREATE GRID <n>**: n equal slices (LEFT / RIGHT: 4, 8, 16, 32, 64),
  each start moved back to a zero crossing.
- **DELETE ALL**: the sample goes back to plain GRID playback.

At most 64 slices a sample.

## Install

You need three things:
- **elekloader**:
  - **Windows:** download `elekloader-<version>-windows.zip` from
    [elekloader's releases](https://github.com/irpina/elekloader/releases/latest),
    unzip it and run `elekloader.exe`. The core mod, which every linkable mod
    needs, is built in.
  - **Other systems:** run elekloader from source with Python 3.9 or newer
    (see [its README](https://github.com/irpina/elekloader#install)). There
    you also need `core-2.0a.elemod`, which is attached to this repository's
    releases too.
- **This mod:** `digislicer-1.2.elemod`, from
  [this repository's releases](https://github.com/irpina/digislicer/releases/latest).
- **The stock OS file:** `Digitakt_OS1.53.syx`, from
  [Elektron's Digitakt downloads](https://www.elektron.se/support-downloads/digitakt).
  The mod is for the Digitakt mk1 on OS 1.53 only; elekloader recognises
  the file by its hash.

Then build your OS in elekloader's window:

1. **Change stock firmware...** (top right): choose `Digitakt_OS1.53.syx`.
2. **+ Install from file...**: choose `digislicer-1.2.elemod`. From source,
   install `core-2.0a.elemod` the same way.
3. **Tick digislicer.** core is ticked with it. The check below the list should
   say "No conflicts ... Ready to build". To add [digihealth](https://github.com/irpina/digihealth) (FAST AUDIO and SYSTEM INFO), install and tick it as well.
4. **OS version shown**: the 4 characters the unit will show, for example
   `SL12`.
5. **BUILD FIRMWARE**, and save the `.syx`. elekloader verifies it before
   writing it.

Flash it with Elektron Transfer, as for any OS update
([Elektron's instructions](https://support.elektron.se/support/solutions/articles/43000662890-how-to-update-your-device)):
1. Connect the unit over USB.
2. In Transfer, select the unit and **Connect**.
3. Drag the `.syx` onto **Drop files here**.
4. Press **YES** on the unit.

Don't turn it off until the upgrade is done.

Or on the command line (elekloader from source):

```bash
python -m elekloader.patch --stock Digitakt_OS1.53.syx \
    --mod core-2.0a.elemod --mod digislicer-1.2.elemod \
    --out Digitakt_OS1.53-slicer.syx --version SL12
```

**Recovery:** elekloader never changes the bootloader, so the stock OS
file always restores the unit. Hold **FUNC** while powering on for the
startup menu, and press **TRIG 4** for OS UPGRADE. Then send the stock
`.syx` with Transfer's legacy OS upgrade mode.

## Build it from source

The Digitakt mk1 cross toolchain (m68k binutils and gcc; on Debian or
Ubuntu, `apt install binutils-m68k-linux-gnu gcc-m68k-linux-gnu`; on
Windows, inside WSL) and elekloader:

```bash
python -m elekloader.sdk.build . --stock Digitakt_OS1.53.syx       # -> out/digislicer-1.2.elemod
python -m elekloader.lint out/digislicer-1.2.elemod --stock Digitakt_OS1.53.syx --with core-2.0a.elemod
```

| file | |
|---|---|
| `mod.json` | the mod: its sites, its hook-bus handlers, its resources |
| `slice.c` | the transient analysis, the slice tables, the +Drive store, the editor |
| `glue.s` | the patched sites (the SLICE window, GRID's text, the SRC page's key handler) and the handlers |
| `os153.inc` | the stock routines it calls |

## How it was checked

These checks ran in digikit's emulator, which runs the stock OS and the
mods through the real bootloader.

- **Against the build it came from:** the mod is the tested custom build
  1.8J split into mods. Linked with core and digihealth, it gives the same
  state as 1.8J at every step of the editor tests: open, select, zoom,
  move, split, delete, the menu (CREATE GRID, DELETE ALL), the arrows and
  the audition windows.
- **A cold boot of core + digislicer against stock:** every screen is
  identical. The audio is identical, apart from one 1 ms block of silence
  at the end of the recording: the hook bus changes the UI task's timing by
  a few instructions, and `core` alone does the same.
- **A cold boot of core + digihealth + digislicer against 1.8J:** a slicer
  script (GRID to AUTO, the SRC page, trigs, then the pattern playing).
  The audio is identical for 20.9 s. The last, silent 51 ms differ by
  under 0.05 of a 16-bit step (RMS).
- **On a unit:** the editor, its slices playing on the pattern, the
  arrows and the trig-key audition were tried on a Digitakt mk1 in the
  custom builds this mod comes from.
- **1.1 (the smooth zoom and the knob labels):**
  - Knob A gets the same events and filter state as in 1.0, event for
    event.
  - Knob D zooms 3/8 of an octave a notch, to the deepest zoom and back.
  - At every zoom level tried, each slice start is drawn in the column
    whose waveform holds it.
  - The editor, menu and arrow tests pass: split, delete, the menu, the
    audition windows, the store on the +Drive, and the arrows. B moves a
    start one view column at a time, so its steps now follow the finer
    zoom.
  - A cold boot of core + digislicer 1.1 against 1.0: every screen and
    the audio are identical.
  - Not yet tried on a unit.
- **1.2 (the vertical zoom, the view line and the slider; the cursor and
  ADD SLICE HERE):**
  - On every frame of a test that zooms D in and out, then H up to 64
    times and back, each slice start, the view line and the slider are
    exactly where they should be.
  - **The cursor:**
    - LEVEL moves it six view columns a notch, and leaves the selected
      slice alone.
    - D zooms around it.
    - Scrolled right, the view keeps it an eighth of the view from the
      edge, and it is drawn in its column with its I-beam.
  - **ADD SLICE HERE:**
    - It is listed only off a slice start.
    - It adds the slice at the cursor, in order, and selects it.
    - The slice plays at once (the slot's table is republished), and is
      stored on the +Drive when the editor closes.
  - **The labels:** in two rows, as the knobs are laid out; the frame
    checks above ran on this layout.
  - H leaves the view alone, and D leaves H's zoom alone. Back at no zoom,
    the screen is the one before, pixel for pixel.
  - Knob A steps exactly as the firmware's filter does for the events it
    gets, as in 1.1.
  - The editor, menu and arrow tests pass.
  - A cold boot against 1.0: every screen and the audio are identical.
  - Not yet tried on a unit.

## Licence

GPL-2.0-or-later. digislicer is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 2 of the
License, or (at your option) any later version. It is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details: [LICENSE](LICENSE)
holds version 2.

Not affiliated with Elektron. Digitakt is a trademark of Elektron. Custom
firmware is at your own risk.
