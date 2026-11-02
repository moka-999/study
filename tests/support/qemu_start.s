/* 仅供QEMU离线运行实际C模块及测试桩，不得烧入STM32开发板。 */
.syntax unified
.cpu cortex-m7
.thumb
.section .isr_vector,"a",%progbits
.word 0x20400000
.word test_reset
.rept 14
.word test_fault
.endr
.section .text
.thumb_func
.global test_reset
test_reset:
    /* 与原GNU硬浮点ABI兼容；测试本身不依赖浮点。 */
    ldr r0, =0xE000ED88
    ldr r1, [r0]
    orr r1, r1, #0xF00000
    str r1, [r0]
    dsb
    isb
    /* setjmp等标准库可能带初始化数据，按链接器装载地址复制后再进入C。 */
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
3:  cmp r1, r2
    bhs 4f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 3b
4:
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
1:  cmp r0, r1
    bhs 2f
    str r2, [r0], #4
    b 1b
2:  bl test_main
    b test_fault
.thumb_func
.global test_fault
test_fault:
    bl test_abort
    b .
