# B1真实联调路径与V2同步（2026-09-29）

## 本次接入

用户确认先接真实驱动、采样及已启动RK的算法链路，自动冷重启继续安全拒绝。

| 入口 | B1执行及响应 |
|---|---|
| 0103，外部就绪模式 | OTA健康确认通过、电源ON、无在途算法/升级时，按名称表下发真实RK算法1..20，回0006 accepted |
| 0103，power或bootMode非null、reboot=true | 不操作电源/Boot/RK，回notReady；健康未确认时优先otaNotReady |
| 0601 | 返回最近真实快照及当前statistics，测试中允许连续查询；查询不会新增样本 |
| RK最终结果 | 四TLV全部有效且Seq＋算法匹配后，原连接收到0105，带testTimeMs、首错地址和18项measurementData |
| 算法在途 | 新测试、电源修改、OTA写操作回busy；断线不撤销算法，不自动重发 |
| TCP异常输入 | 同步V2的有界找头、完整坏JSON失败应答、半帧超时保护，不改18字节头及JSON字段 |

准备好真实RK后，PC使用现有请求（仍需外包18字节TCP头）：

```json
{"algorithmType":"functional","algorithmName":"gtest-stuck-address","algorithmIndex":1,"power":null,"reboot":false,"bootMode":null}
```

没有自动DDR-ready判断；ON只说明MCU电源流程状态，操作者仍须确认RK已进入测试程序。IDD21..36、parametric及SLT不因本次接入而开放。

## 真实测量与保持的硬件边界

- 沿用B1逻辑I2C4＋SPI4/PE11监测，PAC使用20mΩ板级参数。没有V2虚拟波形，也没有新增simulation字段。B1构建显式拒绝两个V2模拟开关。
- 每次成功开始新算法时清空统计；仅新完成的实际采样计入。领取最终结果时冻结；掉电/传输失败/显式超时取消窗口。有限超时后仍保留RK未知状态，不能直接发下一算法。
- 六组统计为三路电压mV和电流mA的样本算术平均、样本最大和最小值；无有效样本的字段为null。当前后台间隔约2秒再加器件IO，不能当作高速瞬态峰值或JEDEC规定的IDD窗口。单窗口上限INT32_MAX毫秒（约24.8天），算法等待默认不设超时。
- 两颗DDR共用三轨时统计属于整板负载，不凭die或首错地址拆分两颗电流；siteId读W25，mirrorIndex=0、physicalAddr空串继续表示未分配。
- 保持PC6/PC7系统轨、PG11可选使能、PG12 PHY复位、SPI2仅B2保活、UART4/USART2候选路由，不用V2引脚覆盖。
- `pwr_ctrl_init(NULL)`仍保持默认OFF及未配置时序门禁；未经批准的毫秒值没有填入。首板须先验证并配置现有`pwr_ctrl_configure()`时序，再通过正常上电流程到ON。本次不保证空板一烧录就能直接发0103。
- 正式IP由B2告知的决定不变；当前本地地址仍只用于测试，没有擅自增加SPI配网协议。

## 已验证（全部离线，无B1真板）

| 检查 | 结果 |
|---|---|
| B1常规Debug及46引脚/板级静态合同 | 通过 |
| 真实业务源码C/QEMU | 非ON拒绝、ON受理、四TLV到0105、断线及超时恢复、连续100次0601查询、统计冻结/重置、部分测量失效、最大值响应容量通过 |
| 两工程PC开始请求/帧恢复、RK、monitor实际C | 通过；不是线上网络或真实串口验证 |
| 两工程AD、PAC、NOR/DAC/PHY/MAC和UART/统计实际C故障回归 | 通过；PCF仅V2文件被测，B1没有PCF硬件 |
| V2普通/模拟配置业务、PC合同24项、分层19项 | 通过，保留V2模拟路径，不混入B1 |
| B1 Release UART4及USART2，A1/A2地址修正 | 两种均143932字节、1457处修正，正反向逐字节一致 |
| V2模拟Release，A1/A2地址修正 | 148036字节、1480处修正，正反向逐字节一致 |
| App OTA真实C后台 | 两板均通过禁用入口、下载完成、部分写失败、坏包、提交不确定和RK健康缺失/停滞7类检查 |

本轮离线候选版本号0.1.23，B1 UART4包149876字节，V2模拟包154072字节。未烧录；V2板上最后确认仍为0.1.22，本轮没有重新读取板卡。

复跑入口在V2仓库，B1不复制V2测试任务：

```powershell
# 在D:/pro/test中运行；compiler指定本机STM32 GCC绝对路径。
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1
python -B -X utf8 tests/verify_pc_start_c.py --app-root ../b1 --compiler <GCC路径>
python -B -X utf8 tests/verify_monitor_boot.py --app-root ../b1 --compiler <GCC路径>
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root ../b1
```

同步前19个B1文件已在本机`D:/pro/test/build/b1-presync-20260929-165858/`逐个备份并校验SHA256；只覆盖经差异核对的公共模块，板级文件以局部补丁接入。未连接ST-Link、串口或MCU网络；不更新Word/飞书。
