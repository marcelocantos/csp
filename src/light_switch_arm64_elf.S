// Minimal-save context switch, ARM64 ELF (🎯T35.1, paper 33 round 3).
// See light_switch_arm64_macho.S for the contract and frame layout.

.file "light_switch_arm64_elf.S"
.text
.align 2
.global csp_light_jump
.hidden csp_light_jump
.type csp_light_jump, %function
csp_light_jump:
    sub  sp, sp, #0x20
    stp  x29, x30, [sp, #0x00]
    str  x30, [sp, #0x10]       // resume PC = return address
    mov  x4, sp                 // from-context
    mov  sp, x0                 // to-context
    ldp  x29, x30, [sp, #0x00]
    ldr  x5, [sp, #0x10]        // resume PC
    add  sp, sp, #0x20
    mov  x0, x4                 // transfer_t.fctx
                                // x1 = data, unchanged
    ret  x5
.size csp_light_jump,.-csp_light_jump

.global csp_light_make
.hidden csp_light_make
.type csp_light_make, %function
.align 2
// x0 = stack top (16-aligned), x1 = entry fn taking transfer_t in x0/x1.
csp_light_make:
    sub  x0, x0, #0x20
    str  x1, [x0, #0x10]        // resume PC = entry
    stp  xzr, x1, [x0, #0x00]   // fp = 0, lr = entry
    ret
.size csp_light_make,.-csp_light_make

/* Mark that we don't need executable stack. */
.section .note.GNU-stack,"",%progbits
