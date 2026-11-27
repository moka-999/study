/* QEMU MPS2-AN500离线测试内存布局，不得用于STM32烧录镜像。 */
ENTRY(test_reset)
MEMORY {
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rw) : ORIGIN = 0x20000000, LENGTH = 4M
}
SECTIONS {
    .isr_vector : { KEEP(*(.isr_vector)) } >ROM
    .text : { *(.text*) *(.rodata*) *(.ARM.extab*) } >ROM
    .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >ROM
    .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } >RAM AT>ROM
    _sidata = LOADADDR(.data);
    .idd_capture (NOLOAD) : { *(.idd_capture) } >RAM
    .uart_rx_ring (NOLOAD) : {
        . = ALIGN(4); __uart_rx_ring_start = .;
        *(.uart_rx_ring)
        __uart_rx_ring_end = .;
    } >RAM
    .bss (NOLOAD) : {
        . = ALIGN(8); _sbss = .;
        *(.bss*) *(COMMON)
        . = ALIGN(8); _ebss = .;
    } >RAM
    ASSERT(_ebss < 0x203FC000, "测试栈至少保留16KiB")
}
