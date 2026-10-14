    .syntax unified
    .arm
    .text
    .global ts3a_unimplemented_stub
    .type ts3a_unimplemented_stub, %function
ts3a_unimplemented_stub:
    push {r4, lr}
    mov r4, sp
    bic sp, sp, #7
    mov r0, ip
    bl ts3a_unimplemented_report
    mov sp, r4
    pop {r4, pc}
    .size ts3a_unimplemented_stub, .-ts3a_unimplemented_stub
    .section .note.GNU-stack,"",%progbits
