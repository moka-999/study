cmake_minimum_required(VERSION 3.22)
project(bootloader LANGUAGES C ASM)

# 同一安装核心，按板型生成独立B；B1复位先关RK/DDR，包身份不能由PC指定。
set(BOOT_BOARD "apollo-v2" CACHE STRING "Boot board: apollo-v2 or b1")
set_property(CACHE BOOT_BOARD PROPERTY STRINGS apollo-v2 b1)
if(NOT BOOT_BOARD STREQUAL "apollo-v2" AND NOT BOOT_BOARD STREQUAL "b1")
    message(FATAL_ERROR "Unknown BOOT_BOARD")
endif()

# B工程不携带App、FreeRTOS、LwIP或测试仪业务。
option(ENABLE_OTA_RELOCATION_PROBE "Build fixed-slot startup probe, not OTA installer" OFF)
set(OTA_APP_BASE "0x08020000" CACHE STRING "App address for isolated startup probe")
set(BOOT_FLAGS -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16)
if(ENABLE_OTA_RELOCATION_PROBE)
    if(BOOT_BOARD STREQUAL "b1")
        message(FATAL_ERROR "B1 uses full safe-off Boot, not V2 fixed-slot probe")
    endif()
    if(NOT OTA_APP_BASE MATCHES "^(0x08020000|0x08100000)$")
        message(FATAL_ERROR "Startup probe only supports A1 or A2")
    endif()
    list(APPEND BOOT_FLAGS -DOTA_BOOT_APP_BASE=${OTA_APP_BASE})
endif()

# 显式沿用原GNU汇编/链接参数，避免App的链接偏移与第三方库泄漏到B。
set(BOOT_ELF ${CMAKE_BINARY_DIR}/boot.elf)
set(BOOT_BIN ${CMAKE_BINARY_DIR}/boot.bin)
set(BOOT_OBJ ${CMAKE_BINARY_DIR}/boot.s.o)
if(ENABLE_OTA_RELOCATION_PROBE)
add_custom_command(
    OUTPUT ${BOOT_ELF} ${BOOT_BIN}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS} -x assembler-with-cpp
            -c ${CMAKE_CURRENT_SOURCE_DIR}/boot/boot.s -o ${BOOT_OBJ}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS} -nostartfiles -nostdlib
            -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_boot.ld
            -Wl,-Map=${CMAKE_BINARY_DIR}/boot.map
            ${BOOT_OBJ} -o ${BOOT_ELF}
    COMMAND ${CMAKE_OBJCOPY} -O binary ${BOOT_ELF} ${BOOT_BIN}
    COMMAND ${CMAKE_SIZE} ${BOOT_ELF}
    DEPENDS boot/boot.s boot/STM32H743_boot.ld
    BYPRODUCTS ${BOOT_OBJ} ${CMAKE_BINARY_DIR}/boot.map
    COMMENT "Building standalone B image"
    VERBATIM
)
add_custom_target(boot_probe DEPENDS ${BOOT_ELF} ${BOOT_BIN})
endif()

# 安装、日志及健康判定保持独立编译，正式B和离线测试使用相同源码。
add_library(ota_core STATIC boot/ota_install.c boot/boot_record.c boot/boot_health.c)
target_include_directories(ota_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/boot)
target_compile_features(ota_core PUBLIC c_std_11)
target_compile_options(ota_core PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Wconversion
    -Wshadow -Werror -ffunction-sections -fdata-sections -fstack-usage)

# 正式B使用独立C启动、持久化日志、受限NOR/内部Flash端口；固定槽探针单独保留。
if(ENABLE_OTA_RELOCATION_PROBE)
    add_custom_target(boot ALL DEPENDS boot_probe)
else()
    add_executable(boot boot/startup.s boot/boot_main.c boot/boot_port.c)
    if(BOOT_BOARD STREQUAL "b1")
        target_compile_definitions(boot PRIVATE BOOT_BOARD_B1=1)
    endif()
    set_source_files_properties(boot/startup.s PROPERTIES COMPILE_OPTIONS "-x;assembler-with-cpp")
    target_include_directories(boot PRIVATE vendor/cmsis vendor/stm32)
    target_link_libraries(boot PRIVATE ota_core c gcc)
    target_compile_options(boot PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Wshadow -Werror
        -ffunction-sections -fdata-sections -fstack-usage)
    target_link_options(boot PRIVATE ${BOOT_FLAGS} -nostartfiles --specs=nosys.specs
        -T${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_ota.ld -Wl,--gc-sections
        -Wl,-Map=${CMAKE_BINARY_DIR}/boot.map)
    set_target_properties(boot PROPERTIES SUFFIX ".elf"
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_ota.ld)
    add_custom_command(TARGET boot POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:boot> ${BOOT_BIN}
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:boot>)
endif()

# 只在显式离线测试构建启用；MPS2测试ELF不是可烧录的STM32镜像。
option(ENABLE_BOOT_RAM_AGENT "Build explicit SWD-only RAM backup/deployment helper" OFF)
if(ENABLE_BOOT_RAM_AGENT)
    add_executable(boot_ram_agent boot/startup.s boot/boot_port.c tests/ram_agent.c)
    target_include_directories(boot_ram_agent PRIVATE vendor/cmsis vendor/stm32)
    target_compile_definitions(boot_ram_agent PRIVATE BOOT_RAM_AGENT=1)
    if(BOOT_BOARD STREQUAL "b1")
        target_compile_definitions(boot_ram_agent PRIVATE BOOT_BOARD_B1=1)
    endif()
    target_compile_options(boot_ram_agent PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
    target_link_libraries(boot_ram_agent PRIVATE ota_core c gcc)
    target_link_options(boot_ram_agent PRIVATE ${BOOT_FLAGS} -nostartfiles --specs=nosys.specs
        -T${CMAKE_CURRENT_SOURCE_DIR}/tests/ram_agent.ld -Wl,--gc-sections)
    set_target_properties(boot_ram_agent PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/ram_agent.ld)
endif()

option(ENABLE_OTA_CORE_TEST "Build offline Cortex-M7 installer tests" OFF)
if(ENABLE_OTA_CORE_TEST)
    foreach(manager IN ITEMS boot_main_test boot_main_b1_test)
        add_executable(${manager} tests/test_boot_main.c boot/boot_main.c tests/qemu_start.s)
        target_link_libraries(${manager} PRIVATE ota_core c gcc)
        target_compile_options(${manager} PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Werror)
        target_link_options(${manager} PRIVATE ${BOOT_FLAGS} -nostartfiles --specs=nosys.specs
            -T${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_test.ld -Wl,--gc-sections)
        set_target_properties(${manager} PROPERTIES SUFFIX ".elf")
    endforeach()
    target_compile_definitions(boot_main_b1_test PRIVATE BOOT_BOARD_B1=1)
    add_executable(boot_record_test tests/test_boot_record.c tests/qemu_start.s)
    target_link_libraries(boot_record_test PRIVATE ota_core c gcc)
    target_compile_options(boot_record_test PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Werror)
    target_link_options(boot_record_test PRIVATE ${BOOT_FLAGS} -nostartfiles --specs=nosys.specs
        -T${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_test.ld -Wl,--gc-sections)
    set_target_properties(boot_record_test PROPERTIES SUFFIX ".elf")
    add_executable(ota_core_test tests/test_ota_install.c tests/qemu_start.s)
    target_link_libraries(ota_core_test PRIVATE ota_core c gcc)
    target_compile_options(ota_core_test PRIVATE ${BOOT_FLAGS} -Wall -Wextra -Werror
        -ffunction-sections -fdata-sections)
    target_link_options(ota_core_test PRIVATE ${BOOT_FLAGS} -nostartfiles --specs=nosys.specs
        -T${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_test.ld -Wl,--gc-sections
        -Wl,-Map=${CMAKE_BINARY_DIR}/ota_core_test.map)
    set_target_properties(ota_core_test PROPERTIES SUFFIX ".elf"
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/qemu_test.ld)
endif()
