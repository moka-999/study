/* B的C运行环境、异常入口与最终跳转。不初始化App硬件，也不启动RTOS。 */
.syntax unified
.cpu cortex-m7
.thumb
.section .isr_vector,"a",%progbits
.global boot_vectors
boot_vectors:
.word 0x20020000
.word Reset_Handler
.rept 164
.word Fault_Handler
.endr

.section .text.startup,"ax",%progbits
.thumb_func
.global Reset_Handler
Reset_Handler:
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
2:  ldr r0, =_siram
    ldr r1, =_sram
    ldr r2, =_eram
3:  cmp r1, r2
    bhs 4f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 3b
4:  ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
5:  cmp r0, r1
    bhs 6f
    str r2, [r0], #4
    b 5b
6:  /* 同Bank擦写期间异常向量也必须从RAM读取。 */
    ldr r0, =boot_vectors
    ldr r1, =ram_vectors
    movw r2, #664
7:  subs r2, #4
    ldr r3, [r0, r2]
    str r3, [r1, r2]
    bne 7b
    ldr r0, =0xE000ED08
    str r1, [r0]
    dsb
    isb
    cpsie i
    bl boot_main
    b .

.section .ramfunc,"ax",%progbits
.thumb_func
Fault_Handler:
    tst lr, #4
    ite eq
    mrseq r0, msp
    mrsne r0, psp
    /* 异常压栈若含浮点扩展，核心寄存器帧位于其后18个字。 */
    tst lr, #16
    it eq
    addeq r0, r0, #72
    b boot_fault

.thumb_func
.global boot_guarded_load
.global boot_load_instruction
.global boot_load_resume
boot_guarded_load:
boot_load_instruction:
    ldr r0, [r0]
boot_load_resume:
    bx lr

.thumb_func
.global boot_jump_tail
boot_jump_tail:
    ldr r1, [r0]
    ldr r2, [r0, #4]
    ldr r3, =0xE000ED08
    str r0, [r3]
    movs r0, #0
    msr control, r0
    msr basepri, r0
    msr faultmask, r0
    msr msp, r1
    dsb
    isb
    cpsie i
    bx r2

.section .ram_vectors,"aw",%nobits
.balign 1024
ram_vectors: .space 1024
