// Apple ObjC2 ARM32 dispatch. Preserve register and stack arguments verbatim.
    .syntax unified
    .arm
    .text

.macro LOOKUP helper
    push {r0-r3, r4, lr}
    mov r4, sp
    bic sp, sp, #7
    bl \helper
    mov ip, r0
    mov sp, r4
    pop {r0-r3, r4, lr}
.endm

    .global objc_msgSend
    .type objc_msgSend, %function
objc_msgSend:
    cmp r0, #0
    beq objc_nil_method
    LOOKUP objc_lookup_for_object
    bx ip
    .size objc_msgSend, .-objc_msgSend

    .global objc_msgSendSuper2
    .type objc_msgSendSuper2, %function
objc_msgSendSuper2:
    LOOKUP objc_lookup_super2
    ldr r0, [r0]
    bx ip
    .size objc_msgSendSuper2, .-objc_msgSendSuper2

    .global objc_msgSend_stret
    .type objc_msgSend_stret, %function
objc_msgSend_stret:
    cmp r1, #0
    bxeq lr
    push {r0-r3, r4, lr}
    mov r4, sp
    bic sp, sp, #7
    mov r0, r1
    mov r1, r2
    bl objc_lookup_for_object
    mov ip, r0
    mov sp, r4
    pop {r0-r3, r4, lr}
    bx ip
    .size objc_msgSend_stret, .-objc_msgSend_stret

    .global objc_nil_method
    .type objc_nil_method, %function
objc_nil_method:
    mov r0, #0
    mov r1, #0
    bx lr
    .size objc_nil_method, .-objc_nil_method
    .section .note.GNU-stack,"",%progbits
