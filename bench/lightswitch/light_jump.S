// Minimal ARM64 context switch: saves only fp/lr (+resume PC).
// The C call site declares x19-x28 and v8-v15 as clobbers, so the
// compiler spills exactly the live subset — the asm needs none of it.
.text
.globl _light_jump
.balign 16
_light_jump:
    sub  sp, sp, #0x20
    stp  fp, lr, [sp, #0x00]
    str  lr, [sp, #0x10]        // resume PC = return address
    mov  x4, sp                 // from-context
    mov  sp, x0                 // to-context
    ldp  fp, lr, [sp, #0x00]
    ldr  x5, [sp, #0x10]        // resume PC
    add  sp, sp, #0x20
    mov  x0, x4                 // transfer_t.fctx
                                // x1 = data, unchanged
    ret  x5

.globl _light_make
.balign 16
// x0 = stack top (16-aligned), x1 = entry fn. Returns context sp.
_light_make:
    sub  x0, x0, #0x20
    str  x1, [x0, #0x10]        // resume PC = entry
    stp  xzr, x1, [x0, #0x00]   // fp = 0, lr = entry (entry never returns)
    ret
