| digislicer: SLICE GRID = AUTO and the slice editor (slice.c); the glue:
| the patched sites and the hook-bus handlers (see README.md).
        .include "os153.inc"

        .section .run, "ax"

| ---- the SLICE machine's AUTO grid (slice.c) --------------------------------
| GRID (a SLICE machine parameter, 0-4 = 4..64 equal slices) gets a sixth
| value, 5 = AUTO: slices on the sample's transients, found by slice.c.
| Stock firmware clamps GRID to 4, so a kit saved with AUTO plays as the
| 64 grid there.
|
| At 0x40074df2 (was: lea -24(sp),sp ; moveq #94,d1), the entry of the
| SLICE window function, by jmp: (sp) = return, 4 p (the voice's
| parameters: GRID at +12), 8 note, 12 length, 16 v. GRID = AUTO with a
| table for the voice's slot goes to slice_auto_window (same arguments,
| same d0/d1 result); anything else runs the stock code. The FAST AUDIO
| copy of the block inherits this jmp (it copies the patched image), and
| its fallback runs the image's copy: the function runs once per voice
| start, not per sample.
        .globl  slice_win
slice_win:
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
| PLAY and STOP, which still reach the transport.
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
        bra.s   7f

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
| points here): (sp) return, 4 the view, 8 the event. Holding YES on a
| SLICE track opens the editor (slc_ui_srckey); anything else is stock's.
ed_srckey:
        move.l  8(%sp), -(%sp)
        move.l  8(%sp), -(%sp)
        jsr     slc_ui_srckey
        addq.l  #8, %sp
        tst.l   %d0
        beq.s   1f
        rts                             | taken
1:      jmp     SRCKEY
