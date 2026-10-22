"""只构建第二块Apollo V2的裸机SPI主机；不启动调试器、不烧录、不访问网络。"""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    build = root / "build/b2-spi-master"
    build.mkdir(parents=True, exist_ok=True)
    hal = root / "Drivers/STM32H7xx_HAL_Driver"
    cmsis = root / "Drivers/CMSIS"
    sources = [here / name for name in ("main.c", "board.c", "wire.c", "syscalls.c")]
    sources += [hal / "Src" / ("stm32h7xx_hal" + name + ".c") for name in (
        "", "_cortex", "_rcc", "_rcc_ex", "_gpio", "_dma", "_dma_ex",
        "_pwr", "_pwr_ex", "_flash", "_flash_ex", "_spi", "_uart", "_uart_ex")]
    sources += [cmsis / "Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c",
                root / "startup_stm32h743xx.s", root / "App/protocol/common/crc.c"]
    includes = [here, hal / "Inc", cmsis / "Include", cmsis / "Device/ST/STM32H7xx/Include",
                root / "App/protocol/b2", root / "App/protocol/common"]
    command = [args.compiler, "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
               "-std=c11", "-Os", "-g3", "-DSTM32H743xx", "-DUSE_HAL_DRIVER",
               "-DUSER_VECT_TAB_ADDRESS", "-DVECT_TAB_OFFSET=0",
               "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections"]
    command += ["-I" + str(path) for path in includes]
    elf = build / "b2_spi_master.elf"
    command += [*map(str, sources), "--specs=nano.specs", "--specs=nosys.specs", "-nostartfiles",
                "-T" + str(here / "flash.ld"), "-Wl,--gc-sections",
                "-Wl,-Map=" + str(build / "b2_spi_master.map"), "-o", str(elf)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    tool = Path(args.compiler)
    for fmt, suffix in (("ihex", ".hex"), ("binary", ".bin")):
        subprocess.run([str(tool.with_name("arm-none-eabi-objcopy.exe")), "-O", fmt,
                        str(elf), str(elf.with_suffix(suffix))], check=True, timeout=30)
    subprocess.run([str(tool.with_name("arm-none-eabi-size.exe")), str(elf)], check=True, timeout=30)
    print(f"Built: {elf}\nFlash origin=0x08000000; SECOND board only; NOT flashed.")


if __name__ == "__main__":
    main()
