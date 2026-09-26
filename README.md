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
| knob D | zoom |
| LEFT / RIGHT | previous / next slice, played while held |
| trig keys | select slice 1-16 and play it while held (UP / DOWN: 17-32, 33-48, 49-64) |
| FUNC + LEFT / RIGHT | previous / next sample slot (the track's SAMP moves with it) |
| FUNC + NO | delete the selected slice |
| FUNC + YES | AUTO SLICE |
| **YES** | **the slice menu** |
| NO | close (the slices are kept) |
| PLAY, STOP | work as ever |

**The slice menu** (YES): UP / DOWN or knob A choose, YES does it, NO
backs out.

- **SPLIT SLICE**: cuts the selected slice at its middle.
- **DELETE SLICE**: removes it; its start joins the slice before.
- **AUTO SLICE**: slices on the sample's transients.
- **CREATE GRID <n>**: n equal slices (LEFT / RIGHT: 4, 8, 16, 32, 64),
  each start moved back to a zero crossing.
- **DELETE ALL**: the sample goes back to plain GRID playback.

At most 64 slices a sample.

## Install

You need Python 3.9 or newer, [elekloader](https://github.com/irpina/elekloader),
the stock `Digitakt_OS1.53.syx` Elektron publishes, and two mods:
`core-2.0a.elemod` (from elekloader, `mods/core`) and
`digislicer-1.0.elemod` (from this repository's releases, or built as
below).

1. In elekloader's window: **Install from file** both mods, tick them,
   choose your stock file, **Build firmware**. Or on the command line:

   ```bash
   python -m elekloader.patch --stock Digitakt_OS1.53.syx \
       --mod core-2.0a.elemod --mod digislicer-1.0.elemod \
       --out Digitakt_OS1.53-slicer.syx --version SL10
   ```

2. Send the `.syx` to the unit the way you send any OS update.

**Recovery:** hold FUNC while powering on for the startup menu, then send
the stock OS file. elekloader never touches the bootloader, so the stock
file always restores the unit.

digislicer combines with [digihealth](https://github.com/irpina/digihealth)
(FAST AUDIO and SYSTEM INFO): add its `.elemod` to the same build.

## Build it from source

The Digitakt mk1 cross toolchain (m68k binutils and gcc; on Debian or
Ubuntu, `apt install binutils-m68k-linux-gnu gcc-m68k-linux-gnu`; on
Windows, inside WSL) and elekloader:

```bash
python -m elekloader.sdk.build . --stock Digitakt_OS1.53.syx       # -> out/digislicer-1.0.elemod
python -m elekloader.lint out/digislicer-1.0.elemod --stock Digitakt_OS1.53.syx --with core-2.0a.elemod
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

## Licence

GPL-2.0: see [LICENSE](LICENSE). Not affiliated with Elektron. Digitakt is
a trademark of Elektron. Custom firmware is at your own risk.
