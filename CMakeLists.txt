cmake_minimum_required(VERSION 3.22)

project(b1 C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(HAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver)
set(HAL_SOURCES
    stm32h7xx_hal.c stm32h7xx_hal_cortex.c stm32h7xx_hal_dma.c
    stm32h7xx_hal_dma_ex.c stm32h7xx_hal_exti.c stm32h7xx_hal_flash.c
    stm32h7xx_hal_flash_ex.c stm32h7xx_hal_gpio.c stm32h7xx_hal_i2c.c
    stm32h7xx_hal_i2c_ex.c stm32h7xx_hal_mdma.c stm32h7xx_hal_pwr.c stm32h7xx_hal_pwr_ex.c
    stm32h7xx_hal_qspi.c stm32h7xx_hal_rcc.c stm32h7xx_hal_rcc_ex.c
    stm32h7xx_hal_spi.c stm32h7xx_hal_spi_ex.c stm32h7xx_hal_uart.c
    stm32h7xx_hal_uart_ex.c
)
list(TRANSFORM HAL_SOURCES PREPEND "${HAL_DIR}/Src/")

add_executable(b1
    Core/Src/main.c
    Core/Src/board_identity.c
    Core/Src/stm32h7xx_it.c
    Core/Src/system_stm32h7xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    startup_stm32h743xx.s
    ${HAL_SOURCES}
)

target_include_directories(b1 PRIVATE
    Core/Inc
    ${HAL_DIR}/Inc
    Drivers/CMSIS/Include
    Drivers/CMSIS/Device/ST/STM32H7xx/Include
)
target_compile_definitions(b1 PRIVATE USE_HAL_DRIVER STM32H743xx USE_PWR_LDO_SUPPLY)
target_link_libraries(b1 m)
target_link_options(b1 PRIVATE -Wl,-Map=b1.map)
set_target_properties(b1 PROPERTIES ADDITIONAL_CLEAN_FILES b1.map)

set(BOOT_OBJ ${CMAKE_BINARY_DIR}/boot.s.o)
set(BOOT_ELF ${CMAKE_BINARY_DIR}/boot.elf)
set(BOOT_FLAGS -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16)
add_custom_command(
    OUTPUT ${BOOT_OBJ}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS} -x assembler-with-cpp
            -c ${CMAKE_CURRENT_SOURCE_DIR}/boot/boot.s -o ${BOOT_OBJ}
    DEPENDS boot/boot.s
)
add_custom_command(
    OUTPUT ${BOOT_ELF}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS} -nostartfiles -nostdlib
            -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_boot.ld
            ${BOOT_OBJ} -o ${BOOT_ELF}
    DEPENDS ${BOOT_OBJ} boot/STM32H743_boot.ld
)
add_custom_target(boot ALL DEPENDS ${BOOT_ELF})
