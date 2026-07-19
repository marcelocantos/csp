// In-between switch: asm saves fp/lr + x19-x24 (the "commonly live"
// subset); the call site clobbers only x25-x28 + vectors.
.text
.globl _mid_jump
.balign 16
_mid_jump:
    sub  sp, sp, #0x50
    stp  fp,  lr,  [sp, #0x00]
    stp  x19, x20, [sp, #0x10]
    stp  x21, x22, [sp, #0x20]
    stp  x23, x24, [sp, #0x30]
    str  lr, [sp, #0x40]
    mov  x4, sp
    mov  sp, x0
    ldp  fp,  lr,  [sp, #0x00]
    ldp  x19, x20, [sp, #0x10]
    ldp  x21, x22, [sp, #0x20]
    ldp  x23, x24, [sp, #0x30]
    ldr  x5, [sp, #0x40]
    add  sp, sp, #0x50
    mov  x0, x4
    ret  x5

.globl _mid_make
.balign 16
_mid_make:
    sub  x0, x0, #0x50
    str  x1, [x0, #0x40]
    stp  xzr, x1, [x0, #0x00]
    ret
