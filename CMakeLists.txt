cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME test)

# 仅用于阿波罗 V2 逻辑分析仪验收；默认关闭，正式镜像绝不跳过 DDR 器件访问。
option(ENABLE_PWR_WAVE_TEST "Build Apollo V2 pwr_ctrl waveform image" OFF)

# 仅用于阿波罗 V2 六路结果灯逻辑分析仪验收；正式镜像默认关闭。
option(ENABLE_RESULT_LED_WAVE_TEST "Build Apollo V2 result LED waveform image" OFF)

# 仅用于阿波罗 V2 板载 W25Q256；所有破坏性访问限制在末尾 64 KiB。
option(ENABLE_FLASH_SELF_TEST "Build isolated Apollo V2 W25Q256 self-test image" OFF)

# 无外接TPS/PAC/AD/MCP器件时，运行正式驱动源码的事务级Mock验收镜像。
option(ENABLE_DRIVER_MOCK_TEST "Build isolated TPS/PAC/AD/MCP mock self-test image" OFF)

# 仅用于阿波罗 V2 实脚 SPI2 波形验收；循环输出 AD7124 典型事务。
option(ENABLE_AD7124_WAVE_TEST "Build isolated Apollo V2 AD7124 SPI waveform image" OFF)

# 仅用于阿波罗 V2 真实以太网验收PC固定头+cJSON；合法帧原样回显，不定义业务语义。
option(ENABLE_PC_PROTOCOL_NET_TEST "Build isolated PC protocol TCP echo test image" OFF)

# 固件身份由构建系统统一提供，禁止在 C 源文件中散落手写版本字符串。
set(FIRMWARE_VERSION_MAJOR 0 CACHE STRING "Firmware semantic version major")
set(FIRMWARE_VERSION_MINOR 1 CACHE STRING "Firmware semantic version minor")
set(FIRMWARE_VERSION_PATCH 0 CACHE STRING "Firmware semantic version patch")
set(FIRMWARE_BUILD_NUMBER 0 CACHE STRING "CI or release build number")

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 版本字符串使用配置时刻和源码状态。重新配置即可刷新；打包清单与固件共用这些值。
string(TIMESTAMP FIRMWARE_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE FIRMWARE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE FIRMWARE_GIT_RESULT
)
if(NOT FIRMWARE_GIT_RESULT EQUAL 0)
    set(FIRMWARE_GIT_COMMIT "unknown")
endif()
execute_process(
    COMMAND git status --porcelain
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE FIRMWARE_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(FIRMWARE_GIT_STATUS STREQUAL "")
    set(FIRMWARE_GIT_DIRTY 0)
else()
    set(FIRMWARE_GIT_DIRTY 1)
endif()

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/uart_router.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/board_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/uart_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/i2c_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/memory_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/bsp_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/ddr_power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/pwr_ctrl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/pwm_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/qspi_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/spi_hw.c
    # PC-MCU 与 MCU-RK 是两套独立协议，源码目录必须保持物理隔离。
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_test_message.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/state_machine.c
    # 正式任务按通信对象分区，纯验证任务统一放test目录。
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/pc/tcp_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk/hb_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk/rk_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common/led_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common/monitor_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/pwr_wave_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/result_led_wave_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/qspi_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/test_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/AD7124/ad7124.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_layout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/IIC/myiic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/LED/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/MCP4725/mcp4725.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PCF8574/pcf8574.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PAC1934/pac1934.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/TPS62868/tps62868.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/WDT/wdt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/cJSON/cJSON.c
)

# Mock任务体只进入隔离测试镜像，正式固件不编译测试代码。
if(ENABLE_DRIVER_MOCK_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/driver_mock_test.c
    )
endif()

# 实脚SPI波形任务同样只进入专用镜像，正式固件不携带测试入口。
if(ENABLE_AD7124_WAVE_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/ad7124_wave_task.c
    )
endif()

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/pc
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/AD7124
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/IIC
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/LED
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/MCP4725
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PCF8574
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PAC1934
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/TPS62868
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/WDT
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/cJSON
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    $<$<BOOL:${ENABLE_PWR_WAVE_TEST}>:ENABLE_PWR_WAVE_TEST=1>
    $<$<BOOL:${ENABLE_RESULT_LED_WAVE_TEST}>:ENABLE_RESULT_LED_WAVE_TEST=1>
    $<$<BOOL:${ENABLE_FLASH_SELF_TEST}>:ENABLE_FLASH_SELF_TEST=1>
    $<$<BOOL:${ENABLE_DRIVER_MOCK_TEST}>:ENABLE_DRIVER_MOCK_TEST=1>
    $<$<BOOL:${ENABLE_AD7124_WAVE_TEST}>:ENABLE_AD7124_WAVE_TEST=1>
    $<$<BOOL:${ENABLE_PC_PROTOCOL_NET_TEST}>:ENABLE_PC_PROTOCOL_NET_TEST=1>
    # MCU 接收的 JSON 只承载扁平业务对象，限制递归深度可抵御畸形嵌套输入。
    CJSON_NESTING_LIMIT=16
)

# 版本宏只影响 flash_layout.c，刷新构建时间时无需重编译整个固件。
set_property(SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_layout.c
    APPEND PROPERTY COMPILE_DEFINITIONS
    FIRMWARE_VERSION_MAJOR=${FIRMWARE_VERSION_MAJOR}
    FIRMWARE_VERSION_MINOR=${FIRMWARE_VERSION_MINOR}
    FIRMWARE_VERSION_PATCH=${FIRMWARE_VERSION_PATCH}
    FIRMWARE_BUILD_NUMBER=${FIRMWARE_BUILD_NUMBER}
    FIRMWARE_BUILD_TIME_UTC="${FIRMWARE_BUILD_TIME_UTC}"
    FIRMWARE_GIT_COMMIT="${FIRMWARE_GIT_COMMIT}"
    FIRMWARE_GIT_DIRTY=${FIRMWARE_GIT_DIRTY}
    FIRMWARE_BUILD_ID="${CMAKE_BUILD_TYPE}"
    FIRMWARE_TARGET="apollo-v2-h743"
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# 生成精确的裸镜像、CRC32 清单和 96 字节版本载荷；不向固件中加入 OTA 行为。
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
            ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_firmware_manifest.py
            --image ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin
            --json ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.manifest.json
            --version-payload ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.version.bin
            --version "${FIRMWARE_VERSION_MAJOR}.${FIRMWARE_VERSION_MINOR}.${FIRMWARE_VERSION_PATCH}"
            --build-number "${FIRMWARE_BUILD_NUMBER}"
            --build-time "${FIRMWARE_BUILD_TIME_UTC}"
            --git-commit "${FIRMWARE_GIT_COMMIT}"
            --git-dirty "${FIRMWARE_GIT_DIRTY}"
            --build-id "${CMAKE_BUILD_TYPE}"
            --target "apollo-v2-h743"
    COMMENT "Generating firmware binary, CRC32 manifest and W25Q256 version payload"
    VERBATIM
)

# ===================================================================
# Bootloader — 最小跳转 (0x08000000, 128KB)
# 烧录顺序: boot.elf 先, test.elf 后
# 用显式命令构建, 绕开 CMake executable 规则 (全局 -T/编译flags 会泄漏)
# ===================================================================
set(BOOT_OBJ ${CMAKE_BINARY_DIR}/boot.s.o)
set(BOOT_ELF ${CMAKE_BINARY_DIR}/boot.elf)
set(BOOT_FLAGS -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-d16)

add_custom_command(
    OUTPUT ${BOOT_OBJ}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS} -x assembler-with-cpp
            -c ${CMAKE_CURRENT_SOURCE_DIR}/boot/boot.s -o ${BOOT_OBJ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/boot/boot.s
    COMMENT "Assembling boot/boot.s"
    VERBATIM
)

add_custom_command(
    OUTPUT ${BOOT_ELF}
    COMMAND ${CMAKE_C_COMPILER} ${BOOT_FLAGS}
            -nostartfiles -nostdlib
            -T ${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_boot.ld
            ${BOOT_OBJ} -o ${BOOT_ELF}
    DEPENDS ${BOOT_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/boot/STM32H743_boot.ld
    COMMENT "Linking boot.elf"
    VERBATIM
)

add_custom_target(boot ALL DEPENDS ${BOOT_ELF})
