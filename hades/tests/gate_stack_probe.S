    .text
    .globl _gate_stack_test
    .p2align 2
_gate_stack_test:
    sub sp, sp, #192
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpsr
    mrs x10, fpcr
    stp x9, x10, [sp, #160]
    mov x9, sp
    sub x0, x0, #32
    stp x9, x1, [x0]
    mov sp, x0
    mov x9, #0xA0000000
    msr nzcv, x9
    movz x9, #1
    movk x9, #0x0800, lsl #16
    msr fpsr, x9
    mov x9, #0x00400000
    msr fpcr, x9
    .macro vector_set number
    movi v\number\().16b, #(\number + 1)
    .endm
    .irp number,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    vector_set \number
    .endr
    .macro register_set number
    mov x\number, #(0x100 + \number)
    .endm
    .irp number,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,19,20,21,22,23,24,25,26,27,28,29
    register_set \number
    .endr
#ifdef HADES_SYMBOL_GATE_TEST
    bl _hades_host_stubs + 24
#else
    bl _dm_gate
#endif
    .globl _gate_stack_return
_gate_stack_return:
    stp x16, x17, [sp, #16]
    ldr x17, [sp, #8]
    stp x0, x1, [x17]
    stp x2, x3, [x17, #16]
    stp x4, x5, [x17, #32]
    stp x6, x7, [x17, #48]
    stp x8, x9, [x17, #64]
    stp x10, x11, [x17, #80]
    stp x12, x13, [x17, #96]
    stp x14, x15, [x17, #112]
    ldp x0, x1, [sp, #16]
    stp x0, x1, [x17, #128]
    stp x18, x19, [x17, #144]
    stp x20, x21, [x17, #160]
    stp x22, x23, [x17, #176]
    stp x24, x25, [x17, #192]
    stp x26, x27, [x17, #208]
    stp x28, x29, [x17, #224]
    str x30, [x17, #240]
    .macro vector_store first, second
    stp q\first, q\second, [x17, #(256 + \first * 16)]
    .endm
    vector_store 0, 1
    vector_store 2, 3
    vector_store 4, 5
    vector_store 6, 7
    vector_store 8, 9
    vector_store 10, 11
    vector_store 12, 13
    vector_store 14, 15
    vector_store 16, 17
    vector_store 18, 19
    vector_store 20, 21
    vector_store 22, 23
    vector_store 24, 25
    vector_store 26, 27
    vector_store 28, 29
    vector_store 30, 31
    mrs x0, nzcv
    mrs x1, fpsr
    mrs x2, fpcr
    str x0, [x17, #768]
    str x1, [x17, #776]
    str x2, [x17, #784]
    ldr x0, [sp]
    mov sp, x0
    ldp x9, x10, [sp, #160]
    msr fpsr, x9
    msr fpcr, x10
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #192
    ret
