# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 每次里程碑更新「六、当前进度」一节。详细调试记录见 `docs/`。

---

## 一、项目是什么

STM32H743 **阿波罗 V2** 板，作为 **TCP server + TLV 协议中继**，把 PC 命令经串口透传给 RK3588（内存测试/电源监控从板）。

**拓扑**：
```
PC ──TCP:8888──► MCU(STM32H743) ──USART1──► RK3588
```

**软件栈**：FreeRTOS 10.6.2 + LWIP 2.2.1 + ETH (YT8512C, RMII) + TLV 协议。

---

## 二、TLV 协议（0x5A/0xA5 帧）

```
[0x5A] [Len:2B BE] [HCRC:2B] [Seq:8B] [TLV...] [FCRC:2B] [0xA5]
```
- CRC = Modbus CRC-16（多项式 0x8005，初值 0xFFFF）
- TLV 单元：`[Type:2B][TLV_Len:2B][Value]`
- 命令容器 `Type=0x5000`（Value = `[CmdID:2B][Params]`）；心跳 `Type=0x0000`
- 帧内 TLV 数量：**至少 1 个，最多 16 个**（`TLV_MAX_PER_FRAME`）
- 状态机 6 态：IDLE→HEADER→DATA→CRC→FOOTER，TCP 和 USART1 **共用**（`App/protocol/state_machine.c`）

> ⚠️ **type 语义未定**：现状是枚举常量（0x5000/0x0000），plan 文档里的「高4位类型+低12位Tag」结构化是另一套互斥设计，已搁置，等驱动写完拿真实数据再定。详见「七、待定事项」。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/` | TLV 协议栈：frame.c / state_machine.c / dispatch.c / crc.c |
| `App/tasks/` | FreeRTOS 任务：tcp_task / rk_task / hb_task / monitor_task 等 |
| `BSP/` | 板级驱动：AD7124 / Flash / IIC / LED / PCF8574 / WDT |
| `HAL_Drv/` | 硬件抽象：uart_router.c（串口路由）等 |
| `Core/` `Drivers/` `Middlewares/` | CubeMX 生成（HAL/CMSIS/FreeRTOS/LWIP），**勿手改** |
| `tests/` | Python 测试：`full_chain_stress.py`（12场景）、`stability_test.py`（6项） |
| `cmake/stm32cubemx/` | CubeMX 生成，**勿手改**（自定义源放顶层 CMakeLists.txt） |

---

## 四、工具链（最重要，换 AI 必读）

### 编译
```bash
cd build_debug && ninja
```

### 烧录（SWD）— 两步: 先 Bootloader 后 App
```bash
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/boot.elf   # Bootloader @ 0x08000000
STM32_Programmer_CLI.exe -c port=SWD -w build_debug/test.elf   # App @ 0x08020000
STM32_Programmer_CLI.exe -c port=SWD -rst                      # 复位运行
```
> Bootloader = 最小跳转 (boot/boot.s, 校验 App 向量表后跳转, 无升级逻辑)。
> App 编译在 0x08020000 (linker + VECT_TAB_OFFSET 已改)。在线升级 (W25Q256 搬移) 待 B2 联调后开发。

### 工具链路径
```
C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/
```

### 串口 / 网络
- 串口：COM3 = CH340 → USART1（阿波罗阶段接 PC 模拟 RK3588）
- MCU IP：`192.168.1.201:8888`

### 崩溃诊断（HardFault 排查）
- 诊断地址：`crash_log` = 0x20009600（16×uint32），`tcp_diag` = 0x20009640（用 `nm test.elf | grep crash_log` 验证为准）→ **实测(20260817): crash_log=0x200094E0, tcp_diag=0x20009520**（.crash_log 段 NOLOAD, 复位不清零, 排查时先 `-w32 0x20009520 0 0 0 0 0 0 0 0` 清空再复位）
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### App 偏移启动坑（20260817 实测发现）
- **`system_stm32h7xx.c` 的 `USER_VECT_TAB_ADDRESS` 必须解除注释**，否则 `SCB->VTOR` 设置代码被 `#if defined(...)` 编译掉 → 向量表留在 0x08000000（bootloader 空白区）→ 任何中断触发即 IACCVIOL 跑飞。CubeMX 重生成会把它注释回去，须恢复（VECT_TAB_OFFSET=0x020000 已设）。
- 诊断手法：`-coreReg PC LR MSP XPSR` 读 PC 定位跑飞点；`-r32 0xE000ED28 1`（CFSR）看故障类型；`-r32 0xE000ED08 1` 看 VTOR。

### 运行测试
```bash
python tests/stability_test.py
python tests/full_chain_stress.py
```

### 工具依赖分层（换 AI 必读）

| 层 | 工具 | 换 AI 后 |
|----|------|---------|
| CLI（机器上，永不丢） | ninja / STM32_Programmer_CLI / arm-none-eabi-objdump / python / git / curl | ✅ 任何 AI 用 Bash 直接调 |
| 仓库脚本（git 内，永不丢） | tests/*.py、tools/mcp_serve.bat、.mcp.json | ✅ clone 即有 |
| MCP 服务器（绑定 Claude Code，会丢） | embedded-docs（查寄存器手册）、PlatformIO、translator | ⚠️ 降级到 CLI |

**MCP 降级方案**（换 AI 后若 MCP 不可用）：
- `embedded-docs`（查 STM32 寄存器/手册）→ 直接读 PDF 手册，或 `mineru-open-api flash-extract xxx.pdf` 转 md 读
- `PlatformIO` → **本项目未用**，编译烧录走 ninja + STM32_Programmer_CLI，无需它
- `mineru`（PDF 转 md skill）→ 底层是 npm 包，CLI 直接 `mineru-open-api flash-extract <file>` 等价

> 核心：开发主链路（编译→烧录→测试）纯 CLI 兜底，MCP 只是查手册辅助，断了可降级。

---

## 五、关键架构约束（改代码前必读）

1. **MCU 是单客户端 TCP server**：`listen(listenfd, 1)` + 单 accept 循环，一次只服务 1 个连接。测试脚本开新连接前必须 `close_tcp()`。
2. **背压链闭环**：`pcToRk_Queue`(8槽) + rk pending(4槽) = 最多 12 条在途。队列满→回 BUSY(0x04)，pending 满→命令滞留队列。**uart_send 可能失败（TX环满），每处调用都要想「失败怎么办」**。
3. **RK 响应匹配不到 pending 一律静默丢弃，绝不回 ACK**（否则 RK 把 ACK 当命令再回响应 → 死循环，曾观测单 seq 重传 1207 次）。
4. **重传**：500ms 超时 + 指数退避 500/1000/2000ms，封顶 3 次（`RK_MAX_RETRIES`）。
5. **dispatch 在 tcp_task 上下文同步执行**，handler 里**禁止 vTaskDelay 阻塞**（reboot 用软件定时器，非阻塞）。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。

---

## 六、当前进度

**阶段：阿波罗 V2 固件调试完成，全链路稳定。**

- ✅ 全链路压测 12 场景 **33/33 PASS**，数据零损坏，准确率 100%，吞吐 27~35 帧/s
- ✅ 稳定性测试 11/11 PASS
- ✅ 已修复 3 个固件 bug（重传热循环 / 转发丢命令 / ACK回环）+ 多个测试脚本 bug
- ✅ git 已初始化，推送到 Gitea `LILIHONG/stm32h743`（私有）

**下一步（待办）**：详见 `doc/driver_tasks.md`（唯一任务来源，每任务 4 步闭环：原理图确认→编码→阿波罗V2模拟→B2真板验证）
> **两阶段策略**：阶段一 = 阿波罗 V2 开发（所有驱动代码 + 模块级验证, 当前）；阶段二 = B2 真板（B2 到货后：引脚重映射 + 实测标定 + RK 联调）
1. **pwr_ctrl**（PC6/PC7/PG11 上电时序, 默认 OFF 已确认）— 阻塞所有电源类驱动
2. **TPS62868**（I2C1/2/4, 地址 0x42-0x44 按 BOM 料号; 与 pwr_on 联动）
3. **PAC1934T**（I2C4, 0x2C）+ **AD7124**（SPI4 PE11-14, 三轨回读已确认）
4. ~~加热控制~~（**已取消** 20260817: B1 MCU 不负责加热, 由 PC 操控独立加热模块, 原理图 P18 电路不采用）
5. **Boot 模式控制**（MCP4725 DAC, I2C2, 0x60 → BOOT_SARADC_IN0, 20260811 新增, 代码 V2 可做）
6. 拿到驱动真实数据后 → **修订 TLV type 语义**（枚举 vs 结构化）
7. B1 板到货后：UART4/USART2 从 CH340 切到 RS0102→RK3588 真机联调（**注意不是 USART1**）
8. 高波特率（460800/921600）DMA+IDLE（等真机 RS0102）

### B2 板原理图（MB360）已解析结论（2026-08，几何法从 PDF 提取）

> 分析工具：`tools/sch_netlist.py`（PDF 矢量线段重建网络表，无视觉模型可用）。
> 原理图源：`docs/MYZR-RK3588-ZUOYI_SCH_20260807-.pdf`（P1-16 RK 核心板 CB314，**P17-23 底板 MB360 = B2**，P24 DIMM314 连接器）。

**⚠️ 重要修正**：真机 RK3588 通信**不是 USART1**！是 **UART4 (PA11=RX/PA12=TX) + USART2 (PD5=TX/PD6=RX) 各经一路 RS0102 电平转换**（U4008: UART0_M0↔UART4；U4013: UART2_M0_DEBUG↔USART2）。USART1(PA9/PA10) 只是板上本地串口。硬件上电时 RK 的 UART0_M0 是 BMC console，联调应改用 UART4/USART2（待真机验证）。

| 外设 | 引脚 | 目标 | 备注 |
|------|------|------|------|
| AD7124 (U4028) | SPI4: PE11=NSS PE12=SCK PE13=MISO PE14=MOSI | AIN0 ← VCC_1V8_S3_LPDDR4X / AIN2 ← VDD2_DDR_S3 / AIN4 ← VDDQ_DDR（导线追踪已确认, 各经 100R+10K+0.1uF 滤波）; AIN1/3/5=AGND, AIN6/7=NC | REFIN1+ ← VCC_2V5 (U4029 RY6211B-25C LDO) |
| PAC1934T (U204) | I2C4 (PH4=SCL/PH5=SDA) | 地址 **0x2C/0x2D**（ADDRSEL=5.23K→GND） | SENSE1-3: VCC_1V8_S3/VDD2_DDR_S3/VDDQ_DDR 各 0.01Ω; SENSE4 未用 |
| TPS62868 ×3 | U4025=I2C1, U4026=I2C2, U4027=I2C4 | 输出 VCC_1V8_S3_LPDDR4X(VDD1) / VDD2_DDR_S3(VDD2) / VDDQ_DDR(VDDQ) | EN 全接 **PG11**（各带 R4074/R4080/R4085 10K 下拉 → 默认 OFF）; VIN=VCC4V0_SYS; pin9=PG → **C 版, I2C 地址全部 0x42**（TI Table 8-3; 三颗各在独立 I2C 总线不冲突; x0C/x1C/x2C 仅电压因子 0.5/1/2, 以 BOM 料号为准）; VOS=输出电压采样 |
| pwr_ctrl | **PC6**→MP8759 EN (VCC4V0_SYS 4.0V), **PC7**→RT8070 EN (VCC3V3_SYS 3.3V), **PG11**→3×TPS62868 EN | EN 均默认 OFF（无上拉）→ 上电序: 12V→5V→(MCU)→3.3V(PC7)→4.0V(PC6)→VDD 轨(PG11)，顺序/延时待实测 | **3V3_MCU 来源原理图无法定位**（全板无独立 3.3V 稳压器、无 VCC3V3_SYS↔3V3_MCU 0R；且 EN 默认 OFF 与自举矛盾 → 必有独立路径, 须实测/等 B2 板） |
| LED | PG0-6 → 2N7002 → LED1-7 | 3V3_MCU 域 | |
| SWD/调试 | PA13=JTMS PA14=JTCK, NRST→MCU_RESET_L, BOOT0 | | |
| ETH RMII | PA1 REF_CLK / PA2 MDIO / PA7 CRS_DV / PB11 TX_EN / PC1 MDC / PC4 PC5 RXD0/1 / PG13 PG14 TXD0/1 | 经 SN74LVC1G3157×8 可选 B1(核心板)/B2(背板) | YT8512H (P23) |
| SPI2 (PB12-15, MCU_SPI_*) | → DIMM314 → RK 核心板 | 用途待定（非 AD7124） | |

> **风扇/加热与 MCU 无关**：风扇 PWM12_M1_FAN = RK3588 自控（P14, Q3/Q2→J15）；P24 "VCC_24V 软开关输出"（疑老化加热电源）由 RK PWRON_L 控制。MCU 只管 PC6/PC7/PG11。原理图源文件 `docs/MYZR-RK3588-ZUOYI_SCH_20260807-.pdf`（原文件名为 ...20260803-(2)，内容即最新版）。

### 原理图 20260811 版新增（docs/MYZR-RK3588-ZUOYI_SCH_20260811.pdf, 当前最新）

> ⚠️ 修正(20260817)：~~加热是 MCU 控制的~~ → **加热已确认不由 MCU 负责**（由 PC 独立加热模块控制, P18 电路不采用）。**优先用本版分析**，与 20260807 版差异如下：

| 新增电路 | 内容 | MCU 影响 |
|----------|------|----------|
| ~~加热控制 (P18)~~ | MCU **PH8/PH9/PH10** → Q19/Q21/Q23 (2N7002) → R7277-7284 (470K) → Q18/Q20/Q22 (Si7129DN N-MOS) → **G88MP04102KEU 加热片×3**（VCC12V_DCIN 供电, J32/J33/J34 接口, 47uF 滤波） | **不采用 (20260817)**：B1 MCU 不负责加热, 由 PC 独立加热模块控制 |
| **引导模式控制 (P24)** | **U4030 MCP4725A0 DAC** (SOT23-6): VDD=3V3_MCU, A0→GND(**地址 0x60**), SCL/SDA ← R7286/R7287(0R) ← **I2C2**（与 TPS62868 U4026 同总线）; VOUT → R7288(0R) → **BOOT_SARADC_IN0** → RK3588 (p<6>) | **MCU 写 DAC 电压 → 选 RK3588 引导模式**: 0V=USB / 0.3V=SD / 0.6V=EMMC / 0.9V=FSPI M0 / 1.2V=FSPI M1 / 1.5V=FSPI M2 / 1.8V=EMMC-SD（TABLE 1, 100K 上拉域）。另有 SARADC_VIN1_KEY/RECOVERY 电阻阶梯 (LEVEL1-7) + MASKROM_Key (J36/J37) |
| 连接器 | 40x2 头子 → **FX18-60P-08SV10 (30x2=60pin)** ×2；新增 J35 (CXC01L3Z) | |
| P22 | PAC1934T 采样路径新增 R7268-7271 (4×0402, 值未提取) | |
| P21 | VDD 去耦 C4094-4104 + FB0603×4 + R7272；AD7124 封装注记 LFCSP-32（疑 AD7124-8?）；CHS-01TB 按键 | |
| P14/P15/P23 | TYPEC-16P 焊座替换 P1 插头；WAFER-GH125 线束座 | |

---

## 七、待定事项（决策搁置中）

| 事项 | 状态 | 何时定 |
|------|------|--------|
| TLV type 语义（枚举 0x5000 vs 结构化高4位+低12位） | 搁置 | 驱动写完拿真实数据后 |
| 0x5000→0x5002 的语义（命令请求 vs 结果响应？） | 待确认 | 同上 |
| 打包侧一帧一 TLV vs 解包侧一帧多 TLV 不对称 | 待改 | 同上（多TLV打包三段式 builder） |
| `tlv_dispatch` 多单元响应互相覆盖 | 待改 | 同上 |

---

## 八、已知坑（改代码前必读）

### CubeMX 重新生成会覆盖的手动改动
1. 自定义源/头 → 放**顶层 CMakeLists.txt**（勿放 cmake/stm32cubemx/）
2. `heap_4.c` 的 `.heap` section → 重加；长期用 `configAPPLICATION_ALLOCATED_HEAP=1`
3. `stm32h7xx_it.c` 的 `HardFault_Handler_C` → USER CODE 区恢复
4. USART TX drain → 已用 HAL `TxISR` 钩子根治，it.c 无自定义代码
5. **`system_stm32h7xx.c`**：`USER_VECT_TAB_ADDRESS` 解除注释 + `VECT_TAB_OFFSET=0x020000`（CubeMX 无对应 UI，重生成后必复位 → 手动恢复，见"App 偏移启动坑"）
6. **`STM32H743xx_FLASH.ld`**：`FLASH ORIGIN=0x08020000, LENGTH=1920K`（CubeMX 无对应 UI，重生成后复位 0x08000000/2048K → 手动恢复）

> ✅ 已入 .ioc 自动保留：`SPI2.CPolarity=HIGH + ClockPhase=2EDGE`（**AD7124 模式 3** CPOL=1/CPHA=1，手册 P8 明确"operates in SPI Mode 3"，20260819 修正——此前误判为模式1）；`freertos.c` StartTestTask 自检在 USER CODE 区；boot/ 与顶层 CMakeLists 不受 CubeMX 影响。

### 测试脚本坑
- 清串口缓冲必须先 settle（等 pending+重传排空）再 `reset_input_buffer()`，否则抓到旧帧
- 全链路测试 `_drain_serial_commands()` 动态排空，别用固定 sleep

### 内存布局
- 任务栈：rk 32KB / tcp 16KB / hb 4KB；堆 128KB 实际占用约 85-100KB，余 ~30KB
- 确认堆余量用 `xPortGetFreeHeapSize()`

---

## 九、提交规范（git）

- **远程仓库**：Gitea `http://192.168.1.122:3000/LILIHONG/stm32h743`（私有），本地 remote=origin，分支 main
- **每次 commit message 必须含两部分**：「做了什么」+「项目整体进度」
- 推送：`git add -A && git commit -m "..." && git push`
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
