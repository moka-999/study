/* B的C运行环境、异常入口与最终跳转。不初始化App硬件，也不启动RTOS。 */
.syntax unified
.cpu cortex-m7
.thumb
.section .isr_vector,"a",%progbits
.global boot_vectors
boot_vectors:
.word 0x20020000
.word Boot_Reset_Handler
.rept 164
.word Fault_Handler
.endr

.section .text.startup,"ax",%progbits
.thumb_func
.global Boot_Reset_Handler
Boot_Reset_Handler:
    cpsid i
#if defined(BOOT_BOARD_B1)
    /* B1耦合复位：在RAM初始化及任何Flash校验前关闭PC6/PC7/PG11。
     * 先锁存低，再切推挽输出；PC6是真实4V总闸，PG11只作防御性关断。 */
    ldr r0, =0x580244E0
    ldr r1, [r0]
    orr r1, r1, #0x44
    str r1, [r0]
    ldr r1, [r0]
    dsb
    ldr r0, =0x58020818
    ldr r1, =0x00C00000
    str r1, [r0]
    ldr r0, =0x58021818
    ldr r1, =0x08000000
    str r1, [r0]
    ldr r0, =0x58020804
    ldr r1, [r0]
    bic r1, r1, #0xC0
    str r1, [r0]
    ldr r0, =0x58020800
    ldr r1, [r0]
    bic r1, r1, #0xF000
    orr r1, r1, #0x5000
    str r1, [r0]
    ldr r0, =0x58021804
    ldr r1, [r0]
    bic r1, r1, #0x800
    str r1, [r0]
    ldr r0, =0x58021800
    ldr r1, [r0]
    bic r1, r1, #0xC00000
    orr r1, r1, #0x400000
    str r1, [r0]
    dsb
#endif
    ldr r0, =0xE000ED88
    ldr r1, [r0]
    orr r1, r1, #0xF00000
    str r1, [r0]
    dsb
    isb
    ldr r0, =_sidata
    ldr r1, =_sdata
    ldr r2, =_edata
1:  cmp r1, r2
    bhs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:  ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
3:  cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b
4:  ldr r0, =0xE000ED08
    ldr r1, =boot_vectors
    str r1, [r0]
    dsb
    isb
    bl boot_main
    b Fault_Handler
.thumb_func
Fault_Handler:
    /* 异常停在Boot，B1系统电源保持早期关断；不继续跳转。 */
    b Fault_Handler
