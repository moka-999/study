set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# GNU Arm工具链公共配置。
# 构建入口须将arm-none-eabi工具目录加入PATH。
set(TOOLCHAIN_PREFIX                arm-none-eabi-)

set(CMAKE_C_COMPILER                ${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER              ${CMAKE_C_COMPILER})
set(CMAKE_CXX_COMPILER              ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_LINKER                    ${TOOLCHAIN_PREFIX}g++)
set(CMAKE_OBJCOPY                   ${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_SIZE                      ${TOOLCHAIN_PREFIX}size)

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Cortex-M7及硬浮点参数，与现有工具链保持一致。
set(TARGET_FLAGS "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard ")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections -fstack-usage")

# STM32CubeIDE的圈复杂度功能需要编译器支持相应选项。
# 当前GNU工具链不保证支持，保持关闭，避免引入构建错误。
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcyclomatic-complexity")

set(CMAKE_C_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_C_FLAGS_RELEASE "-Os -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
if(ENABLE_OTA_RELOCATION_PROBE)
    if(NOT OTA_APP_BASE MATCHES "^(0x08020000|0x08100000)$")
        message(FATAL_ERROR "OTA probe only supports A1 or A2")
    endif()
    # --defsym必须在-T之前，使链接脚本按实验槽地址计算装载区域。
    string(APPEND CMAKE_EXE_LINKER_FLAGS
        " -Wl,--defsym=OTA_APP_BASE=${OTA_APP_BASE},--emit-relocs")
endif()
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/STM32H743xx_FLASH.ld\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(TOOLCHAIN_LINK_LIBRARIES "m")
