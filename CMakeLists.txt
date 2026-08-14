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

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

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
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/uart_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/i2c_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/pwm_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/qspi_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv/spi_hw.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/dispatch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/frame.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol/state_machine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/hb_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/led_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/monitor_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/qspi_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/rk_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/tcp_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks/test_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/AD7124/ad7124.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash/flash_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/IIC/myiic.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/LED/led.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PCF8574/pcf8574.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/WDT/wdt.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_CURRENT_SOURCE_DIR}/HAL_Drv
    ${CMAKE_CURRENT_SOURCE_DIR}/App/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/App/tasks
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/AD7124
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/Flash
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/IIC
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/LED
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/PCF8574
    ${CMAKE_CURRENT_SOURCE_DIR}/BSP/WDT
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
