// ARMv6 variadic arguments are consecutive register spills followed by stack words.
    .syntax unified
    .arm
    .text
.macro VAR name, impl, reg, offset
    .global \name
    .type \name, %function
\name:
    push {r0-r3}
    add \reg, sp, #\offset
    push {r4, lr}
    mov r4, sp
    bic sp, sp, #7
    bl \impl
    mov sp, r4
    pop {r4, lr}
    add sp, sp, #16
    bx lr
    .size \name, .-\name
.endm
VAR hle_printf, hle_printf_impl, r1, 4
VAR hle_snprintf, hle_snprintf_impl, r3, 12
VAR hle_sprintf, hle_sprintf_impl, r2, 8
VAR hle_fprintf, hle_fprintf_impl, r2, 8
VAR hle_sscanf, hle_sscanf_impl, r2, 8
VAR hle_open, hle_open_impl, r2, 8
VAR hle_NSLog, hle_NSLog_impl, r1, 4
VAR hle_CFStringCreateWithFormat, hle_CFStringCreateWithFormat_impl, r3, 12
VAR hle_CFStringAppendFormat, hle_CFStringAppendFormat_impl, r3, 12
VAR hle_NSString_stringWithFormat, hle_NSString_stringWithFormat_impl, r3, 12
VAR hle_NSString_initWithFormat, hle_NSString_initWithFormat_impl, r3, 12
VAR hle_NSMutableString_appendFormat, hle_NSMutableString_appendFormat_impl, r3, 12
VAR hle_NSDictionary_dictionaryWithObjectsAndKeys, hle_NSDictionary_dictionaryWithObjectsAndKeys_impl, r3, 12
VAR hle_NSArray_arrayWithObjects, hle_NSArray_arrayWithObjects_impl, r3, 12
    .section .note.GNU-stack,"",%progbits
