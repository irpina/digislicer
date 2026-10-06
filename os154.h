/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Digitakt mk1 MAIN OS 1.54: the stock routines and data slice.c and kbd.c
 * call or read (os153.h and os154.h, one per OS; they include the one
 * OS154 picks). */
#define OS_SMP_TAB      0x4031A3A0u  /* +16 slot: PCM, rate, length, ratio */
#define OS_REF_TAB      0x421F330Cu  /* +16 slot: the loader's reference; +4 content hash */
#define OS_VOICE_SLOT   0x8000EE20u  /* V(v) + 0x5C, V = 0x8000EDC4 + 94 v */
#define OS_EK_OPEN      0x400cf3a0   /* ekFS open(path, mode, file) */
#define OS_EK_READ      0x400cf0e6   /* ekFS read(buf, len, file) */
#define OS_EK_WRITE     0x400cf146   /* ekFS write(buf, len, file) */
#define OS_EK_CLOSE     0x400cf35e   /* ekFS close(file) */
#define OS_EK_MKDIR     0x400ce034   /* ekFS mkdir(path) */
#define OS_EK_LOOKUP    0x400d0ccc   /* ekFS lookup(path, ...) */
#define OS_EK_LOCK      0x40001884   /* mutex lock(m) */
#define OS_EK_UNLOCK    0x400019b6   /* mutex unlock(m) */
#define OS_EK_MUTEX     0x42686690   /* the filesystem's mutex (its owner first) */
#define OS_EK_MOUNTED   0x420eec50   /* the drive is mounted */
#define OS_PP_TRIG      0x80001228   /* bit v: voice v starts */
#define OS_PP_POS       0x8000EDC8   /* V(v) + 4: a voice's position */
#define OS_ICON_SET     0x421fa480   /* PLAY's icon set */
#define OS_ICON_PICK    0x400c3034   /* the icon set's picker */
#define OS_FILLRECT     0x400c1bce   /* (bmp, x0, y0, x1, y1, colour) */
#define OS_FRAMERECT    0x400c19b2   /* ... an outline */
#define OS_VLINE        0x400c1268   /* (bmp, x, y0, y1, colour) */
#define OS_PIXEL        0x400c0f1c   /* (bmp, x, y, colour) */
#define OS_TEXTF        0x400c27a4   /* (bmp, rawfont, x, y, maxlen, fmt, ...) */
#define OS_FONT5        0x40200ebc   /* the stock 5-px font */
#define OS_NOTEON       0x400d5604   /* note on(track, note, vel, ...) */
#define OS_NOTEOFF      0x400d5986   /* note off(track, note, src) */
#define OS_MACHINE      0x4002b5d4   /* the SRC page's machine */
#define OS_TRACK_OF     0x4001d24e   /* (view + 116) -> the page's track */
#define OS_KNOB_FILTER  0x400c0816   /* the knob filter (state, event, config) */
#define OS_KNOB_TICK    0x400c05f4   /* its tick (state, timeout) */
#define OS_KNOB_NOTCH   0x4208db64   /* its default config: the notches */
#define OS_VOICE_ON     0x8000EDECu  /* V(v) + 0x28: a voice is on */
#define OS_APP          0x40138882   /* the app */
#define OS_TSTATE       0x40014d86   /* (app) -> the TrackState */
#define OS_KB_ON        0x4001d4f0   /* (TrackState) -> the keyboard is on */
#define OS_ACTIVE       0x4001d27e   /* (TrackState) -> the active track */
#define OS_KIT          0x40014d92   /* (app) -> the kit */
#define OS_SOUND        0x4000d7be   /* (kit, t) -> track t's sound */
#define OS_PATTERN      0x40015786   /* (app) -> the pattern */
#define OS_PTRACK       0x40012a02   /* (pattern, t) -> its track t */
#define OS_LOCKSET      0x40024768   /* a p-lock on a step */
#define OS_NOTESET      0x40024942   /* a trig's NOTE */
#define OS_LOCKLIST     0x400ee20e   /* a lock list for a voice, emptied */
#define OS_PREVIEW      0x4020c64c   /* the preview step, <= 63 when one */
#define OS_PFMT         0x401c313d   /* "Slice Page: %d/4" */
#define OS_MFMT         0x401d01f6   /* "  SLICE PAGE: %d/4" */
#define OS_KB_CALL      0x40028bb4   /* NOTEON's return in the keyboard's call */
#define OS_OP_NEW       0x400d43a8   /* operator new(size) -> d0, 0 if the heap is full */
