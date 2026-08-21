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
 * Before jump: interrupts are off by reset; no peripheral init;
 * VTOR is set by App's own SystemInit.
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

/* Boot's own vector table (SP + Reset_Handler only) */
    .section .isr_vector,"a",%progbits
    .word   _estack
    .word   Reset_Handler

/* Reset entry */
    .section .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr     r0, =APP_BASE          /* r0 = App vector table addr    */
    ldr     r1, [r0]               /* r1 = App initial SP           */
    ldr     r2, =RAM_LO
    cmp     r1, r2
    blo     .Lfail                 /* SP < RAM_LO -> invalid        */
    ldr     r2, =RAM_HI
    cmp     r1, r2
    bhs     .Lfail                 /* SP >= RAM_HI -> invalid       */

    ldr     r2, [r0, #4]           /* r2 = App Reset_Handler        */
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
