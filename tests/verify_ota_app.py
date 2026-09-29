#!/usr/bin/env python3
"""使用已有GNU/QEMU运行实际App OTA后台，无网络协议伪实现、不接板卡。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot-root", type=Path, default=Path(__file__).resolve().parents[2] / "bootloader")
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--app-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--target", choices=("apollo-v2-h743", "b1-h743"), default="apollo-v2-h743")
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--qemu", type=Path, default=Path("D:/qemu/qemu-system-arm.exe"))
    args = parser.parse_args()
    root = args.app_root.resolve()
    harness = Path(__file__).resolve().parent / "test_ota_app.c"
    if args.package.read_bytes()[100:116].rstrip(b"\0").decode("ascii") != args.target:
        raise ValueError("测试包与指定板型不一致")
    build = root / "build/ota-app-test"
    build.mkdir(parents=True, exist_ok=True)
    for name in ("boot_record", "boot_health", "ota_install"):
        for ext in ("c", "h"):
            if (root / f"App/ota/{name}.{ext}").read_bytes() != (args.boot_root / f"boot/{name}.{ext}").read_bytes():
                raise ValueError(f"B/App共享文件不同：{name}.{ext}")
    # 只替换实际硬件/RTOS端口；业务、持久化编码、版本和CRC验证仍执行正式C。
    (build / "ota_test_port.h").write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hal_if.h"
extern flash_if_t flash_if;
typedef struct { uint32_t VTOR; } ota_test_scb_t;
extern ota_test_scb_t ota_test_scb;
#define SCB (&ota_test_scb)
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#define PWR_CTRL_STATE_OFF 0U
typedef struct { uint32_t timestamp_ms, power_state; } monitor_snapshot_t;
uint32_t HAL_GetTick(void);
void NVIC_SystemReset(void);
int flash_transaction_begin(uint32_t);
void flash_transaction_end(void);
int monitor_get_latest(monitor_snapshot_t *);
int pwr_ctrl_get_state(void);
int monitor_ota_enter(void);
void monitor_ota_leave(void);
int qspi_read_data(uint32_t,uint8_t *,uint32_t);
int qspi_write_page(uint32_t,const uint8_t *,uint16_t);
int qspi_erase_sector(uint32_t);
''', encoding="utf-8")
    for name in ("flash_bsp", "qspi_hw", "monitor_task", "FreeRTOS", "task", "main"):
        (build / f"{name}.h").write_text('#include "ota_test_port.h"\n', encoding="utf-8")
    env = dict(os.environ)
    env["PATH"] = str(args.compiler.parent) + os.pathsep + env.get("PATH", "")
    sources = [harness, root / "BSP/Flash/flash_layout.c",
        root / "App/protocol/pc/pc_ota.c", root / "App/protocol/pc/pc_protocol.c",
        root / "Middlewares/Third_Party/cJSON/cJSON.c"] + [root / f"App/ota/{n}.c" for n in
        ("ota_app", "ota_install", "boot_record", "boot_health")] + [args.boot_root / "tests/qemu_start.s"]
    command = [str(args.compiler), "-mcpu=cortex-m7", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv5-d16",
        "-g", "-O1", "-DENABLE_OTA_BOOT=1", "-Wall", "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
        "-DCJSON_NESTING_LIMIT=16", f'-DOTA_TEST_TARGET="{args.target}"',
        "-I" + str(build), "-I" + str(root / "App/ota"),
        *["-I" + str(root / p) for p in ("BSP/Flash", "HAL_Drv", "App/protocol/pc", "Middlewares/Third_Party/cJSON")],
        *map(str, sources), "-nostartfiles", "--specs=nosys.specs",
        "-T" + str(args.boot_root / "tests/qemu_test.ld"), "-Wl,--gc-sections", "-o", str(build / "test.elf")]
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (build / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise ValueError(result.stdout + result.stderr)
    # 普通镜像单独链接正式协议层，不提供Flash/复位/OTA后台符号，防止保留动作误接入。
    disabled_sources = [harness.with_name("test_pc_ota_disabled.c"),
        root / "App/protocol/pc/pc_ota.c", root / "App/protocol/pc/pc_protocol.c",
        root / "Middlewares/Third_Party/cJSON/cJSON.c", args.boot_root / "tests/qemu_start.s"]
    disabled_command = [arg for arg in command if arg not in map(str, sources)]
    disabled_command[disabled_command.index("-DENABLE_OTA_BOOT=1")] = "-DENABLE_OTA_BOOT=0"
    disabled_command[-1] = str(build / "disabled.elf")
    disabled_command.extend(map(str, disabled_sources))
    result = subprocess.run(disabled_command, capture_output=True, text=True, env=env, timeout=60)
    (build / "disabled-build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise ValueError(result.stdout + result.stderr)
    result = subprocess.run([str(args.qemu), "-M", "mps2-an500", "-display", "none", "-monitor", "none",
        "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native", "-kernel",
        str(build / "disabled.elf")], capture_output=True, text=True, env=env, timeout=60)
    (build / "disabled.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise ValueError(f"普通镜像拒绝回归失败：{result.returncode}, {result.stdout + result.stderr}")
    print("PASS: disabled firmware upgrade rejection", flush=True)
    cases = ["ready", "partial_program_error", "corrupt_package", "commit_written_but_error",
             "rk_missing_at_start", "rk_stalled_after_confirm"]
    for mode, name in enumerate(cases):
        (build / "config.bin").write_bytes(struct.pack("<I", mode))
        result = subprocess.run([str(args.qemu), "-M", "mps2-an500", "-display", "none", "-monitor", "none",
            "-serial", "none", "-no-reboot", "-semihosting-config", "enable=on,target=native", "-kernel", str(build / "test.elf"),
            "-device", f"loader,file={args.package.resolve().as_posix()},addr=0x60000000,force-raw=on",
            "-device", "loader,file=config.bin,addr=0x60400000,force-raw=on"], cwd=build,
            capture_output=True, text=True, env=env, timeout=60)
        output = result.stdout + result.stderr
        (build / f"{name}.log").write_text(output, encoding="utf-8")
        if result.returncode or "PASS: actual App" not in output:
            raise ValueError(f"{name}: {output}")
        print(f"PASS: {name}", flush=True)
    report = {"scope": "actual App/PC OTA C in QEMU, mocked board ports, no network or hardware proof",
        "passed": ["disabled_firmware_upgrade_rejection", *cases],
        "ota_app_sha256": hashlib.sha256((root / "App/ota/ota_app.c").read_bytes()).hexdigest()}
    (build / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
