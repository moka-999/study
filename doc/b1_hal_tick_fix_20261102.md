# B1启动停在PHY复位的修正

日期：2026-11-02。范围：B1首次实板启动；V2代码不变，无协议变更。

## 现象与原因

用户烧录 `b1_192.168.1.201.elf` 后，串口每约33.2秒重复启动日志，最后一行为 `[boot] MAC and PHY reset`。网线通过飞线转RJ45连接。

B1已经启用TIM1作为HAL的1ms时基，且中断调用 `HAL_TIM_IRQHandler(&htim1)`，但遗漏 `HAL_TIM_PeriodElapsedCallback`。原ELF只有HAL弱空回调，`HAL_IncTick`未被链接；计数不能增长，PHY复位处的 `HAL_Delay(10)` 因而不能结束，程序尚未启动RTOS和lwIP。

重复周期与IWDG配置相符，但本轮没有读取实板复位原因寄存器；不能据此排除板上其他复位来源。PC当时为192.168.1.200/24，ping两板地址均超时；尚未验收飞线和PHY链路。

## 修改

在 `D:/pro/b1/Core/Src/stm32h7xx_it.c` 增加强回调：仅TIM1调用 `HAL_IncTick()`，其他定时器不推进HAL计数。保持TIM1时基、FreeRTOS的SysTick、看门狗和默认RK/DDR断电策略。

V2已有相同职责的有效回调，不重复添加。两板IP规则、PC协议、Boot/App分区及无W25配置均不变。

## 验证与产物

新增 `tests/test_b1_hal_tick.c` 和 `tests/verify_b1_hal_tick_c.py`，编译实际B1中断文件及实际HAL计时实现，用QEMU验证TIM1推进、其他定时器隔离、计数回绕，以及PHY使用的10ms/100ms等待。

- 修改前失败：`FAIL: TIM1 callback does not increment HAL tick`。
- 修改后通过：`PASS: B1 TIM1 tick, other timer isolation, wrap and PHY delays`。
- 测试以模拟回调代替硬件IRQ，只证明软件计时/等待逻辑，不证明实物定时精度或中断实际到达。
- 两份B1完整Boot＋App构建及各自的项目检查通过；最终ELF确认链接强回调和 `HAL_IncTick`。

| 地址 | 完整烧录文件 | ELF SHA256 |
|---|---|---|
| 192.168.1.201:8888 | `D:/pro/b1/build/standalone/Debug/b1_192.168.1.201.elf` | `86DEBB35F493AB7F217FD6DBBD72B91BF49CE0CFC48D64449DDAA17CECCAA35A` |
| 192.168.1.202:8888 | `D:/pro/b1/build/standalone/Debug-ip202/b1_192.168.1.202.elf` | `68B7475EAE55DAE6357C2971160E83FDADD5FC0DFEE09CDC1A9EAB628854F127` |

Boot仍为0x08000000，App为0x08020000，两份BIN均373508字节。本次构建版本0.1.0不是板上安装确认。修改前证据保存在 `build/b1_startup_20261102/before/`；修改后回归日志在 `build/b1-hal-tick-test/`。

## 实板下一步

1. 在STM32CubeProgrammer重新打开对应ELF，避免使用旧标签页缓存；烧录、校验并复位。
2. 确认串口继续出现 `RTOS create/start`、`lwIP init start`、`lwIP init returned; link requires status check`，且不再约33秒循环启动。
3. 发送本地诊断 `status` 并带换行，核对 `[net]` 的IP、up和link，再验证ping及TCP 8888。
4. 若启动已通过但link为0，再检查PHY供电/时钟、复位和飞线转RJ45的连接；不能把本次软件修正视为PHY及线缆验收。

本轮未连接目标进行调试、未暂停/复位/烧录MCU，没有启动算法、改变电源或修改PC网络配置；修正固件待用户重新烧录验证。未提交或推送仓库。

## 后续实板反馈与只读诊断

用户重新烧录后，09:38:10进入RTOS，09:38:11完成lwIP并输出状态。开机1.5秒快照为IP=192.168.1.201、up=1、link=0、power=OFF；这是当时状态，不能视为持续断网。09:39:13、09:41:45、09:41:53、09:42:07及09:42:32的PC日志已成功收发心跳、0102信息及0504电源OFF，证明网络曾正常工作。

其后0502及只读0503均发生连接关闭，09:44:50起连接超时。串口log mcu/status无确认，补发含CRLF的HEX命令仍无响应。当前TCP代码不因普通空闲主动断连；工具的“电源可能仍ON”是失联通用提醒，不是ON状态实测。用户说明RJ45转换带变压器、RJ灯未亮。

获用户授权后，仅尝试一次CubeProgrammer HOTPLUG只读RAM（无halt/reset/write）。探针54FF6D067182505239431087报告参考电压0.76V，随后无法读取Core ID，未取得RAM。进程已退出、释放探针。该读数只能证明探针看到的参考电压异常，不能区分板上供电低与VTref/GND线接触不良。需先量3V3_MCU与烧录器参考输入对GND的电压，再继续当前现场检查；尚未定位后续串口/网络故障原因。
