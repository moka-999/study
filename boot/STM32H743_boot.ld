/*
 * Bootloader 链接脚本 — STM32H743, 0x08000000, 128KB
 * 无 .data/.bss (纯汇编, 零 C 运行时)
 */
ENTRY(Reset_Handler)

MEMORY
{
FLASH (rx)      : ORIGIN = 0x08000000, LENGTH = 128K
}

/* 栈顶: DTCMRAM 顶端 (与 App 一致, 跳转后由 App 向量表接管) */
_estack = 0x20020000;

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    . = ALIGN(4);
  } >FLASH
}
