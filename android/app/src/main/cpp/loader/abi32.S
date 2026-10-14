    .syntax unified
    .arm
    .text
    .global arm32_abi_bridge
    .type arm32_abi_bridge, %function
arm32_abi_bridge:
    push {r0-r6, lr}
    mov r4, sp
    mov r5, ip
    sub sp, sp, #128
    bic sp, sp, #7
    mov r0, sp
    add r1, r4, #32
    mov r2, #32
1:
    ldr ip, [r1], #4
    str ip, [r0], #4
    subs r2, r2, #1
    bne 1b
    ldm r4, {r0-r3}
    blx r5
    add sp, r4, #16
    pop {r4-r6, lr}
    bx lr
    .size arm32_abi_bridge, .-arm32_abi_bridge
    .section .note.GNU-stack,"",%progbits
