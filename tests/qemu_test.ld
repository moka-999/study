/* QEMU MPS2-AN500内存布局，绝不是STM32的正式Flash布局。
 * 输入由loader放到0x60000000，答案到0x60200000，测试配置到0x60400000。
 */
ENTRY(test_reset)
MEMORY {
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 128K
    RAM (rw) : ORIGIN = 0x20000000, LENGTH = 4M
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } >ROM
    .text : { *(.text*) *(.rodata*) *(.ARM.extab*) } >ROM
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >ROM
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >RAM AT>ROM
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) : {
        . = ALIGN(8); _sbss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8); _ebss = .;
    } >RAM
    ASSERT(_ebss < 0x203FC000, "Leave 16KiB test stack")
}
