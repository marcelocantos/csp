; Minimal-save context switch, ARM64 macOS (🎯T35.1, paper 33 round 3).
;
; Contract: unlike Boost fcontext (which preserves all AAPCS
; callee-saved registers), this switch saves only fp/lr (+ resume PC).
; The C++ call-site wrapper (csp/fcontext.h) declares x19-x28 and the
; full vector file as clobbers, so the compiler spills exactly the
; live subset — measured 2.4x faster than fcontext, and still faster
; at 12 live values (bench/lightswitch/switchcmp.cc).
;
; Frame layout (0x20 bytes, sp stays 16-aligned):
;   [0x00] fp   [0x08] lr   [0x10] resume PC   [0x18] pad

.text
.private_extern _csp_light_jump
.globl _csp_light_jump
.balign 16
_csp_light_jump:
    sub  sp, sp, #0x20
    stp  fp, lr, [sp, #0x00]
    str  lr, [sp, #0x10]        ; resume PC = return address
    mov  x4, sp                 ; from-context
    mov  sp, x0                 ; to-context
    ldp  fp, lr, [sp, #0x00]
    ldr  x5, [sp, #0x10]        ; resume PC
    add  sp, sp, #0x20
    mov  x0, x4                 ; transfer_t.fctx
                                ; x1 = data, unchanged
    ret  x5

.private_extern _csp_light_make
.globl _csp_light_make
.balign 16
; x0 = stack top (16-aligned), x1 = entry fn taking transfer_t in x0/x1.
; Returns the initial context sp. fp = 0 terminates backtraces; the
; entry function never returns (imps exit via do_switch(exit)).
_csp_light_make:
    sub  x0, x0, #0x20
    str  x1, [x0, #0x10]        ; resume PC = entry
    stp  xzr, x1, [x0, #0x00]   ; fp = 0, lr = entry
    ret
