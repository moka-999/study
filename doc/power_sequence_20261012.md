# 正式电源编排接入记录（2026-10-12）

> 历史设计记录：2026-10-13已按用户确认改为“PC确认DDR安全训练范围、MCU校验TPS编码、本轮普通心跳作为初始化完成、直接关电”。B1业务初值和适配器已启用，当前规则及验证见[初版业务配置](b1_power_business_20261013.md)。下文安全关机握手、min/max白名单、配置仍为空等内容仅描述当时版本，不再是当前B1待办；原测试数据保留作为历史证据。

## 1. 本轮完成什么

V2与独立B1正式应用都加入 `App/protocol/pc/pc_power_service.c/.h`。它由现有TCP任务周期调用，复用监测层事务锁、PC6/PC7电源状态机、TPS训练电压和MCP4725 Boot驱动，不新增任务或协议字段。业务按阶段查表推进，不在命令处理函数里阻塞等待RK。

| 现有命令 | 已接入的实际路径 | 正常应答 |
|---|---|---|
| `0103` 完整开始测试 | 校验目标→必要时安全关机→断电等待→OFF写Boot→上电写训练目标→等待RK就绪→投递算法 | `0006 accepted`→`0006 completed`→真实RK结果`0105` |
| `0501` 上电 | 使用已保存训练目标，调用真实训练上电驱动；没有显式目标时为标称 | `accepted`→电源状态ON后`completed`；已ON直接`completed` |
| `0502` 下电 | 等RK明确允许掉电后调用真实下电驱动，等待状态OFF | `accepted`→`completed`；已OFF直接`completed` |
| `0505` 冷重启 | 保存训练目标、保留当前Boot设置→安全下电→断电等待→重新上电→RK就绪 | `accepted`→`completed` |

`0103 completed`表示启动准备完成且算法已进入MCU的RK发送队列，不是RK已经执行、不是算法PASS；仍须等待`0105`。RK队列拒绝则返回`0006 failed`，不自动重试。安全关机、驱动、超时等失败也是命令失败，不伪造DDR测试FAIL。

`memoryType`、三路训练mV、Boot任一变化，或上次成功记录失效，MCU强制重新建立启动状态，不因PC的`reboot=false`跳过。目标一致且本次运行期间已有明确RK-ready记录，才允许复用；`reboot=true`仍冷启动。独立`0506/0509`改配置前使复用记录失效，MCU复位后也不保留它。`0505`不把上次运行期拉偏当作新训练目标。

已有外部启动模式（`power:null/bootMode:null/reboot:false`）不改变；B1仍要求真实电源ON和操作者确认RK测试程序就绪。V2电源模拟仍走原模拟实现，真实RK和仅第一颗MATS假FAIL的范围不变；B1不包含模拟或假FAIL。

## 2. 尚待填写的板级配置

截至10月13日，B1 `Core/Src/main.c` 已装载 `board_power_config.h` 的联调初值：PC7先开，10ms后PC6开，再等5ms配置TPS；正常下电反序间隔10ms。V2低层仍使用 `pwr_ctrl_init(NULL)`，两工程均不自动上电。B1初值及验收边界见[增量记录](b1_power_defaults_20261013.md)。

两工程 `pc_power_service_configure(NULL, NULL)` 仍保留：业务等待上限、训练范围和RK确认适配器尚未填写。正式业务代码已接好，缺业务配置时仍返回`notConfigured`；有配置但缺少所需RK适配器时返回`notReady`。低层时序装载不等于全部业务可执行，也不虚构RK确认。

硬件联调时，在调度开始前、真实电源OFF的初始化位置分别提供：

| 配置 | 用途与限制 |
|---|---|
| `pwr_ctrl_config_t` | B1已有可调10/5/10ms初值，仍须真板验收；由默认任务推进，不依赖TCP的轮询周期完成轨内时序 |
| `pc_power_config_t.safe_off_timeout_ms` | 等待RK允许安全掉电的上限；超时不等于允许关电 |
| `transition_timeout_ms` | 等待低层上/下电状态完成的业务上限 |
| `ready_timeout_ms` | 等待本次启动RK明确就绪的上限；不是算法运行时限 |
| `off_hold_ms` | 确认低层OFF后追加的断电等待；实际放电情况需波形验证 |
| `boot_settle_ms` | Boot DAC写完后重新计时的稳定等待，不把I2C事务耗时算进去 |
| `min_mv[2][3] / max_mv[2][3]` | LPDDR4、LPDDR4X各自的VDD1/VDD2/VDDQ训练范围；必须满足10/5/5mV编码网格 |

三个timeout为1～INT32_MAX毫秒，两个附加等待可为0。电压表只实现逐轨上下限，所选范围内的组合也必须经过板卡/颗粒方确认；如果以后出现额外的三轨组合限制，需要补充对应条件，不能认为当前范围表已覆盖任意器件要求。TPS全可写范围或统一±10%都不是默认安全训练范围。

本轮未设置推荐工作范围以外的训练许可，也没有新增“跳过安全确认”开关。测试文件中的100/10/5ms和电压范围只用于软件故障注入，不是B1初版时序参数。

## 3. RK确认接入点

`pc_power_rk_adapter_t`只有两个立即返回的回调，不定义新的串口TLV：

| 回调 | 合同 |
|---|---|
| `begin(SAFE_OFF, token)` | 向RK业务发起安全下电请求；0表示请求被本地适配层受理，非0表示失败 |
| `begin(READY, token)` | 在上电前清除旧确认并开始跟踪本次启动；不能向尚未供电的RK发送请求 |
| `poll(event, token)` | 0等待、1明确确认、负数拒绝或通信失败；不得阻塞 |

`token`是MCU内部阶段令牌，不是PC/RK帧头Seq，也不要求擅自加到线协议。适配器须线程安全地将真实事件对应到本次阶段，并拒绝上一轮残留确认。**目前RK参考协议没有明确的安全关机/启动就绪业务合同，实际适配器尚未实现。**待双方明确消息或其他可信条件后，只接这个接口，不必重写整段电源编排。

心跳、UART有数据、进度100%、固定延时、仅TPS写成功或状态ON，均不能替代RK-ready/安全掉电确认。`0501 completed`仅说明电源状态机完成，不赋予后续完整测试复用“已训练”的资格。

## 4. 失败、断连与测量

- 预检查不改GPIO/I2C；缺配置、越界、非整档等先拒绝。收到accepted后才由轮询推进驱动；下电请求的RK回调也必须非阻塞。
- 安全确认在超时边界及之后才被轮询观察到时，按超时处理；旧token、错误阶段的确认不能放行。
- 等待安全下电超时不触发关轨；等待RK-ready超时不擅自切断可能仍在启动的RK。低层状态转换超时只结束本次业务等待，不取消已经开始的低层动作，后续必须查询真实软件状态并核对硬件，不宣称电源已OFF。
- 现有TPS初始化首错全关及故障状态保留。没有额外自动重试、强制关机或自动重发算法。
- 准备/算法在途时拒绝新的电源改动、测试及OTA写；查询、心跳仍能处理。OTA下载维护中也不启动新的电源流程。
- TCP断开后已经启动的电源流程继续收尾；若算法还没投递，则不再替失联PC启动算法。旧连接的终态不发给新连接。已经投递的算法沿用原断线归属规则。
- B1采样统计在真正投递算法前重置，不把冷启动等待算进算法统计；队列失败则取消统计。窗口仍是MCU侧软件窗口，不等于JEDEC严格同步IDD测量窗口。
- 组accepted/completed失败时不开始新的硬件/算法步骤；驱动失败、RK队列满有明确失败应答或连接错误，不悄悄继续发算法。

## 5. B1现场日志与验收

B1复用现有可切换日志出口，`log mcu`时可看到：

```text
[power] action=3 phase=waitRkReady ms=1234 error=none at=idle
[power] action=3 phase=done ms=2234 error=timeout at=waitRkReady
```

action为0上电、1下电、2重启、3测试准备；phase包括`waitSafeOff/waitOff/offHold/bootSettle/waitOn/waitRkReady/done`。`at`为失败阶段，`ms`为当前阶段进入时刻；内部诊断还保留RK令牌。日志拥塞可丢弃，不等待UART；V2不会将该文本混入RK协议口。

真板到后依次验证：PC6/PC7实际顺序及反序关电、三轨放电时间、Boot DAC稳定、TPS地址/写入/电压真值、共享4V上电到RK训练的天然窗口、明确RK安全下电和ready事件、各阶段故障与断线收尾。PC6同时供RK806和TPS，PC7不是暂停RK训练的开关；PG11选装链路NC时也不能当成可靠三轨开关。业务等待上限不保证TPS一定赶在DDRbin之前写完，必须结合波形和RK日志验收。

## 6. 本轮离线验证

已执行实际C代码的QEMU回归：V2/B1冷启动、相同配置复用、目标变化及主动重启、0501/0502/0505、安全确认及READY超时（含tick回绕/边界）、旧确认、DAC/TPS/队列失败、断连、应答内存不足。外部真实RK业务路径、V2电源模拟和80组MATS隔离回归通过；两板各1536个字符串mV目标及非法输入检查通过。

V2与B1普通Debug构建均通过；PC协议24项、分层19项、纯接口头、B1项目合同检查和两板实际监测/Boot事务回归通过。启用OTA的Release又验证了三种配置：

| 配置 | App字节数 | 地址修正项 | 验证 |
|---|---:|---:|---|
| V2非模拟 | 151744 | 1464 | 修正到A2与独立链接A2逐字节一致；反向A1一致 |
| B1真实配置 | 148300 | 1506 | 同上 |
| V2电源模拟＋MATS假FAIL | 152344 | 1513 | 同上，保留原联调配置 |

这些构建使用离线版本0.1.0，没有生成发布包或安装到板卡。报告位于两工程`build/ota-app-0.1.0-release-a1/relocation_report.json`及V2的`build/ota-app-power-sim-mats-fail-0.1.0-release-a1/relocation_report.json`；电源故障注入输出位于两工程`build/pc-rk-service-power-sequence/run.log`。

离线测试将硬件端口替换为可控桩，执行的是正式C编排/分发代码，**不代表引脚波形、模拟采样或RK真实确认已验证**。复跑入口：

```powershell
python -B -X utf8 tests/verify_pc_rk_service.py --power-sequence
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1 --power-sequence
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build
powershell -ExecutionPolicy Bypass -File ../b1/tools/dev.ps1 build
```

本轮不改变板上固件，不连接网络设备、串口或ST-Link，不提交/推送，不同步Word/飞书；本地Markdown记录此次软件接入和待验收边界。MAC/IP、B2 SPI合同、IDD/SLT算法支持范围未改。
