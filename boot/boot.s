/*
 * ===================================================================
 * Minimal jump Bootloader - STM32H743 (0x08000000, 128KB sector)
 * ===================================================================
 *
 * Job: validate App vector table, then jump to App (0x08020000).
 * NO upgrade logic here (W25Q256 copy etc. comes later, after B2
 * bring-up is done).
 *
 * Validation (prevents jumping into blank flash):
 *   1. App initial SP in [0x20000000, 0x24080000)  (RAM range)
 *   2. App Reset_Handler in [0x08020000, 0x08200000) and Thumb
 *
 * Before jump: interrupts remain off; only GPIOC/G clocks and the three safety
 * EN pins are initialized. VTOR is set by App's own SystemInit.
 * ===================================================================
 */
    .syntax unified
    .cpu    cortex-m7
    .thumb

/* App region constants */
    .equ    APP_BASE,    0x08020000
    .equ    APP_END,     0x08200000
    .equ    RAM_LO,      0x20000000
    .equ    RAM_HI,      0x24080000

/*
 * B1受控电源GPIO。系统已确认采用“耦合复位”语义：任何MCU复位都必须
 * 立即关闭RK和DDR受控电源，之后只允许PC通过正式上电流程重新开启。
 * PC6/PC7是真实使能，PC6关闭4V后TPS随之掉电；当前BRD的PG11路径为NC，
 * 仍在校验App之前保持低，避免未来误装0R时产生意外使能。
 */
    .equ    RCC_AHB4ENR,            0x580244E0
    .equ    RCC_GPIOC_GPIOG_EN,     0x00000044
    .equ    GPIOC_MODER,            0x58020800
    .equ    GPIOC_OTYPER,           0x58020804
    .equ    GPIOC_BSRR,             0x58020818
    .equ    GPIOG_MODER,            0x58021800
    .equ    GPIOG_OTYPER,           0x58021804
    .equ    GPIOG_BSRR,             0x58021818
    .equ    GPIOC_EN_RESET,         0x00C00000  /* PC6/PC7 BSRR reset */
    .equ    GPIOG_EN_RESET,         0x08000000  /* PG11 BSRR reset */
    .equ    GPIOC_OTYPE_MASK,       0x000000C0
    .equ    GPIOG_OTYPE_MASK,       0x00000800
    .equ    GPIOC_MODE_MASK,        0x0000F000
    .equ    GPIOC_MODE_OUTPUT,      0x00005000
    .equ    GPIOG_MODE_MASK,        0x00C00000
    .equ    GPIOG_MODE_OUTPUT,      0x00400000

/* Boot's own vector table (SP + Reset_Handler only) */
    .section .isr_vector,"a",%progbits
    .word   _estack
    .word   Reset_Handler

/* Reset entry */
    .section .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    /* 先开GPIOC/G时钟；读回和DSB保证后续GPIO写入不会越过时钟使能。 */
    ldr     r0, =RCC_AHB4ENR
    ldr     r1, [r0]
    ldr     r2, =RCC_GPIOC_GPIOG_EN
    orr     r1, r1, r2
    str     r1, [r0]
    ldr     r1, [r0]
    dsb

    /* 先写低电平锁存值，再切换输出模式，避免模式切换时产生高脉冲。 */
    ldr     r0, =GPIOC_BSRR
    ldr     r1, =GPIOC_EN_RESET
    str     r1, [r0]
    ldr     r0, =GPIOG_BSRR
    ldr     r1, =GPIOG_EN_RESET
    str     r1, [r0]

    /* PC6是真正的4V/TPS总开关，优先切为输出低；PG11随后保持低。 */
    ldr     r0, =GPIOC_OTYPER
    ldr     r1, [r0]
    ldr     r2, =GPIOC_OTYPE_MASK
    bic     r1, r1, r2
    str     r1, [r0]
    ldr     r0, =GPIOC_MODER
    ldr     r1, [r0]
    ldr     r2, =GPIOC_MODE_MASK
    bic     r1, r1, r2
    ldr     r2, =GPIOC_MODE_OUTPUT
    orr     r1, r1, r2
    str     r1, [r0]

    ldr     r0, =GPIOG_OTYPER
    ldr     r1, [r0]
    ldr     r2, =GPIOG_OTYPE_MASK
    bic     r1, r1, r2
    str     r1, [r0]
    ldr     r0, =GPIOG_MODER
    ldr     r1, [r0]
    ldr     r2, =GPIOG_MODE_MASK
    bic     r1, r1, r2
    ldr     r2, =GPIOG_MODE_OUTPUT
    orr     r1, r1, r2
    str     r1, [r0]
    dsb

    ldr     r0, =APP_BASE          /* r0 = App vector table addr    */
    ldr     r1, [r0]               /* r1 = App initial SP           */
    ldr     r2, =RAM_LO
    cmp     r1, r2
    blo     .Lfail                 /* SP < RAM_LO -> invalid        */
    ldr     r2, =RAM_HI
    cmp     r1, r2
    bhs     .Lfail                 /* SP >= RAM_HI -> invalid       */

    ldr     r2, [r0, #4]           /* r2 = App Reset_Handler        */
    tst     r2, #1
    beq     .Lfail                 /* even address -> not Thumb     */
    bic     r2, r2, #1             /* clear Thumb bit               */
    ldr     r3, =APP_BASE
    cmp     r2, r3
    blo     .Lfail                 /* PC < APP_BASE -> invalid      */
    ldr     r3, =APP_END
    cmp     r2, r3
    bhs     .Lfail                 /* PC >= APP_END -> invalid      */

    msr     msp, r1                /* switch to App stack           */
    ldr     r2, [r0, #4]
    bx      r2                     /* jump to App (Thumb)           */

.Lfail:
    b       .Lfail                 /* invalid: stall (LED later)    */

    .ltorg
    .end
