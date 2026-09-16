/* SWD维护ELF仅占RAM，不能改成内部Flash装载地址。 */
ENTRY(Reset_Handler)
MEMORY {
    AXI (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
    DTCM (rw) : ORIGIN = 0x20000000, LENGTH = 128K
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } >AXI
    .text : { *(.text*) *(.rodata*) *(.ARM.extab*) . = ALIGN(4); } >AXI
    .ARM.exidx : { *(.ARM.exidx*) } >AXI
    .ramfunc : { . = ALIGN(4); _sram = .; *(.ramfunc*) . = ALIGN(4); _eram = .; } >AXI
    _siram = LOADADDR(.ramfunc);
    .ram_vectors (NOLOAD) : { *(.ram_vectors) } >AXI
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >DTCM AT>AXI
    _sidata = LOADADDR(.data);
    .bss (NOLOAD) : { . = ALIGN(32); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(8); _ebss = .; } >DTCM
    ASSERT(_ebss < 0x2001C000, "RAM agent requires 16KiB stack headroom")
}
