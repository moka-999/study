/* B独占128KiB；同Bank擦写代码放AXI RAM，数据/栈放DTCM，无堆。 */
ENTRY(Reset_Handler)
MEMORY {
    FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
    AXI (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } >FLASH
    .text : { *(.text*) *(.rodata*) *(.ARM.extab*) . = ALIGN(4); } >FLASH
    .ARM.exidx : { *(.ARM.exidx*) } >FLASH
    .ramfunc : { . = ALIGN(4); _sram = .; *(.ramfunc*) . = ALIGN(4); _eram = .; } >AXI AT>FLASH
    _siram = LOADADDR(.ramfunc);
    .ram_vectors (NOLOAD) : { *(.ram_vectors) } >AXI
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >DTCM AT>FLASH
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) : { . = ALIGN(8); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(8); _ebss = .; } >DTCM
    ASSERT(_ebss < 0x2001C000, "B requires 16KiB stack headroom")
    ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x08020000, "B exceeds reserved sector")
}
