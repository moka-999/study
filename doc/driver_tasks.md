# B2 板驱动开发任务清单 + 开发流程

> 唯一任务来源（AGENTS.md「下一步」引用本文件）。
> 原理图依据：`docs/MYZR-RK3588-ZUOYI_SCH_20260811.pdf`（当前最新，含加热/引导模式新增电路）。
> 引脚依据：本文件 §9 引脚总表（几何法从原理图提取 + H743 AF 校验）。

---

## 0. 开发阶段与流程

### 阶段一：阿波罗 V2 开发（现在 → B2 板到货前）

> 目标：**所有驱动的代码层全部完成并在 V2 上跑通**（协议/寄存器/时序逻辑），B2 只留"引脚重映射 + 真机标定"。

```
V2 可验证的：
  · GPIO 逻辑（PC6/7/PG11/PH8-10 翻转时序）
  · I2C 总线 + 真实芯片模块（TPS62868 / PAC1934T / MCP4725 用模块/杜邦线挂 V2 的 I2C1/I2C3）
  · SPI 总线 + 真实 AD7124 模块（挂 V2 SPI2）
  · TLV 命令接入 + python 测试脚本（协议层全部可测）
V2 不可验证、留给 B2 的：
  · 电源真时序标定（t1/t2/t3、3V3_MCU 来源、EN 默认态）
  · 加热片真功率/电流
  · Boot 模式与 RK reboot 集成
```

### 阶段二：B2 真板（B2 到货后）

> 目标：引脚重映射 + 实测标定 + 与 RK3588 真机联调。

```
1. CubeMX 重配引脚（SPI4 PE11-14 / I2C1 PB7-8 / I2C2 PF0-1 / I2C4 PH4-5 / UART4 PA11-12 / USART2 PD5-6）
2. 新增 i2c_if/spi_if 工厂实例（B2 引脚），驱动零改动（hal_if.h 设计保证）
3. 万用表/示波器标定：电源时序、DAC 电压-模式映射、加热电流
4. 真机联调：UART4/USART2 → RS0102 → RK3588
```

### 每任务 4 步闭环（阶段一内）

```
① 手册/原理图确认 → ② 编码(ninja 零警告) → ③ V2+模块验证 → ④ 测试脚本入库
```

**流程纪律**：
- 每任务一个 commit：`驱动代码 + 测试脚本 + 本文件更新` 一起提交
- 未定项用 `#define` 开关兜底，B2 实测后翻转
- 原理图版本变更 → 先跑 `tools/sch_netlist.py` 对比，再动驱动

---

## 1. 基础设施（V2 阶段第 1 步）

| # | 任务 | 输出 | 验证 |
|---|------|------|------|
| 1.1 | GPIO 抽象：`bsp_gpio.h` 定义 pwr/heater 引脚组（V2 测试引脚 ↔ B2 正式引脚 双映射） | bsp_gpio.c/h | V2: GPIO 翻转 |
| 1.2 | I2C 总线层：确认 V2 I2C1/I2C3 实例可用（i2c_hw.c 已有），补 is_ready 扫描 | i2c 扫描工具 | V2: 扫出模块地址 |
| 1.3 | SPI 总线层：V2 SPI2 实例 + CS GPIO 抽象（ad7124 已按 spi_if_t 写） | spi2 实例 | V2: SPI 回环/读 ID |

## 2. pwr_ctrl 电源时序（PC6/PC7/PG11）

**原理图依据**（P17/18/22）：PC6→MP8759 EN(4.0V)、PC7→RT8070 EN(3.3V) 经 R4020(1K)、PG11→3×TPS62868 EN（10K 下拉），**EN 均默认 OFF**；12V→5V 常开；3V3_MCU 独立供电（无自举死锁）。

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 2.1 | GPIO 初始化（输出推挽, 初始 LOW）+ 引脚映射表 | pwr_ctrl.c/h | 翻转/逻辑分析仪 | 实测 EN 电平 |
| 2.2 | `pwr_on()`：PC7→t1→PC6→t2→配 TPS62868→PG11→t3 | 时序函数 | 时序逻辑（示波器量 GPIO） | 示波器测 3.3/4.0/VDD 爬升 |
| 2.3 | `pwr_off()` 逆序 | 同上 | 同上 | 同上 |
| 2.4 | TLV：PWR_ON/OFF/STATUS | dispatch | python 回读 | 真机 |

**待实测开关**：`PWR_3V3_MCU_INDEP`（3V3_MCU 独立 vs 同源）；t1/t2/t3 默认 10/10/50ms。

## 3. TPS62868 DDR 电源（I2C1/2/4）

**手册**：TI SLVSFS3B（已下载）。寄存器 VOUT1=0x01/VOUT2=0x02/CONTROL=0x03/STATUS=0x05。C 版地址出厂固定：x0C=0x42 / x1C=0x43 / x2C=0x44（按 BOM 料号后缀）。

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 3.1 | I2C 寄存器读写 + 地址扫描（0x42-0x44） | tps62868.c/h | 模块挂 I2C1 | 三颗总线扫描 |
| 3.2 | 初始化：VOUT 设置（因子换算）、ramp speed、discharge | API | 回读 VOUT | 真机电压实测 |
| 3.3 | STATUS 读（热预警/HICCUP） | API | 回读 | 真机 |
| 3.4 | TLV：DDR_VOLTAGE_SET（老化调压） | dispatch | python | 真机 |

## 4. PAC1934T 电流采样（I2C4, 0x2C）

**原理图**（P22）：ADDRSEL=5.23K→GND → **0x2C**；SENSE1-3 = VCC_1V8_S3/VDD2_DDR_S3/VDDQ_DDR（0.01Ω）；SENSE4 未用；新增 R7268-71 采样路径电阻（值待提取）。

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 4.1 | 寄存器 map + 初始化（连续采样） | pac1934.c/h | 模块挂 I2C1 | 真机读数 |
| 4.2 | 采样读取 + 电流换算（/0.01Ω） | API | 恒流源比对 | 万用表比对 |
| 4.3 | TLV：IDD 采样上报（复用 0x0015-0x0024） | dispatch | python | 真机 |

## 5. AD7124 电压回读（SPI4, PE11-14）

**原理图**（P21）：AIN0←VCC_1V8_S3_LPDDR4X / AIN2←VDD2_DDR_S3 / AIN4←VDDQ_DDR（100R+10K+0.1uF）；REFIN1+←VCC_2V5；AIN1/3/5=AGND, 6/7=NC；封装注记 LFCSP-32（疑 AD7124-8, 待 BOM 确认）。

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 5.1 | 现有 ad7124.c 移植到 spi2_if（V2）+ 后续切 spi4_if（B2），CS 抽象 | 适配层 | 读 ID/寄存器回环 | 真机读数 |
| 5.2 | 通道配置：AIN0/2/4, REFIN1=2.5V, FS/滤波 | 配置 | 恒压源比对 | 万用表比对 |
| 5.3 | 电压换算 + 分压系数 | API | 同上 | 同上 |
| 5.4 | TLV：电压采样上报 | dispatch | python | 真机 |

## 6. 加热控制（PH8/9/10）— 代码 V2 完成, 真机 B2

**原理图**（P18, 20260811 新增）：
```
MCU PH8/9/10 → Q19/21/23 (2N7002) → R7277-7284 (470K) → Q18/20/22 (Si7129DN N-MOS)
             → G88MP04102KEU 加热片×3（VCC12V_DCIN, J32/33/34, 47uF 滤波）
```

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 6.1 | GPIO 配置（输出, 初始 OFF）+ `heater_set(ch,on)` | heater.c/h | 翻转 | 加热电流实测 |
| 6.2 | PWM 支持（PH8-10 TIM/AF 待查, 470K 下拉限频率 → 可能仅开关量） | `heater_set_pwm()` | V2 示波器 | 真机 |
| 6.3 | 安全保护：pwr_on 完成才允许 + PAC1934T 电流联动 | 保护逻辑 | 逻辑测试 | 真机 |
| 6.4 | TLV：HEATER_CTRL | dispatch | python | 真机 |

**⚠️ 待确认**：跨页标 p<24> 抄错（FX18 无 PH8-10）；G88MP04102KEU 额定功率；PWM 可行性。

## 7. Boot 模式控制（MCP4725 DAC, I2C2, 0x60）— 代码 V2 完成, 真机 B2

**原理图**（P24, 20260811 新增）：
```
MCU I2C2 (PF0/PF1) → R7286/87 (0R) → U4030 MCP4725A0T-E/CH (12-bit DAC)
  VDD=3V3_MCU, A0→GND(0x60), VOUT → R7288(0R) → BOOT_SARADC_IN0 → RK3588
```

| 模式 | 电压 | DAC 码(3.3V) | 模式 | 电压 | DAC 码(3.3V) |
|------|------|------|------|------|------|
| USB(Maskrom) | 0V | 0 | FSPI M1 | 1.2V | 1489 |
| SD Card | 0.3V | 372 | FSPI M2 | 1.5V | 1861 |
| EMMC | 0.6V | 745 | EMMC-SD | 1.8V | 2234 |
| FSPI M0 | 0.9V | 1117 | | | |

| # | 任务 | 输出 | V2 验证 | B2 验证 |
|---|------|------|---------|---------|
| 7.1 | MCP4725 驱动（Fast Write + 掉电模式）, 0x60 | mcp4725.c/h | 模块挂 I2C1, 回读寄存器 | 真机 |
| 7.2 | `boot_mode_set(mode)` 电压↔DAC 码映射 | API | 万用表测 VOUT（模块） | 万用表 + RK 真启动 |
| 7.3 | 与 reboot 集成：设模式 → TLV_CMD_REBOOT | 流程函数 | 逻辑测试 | RK 按模式启动 |
| 7.4 | TLV：BOOT_MODE_SET/QUERY | dispatch | python | 真机 |

**⚠️ 待确认**：3.3V DAC 直连 1.8V SARADC 域超量程（实测校准, 可能只用 ≤1.8V 区间）。

## 8. TLV 命令空间规划（新增）

| CmdID | 名称 | 说明 |
|-------|------|------|
| 0x0025 | PWR_ON | 上电时序 |
| 0x0026 | PWR_OFF | 下电时序 |
| 0x0027 | PWR_STATUS | 电源状态查询 |
| 0x0028 | HEATER_CTRL | 加热: [ch][on/pwm][duty] |
| 0x003A | BOOT_MODE_SET | 引导模式: [mode] |
| 0x003B | BOOT_MODE_QUERY | DAC 输出查询 |

（0x0029-0x003D 原为 RK 结果帧, 与本地命令错开, 提交前复核 dispatch.h）

## 9. 测试脚本规划

| 脚本 | 覆盖 | 阶段 |
|------|------|------|
| `tests/test_pwr.py` | pwr_on/off 时序、状态回读 | V2 逻辑 / B2 真时序 |
| `tests/test_ddr_rail.py` | TPS62868 电压设置/回读 | V2 模块 / B2 真机 |
| `tests/test_idd.py` | PAC1934T 电流 + AD7124 电压 | V2 模块 / B2 真机 |
| `tests/test_heater.py` | 加热开关/PWM + 电流联动 | V2 逻辑 / B2 真机 |
| `tests/test_boot_mode.py` | DAC 电压输出校验 | V2 模块 / B2 真机 |

## 10. 引脚总表（几何法提取 + H743 AF 校验, 20260811 版）

| 功能 | B2 引脚 | 总线/目标 | V2 阶段验证用 |
|------|---------|-----------|---------------|
| UART4 (RK UART0_M0) | PA11=RX PA12=TX | RS0102 U4008 | B2 阶段 |
| USART2 (RK UART2 debug) | PD5=TX PD6=RX | RS0102 U4013 | B2 阶段 |
| USART1 | PA9=TX PA10=RX | 板载本地串口 | 现用（CH340） |
| I2C1 | PB7=SDA PB8=SCL | TPS62868 U4025 | V2 I2C1/I2C3 模块 |
| I2C2 | PF0=SDA PF1=SCL | TPS62868 U4026 + **MCP4725(0x60)** | V2 模块 |
| I2C4 | PH4=SCL PH5=SDA | TPS62868 U4027 + PAC1934T(0x2C) | V2 模块 |
| SPI4 | PE11=NSS PE12=SCK PE13=MISO PE14=MOSI | AD7124 U4028 | V2 SPI2 模块 |
| SPI2 | PB12-15 MCU_SPI_* | DIMM/FX18→RK | 待定 |
| ETH RMII | PA1/PA2/PA7/PB11/PC1/PC4/PC5/PG13/PG14 | YT8512H | 现用 |
| pwr_ctrl | PC6=4.0V EN, PC7=3.3V EN, PG11=DDR EN | 上电时序 | V2 GPIO 模拟 |
| 加热 | PH8/9/10 | 3 路加热片 | V2 GPIO 模拟 |
| LED | PG0-6 | LED1-7 | 现用 |
| SWD | PA13=JTMS PA14=JTCK | | |

> ⚠️ `doc/b1_mcu_pinout.md` 已过时（USART1 标注为 RK 主通道、I2C2=PH4/5、I2C4=PF14/15 均错），以本表为准。
