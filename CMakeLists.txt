cmake_minimum_required(VERSION 3.22)

#
# 此文件仅首次生成，
# 后续运行转换器不会重新覆盖。
#
# 自定义源文件和构建配置统一在此维护。
#

# 配置C语言标准。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# 未明确指定时使用调试构建。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# App工程及默认产物名称。
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

# 阿波罗 V2 PC业务网络验收；沿用正式分发，隔离RK任务。
option(ENABLE_PC_PROTOCOL_NET_TEST "Build isolated PC protocol TCP business test image" OFF)

# 隔离全链路镜像保留正式任务监控，只模拟0103业务门禁；正式构建默认关闭。
# ON仅用于V2隔离联调：允许0103走模拟电源/ready路径并连接脚本扮演的RK。
# OFF走正式服务及其外部就绪/电源门禁；不是RK协议总开关，禁止量产打开。
option(ENABLE_RK_GATEWAY_TEST "Build temporary PC MCU RK gateway test image" OFF)

# V2沿用正式TCP/RK任务，仅电源和测量业务使用无硬件副作用的联调模型。
# 不替代真实RK算法；B1及量产构建禁用，只有显式选择才会编入。
option(ENABLE_V2_POWER_SIMULATION "Simulate V2 power and measurements with real RK" OFF)

# 仅供PC失败界面联调；收到真实MATS完整结果后覆盖PC视图，默认及B1构建均禁用。
option(ENABLE_V2_MATS_FAIL_TEST "V2联调：每次MATS结果上报FAIL" OFF)

# 固件身份由构建系统统一提供，禁止在 C 源文件中散落手写版本字符串。
set(FIRMWARE_VERSION_MAJOR 0 CACHE STRING "Firmware semantic version major")
set(FIRMWARE_VERSION_MINOR 1 CACHE STRING "Firmware semantic version minor")
set(FIRMWARE_VERSION_PATCH 0 CACHE STRING "Firmware semantic version patch")
set(B2_TEST_SITE_ID 1 CACHE STRING "V2 SPI联调槽位，取值1..8；B1使用实际GPIO")

# 导出编译命令，供clangd等索引工具使用。
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# 初始化工程与工具链。
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# 版本字符串使用配置时刻。重新配置即可刷新；打包清单与固件共用该值。
string(TIMESTAMP FIRMWARE_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)

# 启用汇编和C语言编译。
enable_language(C ASM)

# 创建App可执行目标。
add_executable(${CMAKE_PROJECT_NAME})
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE B2_TEST_SITE_ID=${B2_TEST_SITE_ID})

# 引入CubeMX生成的外设及第三方库目标。
add_subdirectory(cmake/stm32cubemx)

# 自定义库目录；没有额外目录时保持空列表。
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # 需要额外库搜索目录时在此补充。
)

# 将自定义源文件加入App目标。
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/b2/b2_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/b2/b2_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/b2/b2_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/b2/b2_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/b2_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/b2_board.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/network_config.c
    # 项目自有源文件。
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/uart_router.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/board_identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/uart_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/i2c_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/memory_monitor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/idd_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/bsp_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/ddr_power.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/pwr_ctrl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/pwm_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/qspi_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/spi_hw.c
    # PC-MCU 与 MCU-RK 是两套独立协议，源码目录必须保持物理隔离。
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_protocol.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_test_message.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_test_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_power_service.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/pc/pc_tester_info.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/common/crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/idd_session.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/rk/state_machine.c
    # 正式任务按通信对象分区，纯验证任务统一放test目录。
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/pc/tcp_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk/rk_link.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk/rk_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk/rk_idd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common/led_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common/monitor_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/common/task_watchdog.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/pwr_wave_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/result_led_wave_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/qspi_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test/test_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/AD7124/ad7124.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_layout.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/IIC/myiic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/LED/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/result_led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/MCP4725/mcp4725.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PCF8574/pcf8574.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PAC1934/pac1934.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/TPS62868/tps62868.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/WDT/wdt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/Middlewares/Third_Party/cJSON/cJSON.c
)

# Mock任务体只进入隔离测试镜像，正式固件不编译测试代码。
if(ENABLE_RK_GATEWAY_TEST)
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE App/tasks/test/rk_gateway_test.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_RK_GATEWAY_TEST=1)
endif()

if(ENABLE_V2_POWER_SIMULATION)
    if(DEFINED B1_RK_UART OR ENABLE_RK_GATEWAY_TEST OR ENABLE_PC_PROTOCOL_NET_TEST OR
       ENABLE_PWR_WAVE_TEST OR ENABLE_RESULT_LED_WAVE_TEST OR ENABLE_FLASH_SELF_TEST OR
       ENABLE_DRIVER_MOCK_TEST OR ENABLE_AD7124_WAVE_TEST OR ENABLE_DRIVER_SELF_TEST)
        message(FATAL_ERROR "V2 power simulation requires normal V2 tasks and real RK")
    endif()
    target_sources(${CMAKE_PROJECT_NAME} PRIVATE App/tasks/test/v2_power_sim.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_V2_POWER_SIMULATION=1)
endif()

if(ENABLE_V2_MATS_FAIL_TEST)
    if(NOT ENABLE_V2_POWER_SIMULATION OR DEFINED B1_RK_UART)
        message(FATAL_ERROR "MATS失败联调仅允许显式启用电源模拟的V2镜像")
    endif()
    message(WARNING "此镜像每次MATS都向PC上报模拟FAIL，不可作为真实测试结论")
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_V2_MATS_FAIL_TEST=1)
endif()

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

# 配置自定义头文件搜索目录。
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/b2
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/common
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/b2
    # 自定义模块头文件目录。
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

# 配置项目编译宏。
# 正式/网络镜像使用V2六路排针模拟结果灯；隔离自检及波形镜像保留各自GPIO归属。
if(NOT (ENABLE_PWR_WAVE_TEST OR ENABLE_RESULT_LED_WAVE_TEST OR
        ENABLE_FLASH_SELF_TEST OR ENABLE_DRIVER_MOCK_TEST OR ENABLE_AD7124_WAVE_TEST))
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_V2_RESULT_LED=1)
endif()

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # 各隔离验证功能的编译开关。
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
    FIRMWARE_BUILD_TIME_UTC="${FIRMWARE_BUILD_TIME_UTC}"
    FIRMWARE_BUILD_ID="${CMAKE_BUILD_TYPE}"
    FIRMWARE_TARGET="apollo-v2-h743"
)

# 移除工具链可能误检出的ob库依赖。
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# 连接生成层目标及其传递依赖。
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # 需要额外库时在此补充。
)

# cJSON用浮点格式化及回读校验输出小数；newlib-nano默认不链接这两项。
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,-u,_printf_float -Wl,-u,_scanf_float)

# 生成可直接烧录的裸镜像、CRC32清单和76字节版本载荷。
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
            --build-time "${FIRMWARE_BUILD_TIME_UTC}"
            --build-id "${CMAKE_BUILD_TYPE}"
            --target "apollo-v2-h743"
    COMMENT "Generating firmware binary, CRC32 manifest and W25Q256 version payload"
    VERBATIM
)

# 最终ELF/BIN同时包含精简Boot和单App。
include(boot/minimal_boot.cmake)
