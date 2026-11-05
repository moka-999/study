cmake_minimum_required(VERSION 3.22)

project(b1 C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# B1只使用真实驱动和真实算法结果；拒绝V2的模拟电源、隔离网关及MATS假FAIL。
if(ENABLE_V2_POWER_SIMULATION OR ENABLE_RK_GATEWAY_TEST OR ENABLE_V2_MATS_FAIL_TEST)
    message(FATAL_ERROR "B1 does not support V2 power simulation, gateway test or MATS fake failure")
endif()

# UART4固定运行RK协议；保留变量名供共用代码识别B1，拒绝旧USART2配置。
set(B1_RK_UART 4 CACHE STRING "B1 protocol UART: fixed UART4")
set_property(CACHE B1_RK_UART PROPERTY STRINGS 4)
if(NOT B1_RK_UART STREQUAL "4")
    message(FATAL_ERROR "B1_RK_UART must be 4; USART2 is reserved for RK debug logs")
endif()

# 仅决定每次上电的默认日志来源；串口命令可临时切换，不写Flash。
set(B1_LOG_DEFAULT_SOURCE MCU CACHE STRING "USART1 default log source: MCU or RK")
set_property(CACHE B1_LOG_DEFAULT_SOURCE PROPERTY STRINGS MCU RK)
if(B1_LOG_DEFAULT_SOURCE STREQUAL "MCU")
    set(B1_LOG_DEFAULT_VALUE 0)
elseif(B1_LOG_DEFAULT_SOURCE STREQUAL "RK")
    set(B1_LOG_DEFAULT_VALUE 1)
else()
    message(FATAL_ERROR "B1_LOG_DEFAULT_SOURCE must be MCU or RK")
endif()

# B2尚未通知编号时的本地调试地址；正式动态IP不受该构建参数限制。
set(B1_TEST_IP_LAST_OCTET 201 CACHE STRING "B1 local test IP last octet: 1..254")
if(NOT B1_TEST_IP_LAST_OCTET MATCHES "^[1-9][0-9]*$"
   OR B1_TEST_IP_LAST_OCTET LESS 1 OR B1_TEST_IP_LAST_OCTET GREATER 254)
    message(FATAL_ERROR "B1_TEST_IP_LAST_OCTET must be an integer from 1 to 254")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# B1版本身份由构建系统统一生成，保持与V2验收格式完全一致。
set(FIRMWARE_VERSION_MAJOR 0 CACHE STRING "Firmware semantic version major")
set(FIRMWARE_VERSION_MINOR 1 CACHE STRING "Firmware semantic version minor")
set(FIRMWARE_VERSION_PATCH 0 CACHE STRING "Firmware semantic version patch")
string(TIMESTAMP FIRMWARE_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

set(HAL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Drivers/STM32H7xx_HAL_Driver)
set(HAL_SOURCES
    stm32h7xx_hal.c stm32h7xx_hal_cortex.c stm32h7xx_hal_dma.c
    stm32h7xx_hal_dma_ex.c stm32h7xx_hal_exti.c stm32h7xx_hal_flash.c
    stm32h7xx_hal_flash_ex.c stm32h7xx_hal_gpio.c stm32h7xx_hal_i2c.c
    stm32h7xx_hal_i2c_ex.c stm32h7xx_hal_iwdg.c stm32h7xx_hal_mdma.c stm32h7xx_hal_pwr.c stm32h7xx_hal_pwr_ex.c
    stm32h7xx_hal_qspi.c stm32h7xx_hal_rcc.c stm32h7xx_hal_rcc_ex.c
    stm32h7xx_hal_spi.c stm32h7xx_hal_spi_ex.c stm32h7xx_hal_uart.c
    stm32h7xx_hal_uart_ex.c stm32h7xx_hal_eth.c stm32h7xx_hal_eth_ex.c
    stm32h7xx_hal_tim.c stm32h7xx_hal_tim_ex.c
)
list(TRANSFORM HAL_SOURCES PREPEND "${HAL_DIR}/Src/")

set(FREERTOS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/FreeRTOS/Source)
set(FREERTOS_SOURCES
    ${FREERTOS_DIR}/portable/Common/mpu_wrappers_v2.c
    ${FREERTOS_DIR}/croutine.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/CMSIS_RTOS_V2/cmsis_os2.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
)

set(LWIP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/LwIP)
# 只编译 B1 网关需要的 socket/TCP/IPv4 源；PPP、MQTT、IPv6 暂不进入固件。
file(GLOB LWIP_API_SOURCES CONFIGURE_DEPENDS ${LWIP_DIR}/src/api/*.c)
file(GLOB LWIP_CORE_SOURCES CONFIGURE_DEPENDS ${LWIP_DIR}/src/core/*.c)
file(GLOB LWIP_IPV4_SOURCES CONFIGURE_DEPENDS ${LWIP_DIR}/src/core/ipv4/*.c)
set(LWIP_SOURCES
    ${LWIP_API_SOURCES}
    ${LWIP_CORE_SOURCES}
    ${LWIP_IPV4_SOURCES}
    ${LWIP_DIR}/src/netif/ethernet.c
    ${LWIP_DIR}/system/OS/sys_arch.c
)

add_executable(b1
    Core/Src/main.c
    Core/Src/board_identity.c
    Core/Src/result_led.c
    Core/Src/log_console.c
    Core/Src/board_diagnostics.c
    Core/Src/freertos.c
    Core/Src/ethernet_board.c
    Core/Src/stm32h7xx_hal_timebase_tim.c
    LWIP/App/lwip.c
    LWIP/Target/ethernetif.c
    LWIP/Target/eth_custom_phy_interface.c
    HAL_Drv/qspi_hw.c
    HAL_Drv/i2c_hw.c
    HAL_Drv/spi_hw.c
    HAL_Drv/ddr_power.c
    HAL_Drv/pwr_ctrl.c
    HAL_Drv/memory_monitor.c
    HAL_Drv/idd_stats.c
    HAL_Drv/uart_hw.c
    HAL_Drv/uart_router.c
    # 两套协议/任务独立构建；不加入V2测试任务或旧PG12复位分发器。
    App/protocol/pc/pc_protocol.c
    App/protocol/pc/pc_state_machine.c
    App/protocol/pc/pc_dispatch.c
    App/protocol/pc/pc_tester_info.c
    App/protocol/pc/pc_test_message.c
    App/protocol/pc/pc_test_service.c
    App/protocol/pc/pc_power_service.c
    App/protocol/common/crc.c
    App/protocol/rk/frame.c
    App/protocol/rk/idd_session.c
    App/protocol/rk/state_machine.c
    App/tasks/pc/tcp_task.c
    App/tasks/rk/rk_task.c
    App/tasks/rk/rk_link.c
    App/tasks/common/monitor_task.c
    App/tasks/common/task_watchdog.c
    App/protocol/b2/b2_protocol.c
    App/protocol/b2/b2_state_machine.c
    App/tasks/b2/b2_service.c
    App/tasks/b2/b2_task.c
    HAL_Drv/b2_spi.c
    HAL_Drv/b2_board.c
    HAL_Drv/network_config.c
    BSP/AD7124/ad7124.c
    BSP/TPS62868/tps62868.c
    BSP/PAC1934/pac1934.c
    BSP/Flash/flash_bsp.c
    BSP/Flash/flash_layout.c
    BSP/MCP4725/mcp4725.c
    Middlewares/Third_Party/cJSON/cJSON.c
    Core/Src/stm32h7xx_it.c
    Core/Src/system_stm32h7xx.c
    Core/Src/syscalls.c
    Core/Src/sysmem.c
    startup_stm32h743xx.s
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
    ${LWIP_SOURCES}
)

target_include_directories(b1 PRIVATE
    Core/Inc
    HAL_Drv
    BSP/Flash
    BSP/AD7124
    BSP/TPS62868
    BSP/PAC1934
    BSP/MCP4725
    App/protocol/pc
    App/protocol/rk
    App/tasks
    App/tasks/rk
    App/tasks/common
    App/protocol/b2
    App/protocol/common
    App/tasks/b2
    Middlewares/Third_Party/cJSON
    LWIP/App
    LWIP/Target
    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/system
    ${LWIP_DIR}/system/arch
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/CMSIS_RTOS_V2
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
    Drivers/CMSIS/RTOS2/Include
    ${HAL_DIR}/Inc
    Drivers/CMSIS/Include
    Drivers/CMSIS/Device/ST/STM32H7xx/Include
)
target_compile_definitions(b1 PRIVATE
    USE_HAL_DRIVER
    STM32H743xx
    USE_PWR_LDO_SUPPLY
    # 与V2真实网络验收保持一致，限制畸形JSON的递归内存占用。
    CJSON_NESTING_LIMIT=16
    B1_RK_UART=${B1_RK_UART}
    B1_LOG_DEFAULT_SOURCE=${B1_LOG_DEFAULT_VALUE}
)

# IP宏仅作用于lwIP初始化文件，切换地址时不重编译无关驱动。
set_property(SOURCE LWIP/App/lwip.c APPEND PROPERTY COMPILE_DEFINITIONS
    B1_TEST_IP_LAST_OCTET=${B1_TEST_IP_LAST_OCTET}
)

# 版本宏只作用于持久化布局实现，避免字符串定义散落到业务代码。
set_property(SOURCE BSP/Flash/flash_layout.c APPEND PROPERTY COMPILE_DEFINITIONS
    FIRMWARE_VERSION_MAJOR=${FIRMWARE_VERSION_MAJOR}
    FIRMWARE_VERSION_MINOR=${FIRMWARE_VERSION_MINOR}
    FIRMWARE_VERSION_PATCH=${FIRMWARE_VERSION_PATCH}
    FIRMWARE_BUILD_TIME_UTC="${FIRMWARE_BUILD_TIME_UTC}"
    FIRMWARE_BUILD_ID="${CMAKE_BUILD_TYPE}"
    FIRMWARE_TARGET="b1-h743"
)
target_link_libraries(b1 m)
# cJSON用浮点格式化及回读校验输出小数；newlib-nano默认不链接这两项。
target_link_options(b1 PRIVATE -Wl,-Map=b1.map -Wl,-u,_printf_float -Wl,-u,_scanf_float)
set_target_properties(b1 PROPERTIES ADDITIONAL_CLEAN_FILES b1.map)

# 产出PC/量产工具可直接写入W25Q256的76字节版本载荷及校验清单。
add_custom_command(TARGET b1 POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:b1> ${CMAKE_BINARY_DIR}/b1.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_firmware_manifest.py
            --image ${CMAKE_BINARY_DIR}/b1.bin
            --json ${CMAKE_BINARY_DIR}/b1.manifest.json
            --version-payload ${CMAKE_BINARY_DIR}/b1.version.bin
            --version "${FIRMWARE_VERSION_MAJOR}.${FIRMWARE_VERSION_MINOR}.${FIRMWARE_VERSION_PATCH}"
            --build-time "${FIRMWARE_BUILD_TIME_UTC}"
            --build-id "${CMAKE_BUILD_TYPE}"
            --target "b1-h743"
    COMMENT "Generating B1 firmware CRC32 manifest and W25Q256 version payload"
    VERBATIM
)

# 最终ELF/BIN同时包含精简Boot和单App。
include(boot/minimal_boot.cmake)
