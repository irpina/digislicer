| SPDX-License-Identifier: GPL-2.0-or-later
| digislicer: the DIGISLICER machine, GRID = AUTO, PLAY = PIPO and the slice
| editor (slice.c); the glue: the patched sites and the hook-bus handlers
| (see README.md).
        .include "os153.inc"

        .section .run, "ax"

| ---- the DIGISLICER machine ---------------------------------------------------
| Machine 5, through core's machine slots (core 2.1, docs/ADAPTING.md "SRC
| machines"): SLICE's parameters and SRC page, and it plays as SLICE (the
| render sees 3), so everything SLICE has works on it. What this mod adds
| (the sample's own slices, AUTO, PIPO, the editor) is for DIGISLICER
| tracks only: core_track_machine[t] tells them apart. The stock SLICE
| machine is left as stock has it.
        .equ    DSL_ID, 5
        .equ    BMP_VT, 0x401b73b4      | the firmware's Bitmap vtable
        .balign 4
        .globl  dsl_machine
dsl_machine:
        .long   DSL_ID, str_dsl, str_dsl_short, dsl_icon, 3, 3
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
        jmp     0x40074df8
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
        lea     0x8000ee20, %a1         | V(v) + 0x5C: its sample slot
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
        jmp     0x40074df8

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
        jmp     0x40000e82
1:      move.l  %d2, -(%sp)             | the replaced instructions
        moveq   #1, %d2
        move.l  12(%sp), %d0
        jmp     0x4005f924

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
        jmp     0x400c2960
1:      lea     -12(%sp), %sp           | the replaced instructions
        move.l  20(%sp), %d0
        jmp     0x40060612

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
        move.l  #0x401d09ca, %d0        | "%s"
        move.l  %d0, 8(%sp)
        move.l  #str_auto, %d0
        move.l  %d0, 12(%sp)
        jmp     0x40000e82
1:      move.l  12(%sp), %d0            | the replaced instructions
        move.l  8(%sp), %d1
        jmp     0x4005fa36

str_auto:   .asciz  "AUTO"
        .balign 2

| The slice editor's handlers: ev_draw, ev_key and ev_enc (were hook_draw's
| tail, ed_key and ed_enc; the stock calls are core's now). A handler that
| takes an event returns 1 and marks the frame dirty, as ed_key did.

        .equ KEY_PLAY, 10
        .equ KEY_STOP, 11
        .equ SRCKEY,  0x4003b272        | the SRC page's consumeKeyEvent
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

| A parameter's range: 0x40078f0c(id), a0 the result's place, copies its
| {min, max, default} from the parameter table and returns that place. On
| the SRC page a knob steps its value within that range (0x4000f534, in
| 0x4000f50a, for list parameters such as PLAY and GRID) or scales its turn
| into it (0x400100c4, in the page's parameter setter 0x40010026); the
| value must then pass the parameter object's check (slot 9, 0x40011576 ->
| 0x4000f5da: min <= value <= max, its range got through a2, 0x4000f5fc);
| and the setter clamps to it (0x4000ff20, in 0x4000fef6). These four come
| here, by keep2. On a DIGISLICER track, PLAY and GRID go one step
| further, to PIPO and AUTO; SLICE keeps stock's range. The page's
| parameter object (vtable 0x4017eb58) is in a2 at the three calls, and
| 0x4000f5da's first argument at the fourth; it reaches its track's sound
| as obj[16][16] (the object at +16 has vtable 0x40181330, whose slot 10
| returns its +16). Any other object gets stock's range.
        .equ    P_PLAY, 0x85
        .equ    P_GRID, 0x8a
        .globl  dsl_prange, dsl_prange_f
dsl_prange:                             | the object in a2
        movea.l %a2, %a1
        bra.s   1f
dsl_prange_f:                           | from 0x4000f5da: its first argument
        movea.l 36(%sp), %a1
1:      move.l  %a1, -(%sp)             | the object
        move.l  %a0, -(%sp)             | the result's place
        move.l  12(%sp), -(%sp)         | the id
        jsr     0x40078f0c
        addq.l  #4, %sp
        movea.l (%sp)+, %a0
        movea.l (%sp)+, %a1
        move.l  4(%sp), %d1             | the id
        cmpi.l  #P_PLAY, %d1
        beq.s   2f
        cmpi.l  #P_GRID, %d1
        bne.s   9f
2:      move.l  (%a1), %d1
        cmpi.l  #0x4017eb58, %d1
        bne.s   9f
        movea.l 16(%a1), %a1
        move.l  (%a1), %d1
        cmpi.l  #0x40181330, %d1
        bne.s   9f
        movea.l 16(%a1), %a1            | the sound
        moveq   #0, %d1
        move.b  126(%a1), %d1           | its machine
        subq.l  #DSL_ID, %d1
        bne.s   9f
        move.l  4(%a0), %d1             | the max, one step on: PIPO, AUTO
        addi.l  #0x100, %d1
        move.l  %d1, 4(%a0)
9:      move.l  %a0, %d0
        rts
