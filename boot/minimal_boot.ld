/* 精简Boot独立链接，不链接App、RTOS或升级安装器。 */
ENTRY(Boot_Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } >FLASH
    .text : { *(.text*) *(.rodata*) *(.ARM.extab*) } >FLASH
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >RAM AT>FLASH
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) : { . = ALIGN(8); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(8); _ebss = .; } >RAM
    ASSERT(_ebss <= 0x2001C000, "Boot必须保留至少16KiB栈")
    /DISCARD/ : { *(.comment) *(.note*) }
}
