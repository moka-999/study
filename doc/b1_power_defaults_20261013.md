# B1首板电源时序初值（2026-10-13）

> 本文保留首次只配置低层时序时的记录。同日后续已启用业务初值、普通心跳就绪及直接下电，训练安全范围交由PC确认，见[后续业务配置](b1_power_business_20261013.md)。下文“业务仍空置/必须安全下电应答”不再适用；10/5/10ms低层初值未变。

## 本轮修改

按用户要求先选定可调的开发初值，不再让B1低层轨间配置一直为空。参数集中在 `D:/pro/b1/Core/Inc/board_power_config.h`，`main.c`装载它；复用现有 `pwr_ctrl` 和默认任务，不新增任务、不在MCU启动时自动上电。V2驱动和电源模拟配置不变。

| 阶段 | 动作 | 联调初值 |
|---|---|---|
| 初始化/复位后的App入口 | PC6、PC7、PG11全部拉低，状态OFF | 只装载参数，不写TPS |
| 受理上电 | PC7拉高，开启外设侧`VCC3V3_SYS` | 等10ms |
| 开启RK/TPS共同供电 | PC6拉高，开启`VCC4V0_SYS`；随后PG11防御性拉高 | 再等5ms |
| 配置训练电压 | 调用现有`ddr_power_init()`，直接写保存的三轨目标 | 全部写成功才ON；首错立即全关并进入FAULT |
| 正常下电 | 上层取得RK安全掉电确认后，先拉低PG11及PC6 | 等10ms，再拉低PC7并进入OFF |
| 启动途中取消/驱动故障 | PC6、PC7、PG11全部拉低 | 不等待正常下电间隔 |

这些是**首板联调初值，不是JEDEC规定、硬件批准时序或已测得的稳定时间**。等待从实际发出GPIO动作的时刻计起；默认任务约1ms轮询，但会有调度/锁等待，5ms指开始配置前的最短软件等待，不是保证三轨在5ms内全部稳定。PC7先开不能延迟RK训练；PC6一开，RK806和TPS便共同启动。

## 选择依据与原图边界

核对 `MYZR-RK3588-ZUOYI_SCH_20260920-2134.pdf`，SHA-256为 `EA886788A1C356EB571AE9243F9E5A201090DC1D87420C2A0E5C449B03FA7D1C`。原文件在 `D:/Backup/Documents/xwechat_files/wxid_ucuzmmfoa6i112_4de2/msg/file/2026-09/`。

| 原图页 | 已确认路径 | 对时序的影响 |
|---|---|---|
| P17、P15、P20 | PC7→U4004 RT8070 EN→系统3.3V，供外设/接口；不是MCU供电或RK核心电源 | 先建立外设电源，把这段等待放在共同4V开启之前 |
| P18、P12、P22 | PC6→U4003 MP8759 EN→4V，同时供RK806与TPS | 4V开启后只保留初始可访问等待，尽早写TPS，不额外等待500ms |
| P22 | TPS EN上拉4V，PG11串联位NC | PG11不能保证真实关轨，也不能暂停TPS/RK启动 |
| P17、P21 | 12V→5V→`3V3_MCU`独立供电 | MCU可先准备配置，再通过PC6/PC7启动系统 |

原图未给出PC6/PC7的强制顺序或毫秒值。参考[MPS MP8759数据手册](https://www.monolithicpower.com/en/documentview/productdocument/index/version/2/document_type/Datasheet/lang/en/sku/MP8759GD-Z/document_id/1011/)中的典型软启动约1.2ms，以及[TI TPS62868/69数据手册](https://www.ti.com/lit/ds/symlink/tps62869.pdf)§8.3.4的使能延迟后I2C可访问机制，选5ms作为初始等待；这不是两颗器件串联启动的最坏情况证明。RT8070软启动由外接电容决定，10ms也不是对实际板卡的测量结论。

首板必须检查：外设3.3V先于RK供电时的IO反灌、PC6复位高阻期EN偏置、实际TPS输出选装、三轨爬升/掉电顺序、I2C可访问时间、全部写入及稳定是否赶在DDR训练之前。10ms反序关断间隔不替代冷重启的完整放电等待；如果窗口不够，不能只把等待加长，更不能把失败写成训练成功。

## 仍未放开的业务配置

这次完成的是 `pwr_ctrl_config_t`，不是所有电源决策。B1仍调用 `pc_power_service_configure(NULL, NULL)`，训练范围、业务等待上限和RK确认适配器留给下一项。完整0103、0501/0502/0505仍可能按原门禁返回 `notConfigured/notReady`，不能把装载10/5/10ms解释为已经能通过网络安全执行完整冷启动。现有外部就绪模式、只读查询、Boot/调压条件、OTA门禁均不变。

不以心跳、固定延时、TPS写成功或状态ON代替RK-ready/安全掉电。PC JSON、UART TLV、V2 MATS假FAIL、B1真实测量及IP分配均未改变。

## 离线验证

- 新增B1 `tests/test_power_sequence.c` 与 `verify_power_sequence.py`：编译实际 `HAL_Drv/pwr_ctrl.c` 和同一份板级参数，QEMU执行；仅GPIO与DDR事务使用桩。覆盖默认OFF、无配置拒绝、10/5/10ms边界、反序关断、TPS失败全关、两个启动阶段取消、目标拒绝、32位回绕与延迟调度不抵扣下一阶段等待。通过。
- B1 `tools/dev.ps1 check`：49/49引脚及既有协议/Flash/工具合同、实际槽位读取、新电源回归均通过。
- B1实际PC电源服务QEMU回归：0103及0501/0502/0505、安全门禁、失败/超时/旧确认/断线收尾通过。此组使用明确注入的测试配置，不代表main中的PC业务配置已启用。
- B1 Debug构建通过，生成235620字节App；本轮仅普通构建，没有重做OTA发布包或双槽地址修正验证。
- V2既有电源/波形合同与4组模型状态场景通过；本轮没有修改V2固件行为。

复跑（从 `D:/pro/test`）：

```powershell
powershell -ExecutionPolicy Bypass -File ../b1/tools/dev.ps1 check
powershell -ExecutionPolicy Bypass -File ../b1/tools/dev.ps1 build
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1 --power-sequence
python -B -X utf8 tests/verify_pwr_ctrl.py
```

结果日志：`D:/pro/b1/build/power-sequence-test/run.log`、`D:/pro/b1/build/pc-rk-service-power-sequence/run.log`。没有连接板卡、烧录、重启、改变实际供电、提交或推送，也没有同步Word/飞书。真板尚未到货，以上均不是B1实测验收。
