| SPDX-License-Identifier: GPL-2.0-or-later
| digislicer: the DIGISLICER machine, GRID = AUTO, PLAY = PIPO and the slice
| editor (slice.c); the glue: the patched sites and the hook-bus handlers
| (see README.md).
        .ifdef  OS154                   | the Digitakt mk1 1.54 (mod.json's port)
        .include "os154.inc"
        .else                           | the Digitakt mk1 1.53
        .include "os153.inc"
        .endif
        .include "digitakt-mk1/core3.inc"

        .section .run, "ax"

| ---- the DIGISLICER machine ---------------------------------------------------
| Machine 5, through core's machine slots (core 2.1, docs/ADAPTING.md "SRC
| machines"): SLICE's parameters and SRC page, and it plays as SLICE (the
| render sees 3), so everything SLICE has works on it. What this mod adds
| (the sample's own slices, AUTO, PIPO, the editor) is for DIGISLICER
| tracks only: core_track_machine[t] tells them apart. The stock SLICE
| machine is left as stock has it.
|
| Its page (core 3.0, drawn by machine-pages: docs/ADAPTING.md "Machine
| pages") is SLICE's, with PLAY and GRID one step further on a DIGISLICER
| track, to PIPO and AUTO: stock's ranges are 0-3 and 0-4 (default 3 and
| 0), on OS 1.53 and 1.54 alike.
        .equ    DSL_ID, 5
        .balign 4
        .globl  dsl_machine
dsl_machine:
        CM_MACHINE DSL_ID, str_dsl, str_dsl_short, dsl_icon, 3, 3, dsl_page
        .balign 4
dsl_page:
        CM_UI   3                                       | SLICE's page
        CM_KNOB                                         | A: TUNE
        CM_KNOB flags=CM_RANGE, min=0, max=0x400        | B: PLAY, to PIPO
        CM_KNOB                                         | C: BR
        CM_KNOB                                         | D: SAMP
        CM_KNOB                                         | E: SLICE
        CM_KNOB                                         | F: LEN
        CM_KNOB flags=CM_RANGE, min=0, max=0x500        | G: GRID, to AUTO
        CM_KNOB                                         | H: LEV
| Its menu icon, 11 x 7 as the stock ones (a word a column, rows in bits
| 31-25): SLICE's ramps, between slice lines at unequal places.
|   #..#....#..
|   #..#....#..
|   ##.##...##.
|   #######.###
|   ##.##...##.
|   #..#....#..
|   #..#....#..
dsl_icon:
        .long   BMP_VT, 11, 7, 1, dsl_icon_px, dsl_icon_mask, 0
dsl_icon_px:
        .long   0xfe000000, 0x38000000, 0x10000000, 0xfe000000, 0x38000000
        .long   0x10000000, 0x10000000, 0x00000000, 0xfe000000, 0x38000000
        .long   0x10000000
dsl_icon_mask:
        .long   0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000
        .long   0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000, 0xfe000000
        .long   0xfe000000
str_dsl:        .asciz  "DIGISLICER"
str_dsl_short:  .asciz  "DSLC"
        .balign 2

| ---- the slice window: own slices, AUTO and PIPO (slice.c) -------------------
| GRID (0-4 = 4..64 equal slices) gets a sixth value on DIGISLICER, 5 =
| AUTO: slices on the sample's transients, found by slice.c. Stock firmware
| clamps GRID to 4, so a kit saved with AUTO plays as the 64 grid there.
|
| At 0x40074df2 (was: lea -24(sp),sp ; moveq #94,d1), the entry of the
| SLICE window function, by jmp: (sp) = return, 4 p (the voice's
| parameters: PLAY at +2, GRID at +12), 8 note, 12 length, 16 v (the
| track). A SLICE voice runs the stock code. On a DIGISLICER voice, the
| sample's own slices, or GRID = AUTO with a table for the voice's slot, go
| to slice_auto_window (same arguments, same d0/d1 result); anything else
| runs the stock code. The FAST AUDIO copy of the block inherits this jmp
| (it copies the patched image), and its fallback runs the image's copy.
| The render calls it every block for every SLICE-like voice, not per
| sample.
|
| A DIGISLICER voice whose PLAY is PIPO (slice.c pp_pre) takes pp_win
| instead: the same window, then pp_step, which may turn the voice and
| returns the window's ends in the order its direction needs (the reverse
| modes' order, swapped).
        .equ PP_PLAY, 4
        .globl  slice_win
slice_win:
        move.l  16(%sp), %d0            | the voice's track machine
        lea     core_track_machine, %a0
        moveq   #0, %d1
        move.b  0(%a0,%d0.l), %d1
        moveq   #DSL_ID, %d0
        cmp.l   %d0, %d1
        beq.s   1f
| A SLICE voice: stock. PLAY = PIPO, which a track keeps when it leaves
| DIGISLICER, plays as stock plays it, FWD: the render's copy of PLAY
| (rewritten every block) reads 3 for this block's later readers.
        movea.l 4(%sp), %a0
        move.b  2(%a0), %d1
        moveq   #PP_PLAY, %d0
        cmp.b   %d0, %d1
        bne.s   2f
        moveq   #3, %d0
        move.b  %d0, 2(%a0)
2:      lea     -24(%sp), %sp           | the replaced instructions
        moveq   #94, %d1
        jmp     SLICE_WIN_ON
1:      move.l  16(%sp), -(%sp)         | pp_pre(p, v)
        move.l  8(%sp), -(%sp)
        jsr     pp_pre
        addq.l  #8, %sp
        tst.l   %d0
        bne.w   pp_win
win_body:
        moveq   #1, %d0                 | any SLICE voice wakes slice_tick
        move.b  %d0, slc_any
        move.l  16(%sp), %d0            | the voice
        mulu.w  #94, %d0
        lea     VOICE_SLOT, %a1         | V(v) + 0x5C: its sample slot
        moveq   #0, %d1
        move.b  0(%a1,%d0.l), %d1
        cmpi.l  #128, %d1
        bcc.s   9f
        lea     slc_n, %a1
        tst.b   0(%a1,%d1.l)
        beq.s   8f
        lea     slc_cust, %a1           | the sample's own slice list: always
        tst.b   0(%a1,%d1.l)
        bne.s   7f
        movea.l 4(%sp), %a0             | GRID = AUTO
        move.b  12(%a0), %d0
        cmpi.b  #5, %d0
        beq.s   7f
        move.l  slc_aud_voice, %d0      | the slice editor auditions this voice
        cmp.l   16(%sp), %d0
        bne.s   9f
7:      jmp     slice_auto_window
8:      movea.l 4(%sp), %a0             | no table yet: an AUTO voice asks for
        move.b  12(%a0), %d0            | one, and plays the 64 grid meanwhile
        cmpi.b  #5, %d0
        bne.s   9f
        lea     slc_want, %a1
        moveq   #1, %d0
        move.b  %d0, 0(%a1,%d1.l)
9:      clr.l   -(%sp)                  | log it (slice.c slc_dbg, over USB):
        pea     -1                      | (v, sel -1 = the stock path, 0)
        move.l  24(%sp), -(%sp)
        jsr     slc_dbg_win
        lea     12(%sp), %sp
        lea     -24(%sp), %sp           | the replaced instructions
        moveq   #94, %d1
        jmp     SLICE_WIN_ON

| PIPO: win_body with the arguments again (PLAY is 4, so either window
| returns the forward order: d0 start, d1 end), then pp_step(v, d0, d1),
| which returns the window to play in d0:d1, in its direction's order.
pp_win:
        move.l  16(%sp), -(%sp)
        move.l  16(%sp), -(%sp)
        move.l  16(%sp), -(%sp)
        move.l  16(%sp), -(%sp)
        jsr     win_body
        lea     16(%sp), %sp
        move.l  %d1, -(%sp)
        move.l  %d0, -(%sp)
        move.l  24(%sp), -(%sp)         | v
        jsr     pp_step
        lea     12(%sp), %sp
        rts

| The render's reads of a SLICE voice's PLAY, straight after the window
| (was: mvs.b 54(a3),d2 ; move.l d2,d3 at 0x40075282 for voice 0, and
| mvs.b 160(a2),d2 ; move.l d2,d3 at 0x400759dc for voices 1-7, the voice in
| d4; 6 bytes each, by jsr). PIPO reads as FWD.L (2) or REV.L (1) as
| pp_dir says; anything else as it is. Only d2 and d3 change.
        .globl  pp_read0, pp_readv
pp_read0:
        mvs.b   54(%a3), %d2
        moveq   #PP_PLAY, %d3
        cmp.l   %d3, %d2
        bne.s   2f
        moveq   #2, %d2
        tst.b   pp_dir
        bpl.s   2f
        moveq   #1, %d2
2:      move.l  %d2, %d3
        rts
pp_readv:
        mvs.b   160(%a2), %d2
        moveq   #PP_PLAY, %d3
        cmp.l   %d3, %d2
        bne.s   2f
        move.l  %a0, -(%sp)
        lea     pp_dir, %a0
        moveq   #2, %d2
        tst.b   0(%a0,%d4.l)
        bpl.s   1f
        moveq   #1, %d2
1:      movea.l (%sp)+, %a0
2:      move.l  %d2, %d3
        rts

| A later stage reads each voice's PLAY again, for its loop flag alone
| ((PLAY - 1) < 2), and ends a voice that does not loop once it reaches its
| window's end (was: move.w 54(a6),d0 ; lea 52(a5),a5 at 0x400760ba, 8
| bytes: jsr + nop). PIPO loops, so it reads as FWD.L there. Only d0 and a5
| change, as they did.
        .globl  pp_read2
pp_read2:
        lea     52(%a5), %a5
        mvs.b   54(%a6), %d0
        subq.l  #PP_PLAY, %d0
        beq.s   1f
        move.w  54(%a6), %d0            | not PIPO: the word as stock read it
        rts
1:      move.w  #0x0200, %d0            | PIPO: FWD.L
        rts

| At 0x4005f91c (was: move.l d2,-(sp) ; moveq #1,d2 ; move.l 12(sp),d0, 8
| bytes: jmp + nop), PLAY's formatter: (sp) = return, 4 its object, 8 the
| value (8.8), 12 the buffer. Stock names 0-3 (REV, REV.L, FWD.L, FWD) and
| writes nothing for anything else; 4 is PIPO, the Octatrack's name.
        .globl  play_fmt
play_fmt:
        move.l  8(%sp), %d0
        asr.l   #8, %d0
        moveq   #PP_PLAY, %d1
        cmp.l   %d1, %d0
        bne.s   1f
        move.l  12(%sp), %d0            | sprintf(buffer, "PIPO")
        move.l  %d0, 4(%sp)
        move.l  #str_pipo, %d0
        move.l  %d0, 8(%sp)
        jmp     SPRINTF
1:      move.l  %d2, -(%sp)             | the replaced instructions
        moveq   #1, %d2
        move.l  12(%sp), %d0
        jmp     PLAY_FMT_ON

| At 0x4006060a (was: lea -12(sp),sp ; move.l 20(sp),d0, 8 bytes: jmp +
| nop), PLAY's icon lambda: (sp) = return, 4 its functor, 8 the value (8.8),
| 12 the Bitmap to draw on, 16 x, 20 y. Stock picks the value's icon from
| the set 0x421f9480 and ends in blit(dst, icon, x, y, 0) (0x400c2960); the
| set has none for 4, so PIPO blits its own (slice.c pp_icon).
        .globl  play_icon
play_icon:
        move.l  8(%sp), %d0
        asr.l   #8, %d0
        moveq   #PP_PLAY, %d1
        cmp.l   %d1, %d0
        bne.s   1f
        jsr     pp_icon
        move.l  12(%sp), %d1            | blit(dst, icon, x, y, 0)
        move.l  %d1, 4(%sp)
        move.l  %d0, 8(%sp)
        move.l  16(%sp), %d1
        move.l  %d1, 12(%sp)
        move.l  20(%sp), %d1
        move.l  %d1, 16(%sp)
        clr.l   20(%sp)
        jmp     BLIT
1:      lea     -12(%sp), %sp           | the replaced instructions
        move.l  20(%sp), %d0
        jmp     PLAY_ICON_ON

str_pipo:   .asciz  "PIPO"
        .balign 2

| At 0x4005fa2e (was: move.l 12(sp),d0 ; move.l 8(sp),d1, 8 bytes: jmp +
| nop), GRID's formatter, by jmp: (sp) = return, 4 its object, 8 the value
| (8.8), 12 the buffer. Stock prints 4 << value with "%d" (0x401cff67)
| through 0x40000e82(buffer, format, ...); AUTO (5) prints "AUTO".
        .globl  grid_fmt
grid_fmt:
        move.l  8(%sp), %d1
        asr.l   #8, %d1
        moveq   #5, %d0
        cmp.l   %d0, %d1
        bne.s   1f
        move.l  12(%sp), %d0            | sprintf(buffer, "%s", "AUTO")
        move.l  %d0, 4(%sp)
        move.l  #FMT_S, %d0             | "%s"
        move.l  %d0, 8(%sp)
        move.l  #str_auto, %d0
        move.l  %d0, 12(%sp)
        jmp     SPRINTF
1:      move.l  12(%sp), %d0            | the replaced instructions
        move.l  8(%sp), %d1
        jmp     GRID_FMT_ON

str_auto:   .asciz  "AUTO"
        .balign 2

| The slice editor's handlers: ev_draw, ev_key and ev_enc (were hook_draw's
| tail, ed_key and ed_enc; the stock calls are core's now). A handler that
| takes an event returns 1 and marks the frame dirty, as ed_key did.

        .equ KEY_PLAY, 10
        .equ KEY_STOP, 11
        .globl  slc_draw, slc_key, slc_enc, ed_srckey

| slc_draw(bmp, ctrl): the editor, over everything, while it is open.
slc_draw:
        tst.l   slc_ed                  | slc_ed.open
        beq.s   1f
        move.l  4(%sp), -(%sp)          | the Bitmap
        jsr     slc_ui_draw
        addq.l  #4, %sp
1:      rts

| slc_key(brain, event) -> 1 when the open editor takes the key: all but
| PLAY and STOP, which still reach the transport, and the page keys, which
| close it (slc_ui_key).
slc_key:
        tst.l   slc_ed
        beq.s   8f
        movea.l 8(%sp), %a0
        move.l  12(%a0), %d0
        moveq   #KEY_PLAY, %d1
        cmp.l   %d1, %d0
        beq.s   8f
        moveq   #KEY_STOP, %d1
        cmp.l   %d1, %d0
        beq.s   8f
        move.l  %a0, -(%sp)
        jsr     slc_ui_key
        addq.l  #4, %sp
        tst.l   %d0
        bne.s   7f
        movea.l 4(%sp), %a0             | not taken: redraw, and on
        moveq   #1, %d0
        move.b  %d0, 0x60(%a0)
        bra.s   8f

| slc_enc(brain, event) -> 1 when the editor is open: it takes every turn.
slc_enc:
        tst.l   slc_ed
        beq.s   8f
        move.l  8(%sp), -(%sp)
        jsr     slc_ui_enc
        addq.l  #4, %sp
7:      movea.l 4(%sp), %a0             | redraw: the view controller's dirty
        moveq   #1, %d0                 | byte (brain + 0x40 + 0x20)
        move.b  %d0, 0x60(%a0)
        rts
8:      moveq   #0, %d0
        rts

| The SRC page's key handler (SamplePageView vtable slot 2, 0x401848dc,
| points here): (sp) return, 4 the view, 8 the event. Stock's runs first:
| pressing SRC on the SRC page steps its sub-page, view + 0x90 (0 the
| parameters, 1 the waveform view). Then slc_ui_srcpost opens the editor
| when a DIGISLICER track's page has just gone to its waveform view.
ed_srckey:
        move.l  8(%sp), -(%sp)
        move.l  8(%sp), -(%sp)
        jsr     SRCKEY
        addq.l  #8, %sp
        move.l  %d0, -(%sp)             | its result
        move.l  8(%sp), -(%sp)
        jsr     slc_ui_srcpost
        addq.l  #4, %sp
        move.l  (%sp)+, %d0
        rts

| ---- the keyboard's trig slice mode (kbd.c) ----------------------------------
| The keyboard's "fold slice" layout: trig key k of slice page p plays note
| 12 + 16p + k. Its test, 0x40028f3c (the keyboard: its keys, LEDs and
| UP/DOWN) and the keyboard menu's copy 0x400a16c4, is true on a SLICE
| track whose SLICE is 0 and FOLD on. On a DIGISLICER track with the
| keyboard on, kbd.c dsl_kb_check makes it true (and notes the track's
| slices in dsl_kb_n); anything else runs the stock test. At both entries
| (was: lea -12(sp),sp ; movem.l d2/a2-a3,(sp), 8 bytes), by jmp. The
| callers test the result's low byte.
        .globl  dsl_kbpred, dsl_kbpred2
dsl_kbpred:
        jsr     dsl_kb_check
        tst.l   %d0
        bne.s   1f
        lea     -12(%sp), %sp           | the replaced instructions
        movem.l %d2/%a2-%a3, (%sp)
        jmp     KBPRED_ON
1:      moveq   #1, %d0
        rts
dsl_kbpred2:
        jsr     dsl_kb_check
        tst.l   %d0
        bne.s   1f
        lea     -12(%sp), %sp
        movem.l %d2/%a2-%a3, (%sp)
        jmp     KBPRED2_ON
1:      moveq   #1, %d0
        rts

| A trig key's note on the fold slice layout plays if it is at most 75,
| slice 64 (was: move.b #75,d1 ; cmp.l d4,d1 at 0x40029d64, 6 bytes, by
| jsr; d4 the note, the flags go to a blt). On DIGISLICER: at most 11 + its
| slices. Only d1 changes.
        .globl  dsl_kblim
dsl_kblim:
        moveq   #75, %d1
        tst.l   dsl_kb_n
        beq.s   1f
        moveq   #11, %d1
        add.l   dsl_kb_n, %d1
1:      cmp.l   %d4, %d1
        rts

| The keyboard's LEDs (its draw, 0x4002965a-0x40029671, 24 bytes, by jmp;
| d6 the fold slice test): d0 = the highest note a key may have and be
| lit, 75 on the fold slice layout, else 84 on an audio track and 127 on a
| MIDI one, then 0x40029672. On DIGISLICER: 11 + its slices, and d5, which
| makes every d5-th key light blue (the fold slice layout: 4 << GRID, where
| its notes wrap), is 4.
        .globl  dsl_kbled
dsl_kbled:
        tst.b   %d6
        bne.s   2f
        moveq   #7, %d0                 | the replaced instructions
        cmp.l   -44(%fp), %d0
        blt.s   1f
        moveq   #84, %d0
        jmp     KBLED_ON
1:      moveq   #127, %d0
        jmp     KBLED_ON
2:      moveq   #75, %d0
        tst.l   dsl_kb_n
        beq.s   3f
        moveq   #11, %d0
        add.l   dsl_kb_n, %d0
        moveq   #4, %d5
3:      jmp     KBLED_ON

| The slice page's setter 0x40025344(trk, page), from UP/DOWN in the
| keyboard and in its menu, straight after the fold slice test (was: lea
| -16(sp),sp ; moveq #3,d0, 6 bytes, by jmp). It refuses a page past 3, and
| -1. On DIGISLICER a page past the last slice's goes to that page
| (kbd.c dsl_kb_page): UP stays on the last page.
        .globl  dsl_kbpage
dsl_kbpage:
        move.l  8(%sp), -(%sp)
        jsr     dsl_kb_page
        addq.l  #4, %sp
        move.l  %d0, 8(%sp)
        lea     -16(%sp), %sp           | the replaced instructions
        moveq   #3, %d0
        jmp     KBPAGE_ON

| The slice page's getter 0x40025314(trk) reads the page byte at +910 of
| the pattern track's data (was: move.b 910(a0),d0 ; bra.s 0x40025340 at
| 0x40025338, 6 bytes, by jsr). The keyboard, its menu and UP/DOWN read it
| straight after the fold slice test. On DIGISLICER a page past the last
| slice's reads as that page: a pattern track keeps its page when its
| sample changes to one with fewer slices. d0 and d1 change.
        .globl  dsl_kbpget
dsl_kbpget:
        moveq   #0, %d0
        move.b  910(%a0), %d0
        tst.l   dsl_kb_n
        beq.s   1f
        move.l  dsl_kb_n, %d1           | the last page: (slices - 1) >> 4
        subq.l  #1, %d1
        asr.l   #4, %d1
        cmp.l   %d1, %d0
        ble.s   1f
        move.l  %d1, %d0
1:      move.l  #KBPGET_BRA, (%sp)      | past the stock's other branch
        rts

| UP/DOWN's popup, "Slice Page: %d/4" (was: pea 0x401c2dbd at 0x400299b2,
| 6 bytes, by jsr): kbd.c dsl_kb_pfmt gives its format, which counts
| DIGISLICER's own pages. d0, d1, a0 and a1 change.
        .globl  dsl_kbpfmt
dsl_kbpfmt:
        jsr     dsl_kb_pfmt
        movea.l (%sp), %a0
        move.l  %d0, (%sp)
        jmp     (%a0)

| The keyboard menu's line "  SLICE PAGE: %d/4" (was: pea 0x401cfe42 at
| 0x400a2042, 6 bytes, by jsr), the same way; a0 holds the text routine
| the menu calls next, so it is kept. d0, d1 and a1 change.
        .globl  dsl_kbmfmt
dsl_kbmfmt:
        move.l  %a0, -(%sp)
        jsr     dsl_kb_mfmt
        movea.l (%sp)+, %a0
        move.l  (%sp), %d1              | the return
        move.l  %d0, (%sp)              | the format, where pea put it
        movea.l %d1, %a1
        jmp     (%a1)

| NOTEON 0x400d53dc(track, note, vel, src, ...) gives the voice it starts
| the preview step's locks, when 0x4020c29c is a step (<= 63; was: moveq
| #63,d1 ; cmp.l 0x4020c29c,d1 at 0x400d553e, 8 bytes, by jsr; the flags go
| to a bcs past it). kbd.c dsl_kb_noteon gives a trig key's note on
| DIGISLICER (the keyboard's call, told by NOTEON's return address) a lock
| of SLICE = 0 instead, so the note picks its slice. d2 the track, d3 the
| note, a6 the frame, a4 and a3 the audio voice flags. Only d1 changes, as
| it did.
        .globl  dsl_kbnote
dsl_kbnote:
        lea     -12(%sp), %sp
        movem.l %d0/%a0-%a1, (%sp)
        move.l  %a3, -(%sp)
        move.l  %a4, -(%sp)
        move.l  %fp, -(%sp)
        move.l  %d3, -(%sp)
        move.l  %d2, -(%sp)
        jsr     dsl_kb_noteon
        lea     20(%sp), %sp
        movem.l (%sp), %d0/%a0-%a1
        lea     12(%sp), %sp
        moveq   #63, %d1                | the replaced instructions
        cmp.l   PREVIEW_STEP, %d1
        rts

| Live recording: the UI's note message handler records a note with
| 0x4001c10a(app, track, note, vel, first, step, micro, b, -1) (was: jsr
| 0x4001c10a at 0x40008d40, by keep2; a2 the message). Here it records it,
| then kbd.c dsl_kb_rec turns a trig key's note on DIGISLICER into a SLICE
| lock. d0 is its result.
        .globl  dsl_kbrec
dsl_kbrec:
        move.l  36(%sp), -(%sp)         | its nine arguments again
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        move.l  36(%sp), -(%sp)
        jsr     REC_NOTE
        lea     36(%sp), %sp
        move.l  %d0, -(%sp)
        move.l  %a2, -(%sp)
        jsr     dsl_kb_rec
        addq.l  #4, %sp
        move.l  (%sp)+, %d0
        rts

| STEP REC: 0x400088b6(ctx, step, vel) sets the cursor step's NOTE with
| 0x40024942(trk, step, note) (was: jsr 0x40024942 at 0x40008904, by keep2;
| a2 = ctx: its +0 points to the track, +4 is the note message).
| kbd.c dsl_kb_stepnote(trk, step, note, ctx) sets it, or a SLICE lock.
        .globl  dsl_kbstep
dsl_kbstep:
        move.l  %a2, -(%sp)             | ctx
        move.l  16(%sp), -(%sp)         | note
        move.l  16(%sp), -(%sp)         | step
        move.l  16(%sp), -(%sp)         | trk
        jsr     dsl_kb_stepnote
        lea     16(%sp), %sp
        rts
