# 独立构建Boot，再将其作为固定地址段装入最终ELF；每个工程均能独立构建。
set(BOOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boot)
set(BOOT_OUT ${CMAKE_BINARY_DIR}/minimal-boot)
set(BOOT_BOARD_FLAG)
if(CMAKE_PROJECT_NAME STREQUAL "b1")
    set(BOOT_BOARD_FLAG -DBOOT_BOARD_B1=1 -DB1_RK_UART=4)
endif()
# 隔离Flash自检沿用末尾64KiB地址，Boot也不得写正式FRU扇区。
if(ENABLE_FLASH_SELF_TEST)
    list(APPEND BOOT_BOARD_FLAG -DENABLE_FLASH_SELF_TEST=1)
endif()
set(BOOT_SOURCES ${BOOT_DIR}/startup.S ${BOOT_DIR}/boot_main.c
    ${BOOT_DIR}/boot_port.c ${BOOT_DIR}/boot_fru.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_layout.c)
add_custom_command(OUTPUT ${BOOT_OUT}/boot_image.S
    BYPRODUCTS ${BOOT_OUT}/boot.elf ${BOOT_OUT}/boot.bin ${BOOT_OUT}/boot.map
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BOOT_OUT}
    COMMAND ${CMAKE_C_COMPILER} -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
        -Os -g3 -Wall -Wextra -Werror -std=c11 -ffunction-sections -fdata-sections
        -DSTM32H743xx ${BOOT_BOARD_FLAG}
        -I${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc -I${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv
        -I${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash
        -I${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Include
        -I${CMAKE_CURRENT_SOURCE_DIR}/Drivers/CMSIS/Device/ST/STM32H7xx/Include
        ${BOOT_SOURCES} -nostartfiles --specs=nano.specs --specs=nosys.specs
        -T${BOOT_DIR}/minimal_boot.ld -Wl,--gc-sections,-Map=${BOOT_OUT}/boot.map
        -o ${BOOT_OUT}/boot.elf
    COMMAND ${CMAKE_OBJCOPY} -O binary ${BOOT_OUT}/boot.elf ${BOOT_OUT}/boot.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_boot.py
        ${BOOT_OUT}/boot.bin ${BOOT_OUT}/boot_image.S
    DEPENDS ${BOOT_SOURCES} ${BOOT_DIR}/minimal_boot.ld ${BOOT_DIR}/boot_port.h
        ${BOOT_DIR}/boot_fru.h ${BOOT_DIR}/boot_vectors.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/board_storage_config.h
        ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_layout.h
        ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/hal_if.h
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_boot.py
    COMMENT "构建精简Boot及首次FRU初始化"
    VERBATIM)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${BOOT_OUT}/boot_image.S)
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/STM32H743xx_FLASH.ld)
