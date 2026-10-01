| SPDX-License-Identifier: GPL-2.0-or-later
| Digitakt mk1 MAIN OS 1.54: stock routines and data this mod calls or reads
        | ---- stock routines and data (MAIN OS 1.54) ----------------------
        .equ OP_NEW,      0x400d43a8    | operator new(size) -> d0
        .equ ITEM_CTOR,   0x400c4464    | MenuItem(this, label, select, draw, change, id, step)
        .equ MENU_ADD,    0x400c3c9a    | Menu::addItem(menu, item)
        .equ FN_MGR,      0x401376a0    | std::function manager, 4-byte payload
        .equ STR_CTOR,    0x4017af20    | std::string(this, const char*, alloc&)
        .equ INVALIDATE,  0x400c9a3a    | View::invalidate(view)
        .equ BLIT,        0x400c2b88    | blit(dst, src, x, y, centre)
        .equ CHECKBOXES,  0x421f8a3c    | -> Bitmap[2], 0x1c each: empty, ticked
        .equ DRAWALL,     0x400ca5aa    | ViewController::drawAll(ctrl, Bitmap&)
        .equ FILLRECT,    0x400c1bce    | (bmp, x0, y0, x1, y1, colour)
        .equ FRAMERECT,   0x400c19b2    | (bmp, x0, y0, x1, y1, colour), outline
        .equ TEXTF,       0x400c27a4    | (bmp, rawfont, x, y, maxlen, fmt, ...)
        .equ FONT5,       0x40200ebc    | the stock 5-px font
        .equ LOCK,        0x400018ec    | mutex lock(m)
        .equ UNLOCK,      0x40001a0e    | mutex unlock(m)
        .equ HEAPMTX,     0x4219cc00    | the heap's mutex
        .equ HEAPCB,      0x4020bec4    | buddy allocator control block
        .equ SMPFREE,     0x400eb9aa    | sample pool free bytes -> d0
        .equ SYSEX_SEND,  0x400d7046    | bool (buf, len, abort*, port); 2 = USB
        .equ DTMR0,       0xFC070000    | DMA timer 0 mode
        .equ DTCN0,       0xFC07000C    | DMA timer 0 counter, bus clock
        .equ PPMSR0,      0xFC04002D    | peripheral clock set register
        .equ CUR_TCB,     0x4399E798    | the running task's TCB
        .equ IDLE_TCB,    0x4198597C    | the idle task (the init task, parked)
        .equ IDLE_PC,     0x4006932E    | where it spins: bra.b *
        .equ VBR_FN,      0x40001c94    | the call 0x40000538 made
        | ---- the sites' continuations, and what glue.s calls or reads -------
        .equ BMP_VT,       0x401b7734    | the firmware's Bitmap vtable
        .equ SRCKEY,       0x4003b272    | the SRC page's consumeKeyEvent
        .equ SPRINTF,      0x40000e82    | sprintf(buffer, format, ...)
        .equ FMT_S,        0x401d0d7e    | "%s"
        .equ PRANGE,       0x40078f0c    | a parameter's range: (id), a0 the result's place
        .equ PARAM_VT,     0x4017ee58    | the SRC page's parameter object's vtable
        .equ SNDREF_VT,    0x40181630    | the vtable of the object at its +16
        .equ VOICE_SLOT,   0x8000ee20    | V(v) + 0x5C: a voice's sample slot
        .equ SLICE_WIN_ON, 0x40074df8    | the SLICE window function, past its site
        .equ PLAY_FMT_ON,  0x4005f924    | PLAY's formatter, past its site
        .equ PLAY_ICON_ON, 0x40060612    | PLAY's icon lambda, past its site
        .equ GRID_FMT_ON,  0x4005fa36    | GRID's formatter, past its site
        .equ KBPRED_ON,    0x40028f44    | the fold slice test, past its site
        .equ KBPRED2_ON,   0x400a1828    | the keyboard menu's copy of it, past its site
        .equ KBLED_ON,     0x40029672    | the keyboard's LEDs, past their site
        .equ KBPAGE_ON,    0x4002534a    | the slice page's setter, past its site
        .equ KBPGET_BRA,   0x40025340    | the slice page's getter, past its other branch
        .equ PREVIEW_STEP, 0x4020c64c    | the preview step, <= 63 when one
        .equ REC_NOTE,     0x4001c10a    | the UI records a note (app, track, note, vel, ...)
