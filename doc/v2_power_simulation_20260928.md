# V2 电源/测量模拟 + 真实 RK 联调

## 范围

以下电压请求示例已按2026-10-12的新字符串mV合同更新，V2当天已安装配套0.1.34并健康确认，不能发送给旧Steps固件。当前进度以[任务清单](driver_tasks.md#current-status)为准，修改和安装范围见[电压字段记录](pc_voltage_mv_20261012.md)。

历史安装记录（2026-09-29 09:20）：当时开发板已安装0.1.21，所有PC响应不再附加`simulation`字段。当时MCU→RK串口心跳间隔为2秒，PC主动心跳的应答方式不变；这些描述不作为当前板上版本或心跳间隔的证据。

0.1.21已包含`0105.testTimeMs`，以下结果示例包含该新增字段。代码及离线结果路径已验证；本次升级未下发算法，真实RK完成后转发耗时的实板验收尚未执行。

沿用 V2 的正式 TCP 服务、命令表、RK任务、信息查询、LED和OTA；不是另一套服务器。显式启用 `ENABLE_V2_POWER_SIMULATION` 后，仅电源、Boot配置和测量使用内存模型，**不写TPS/DAC、不切实际电源、不重启RK、不制造RK结果**。B1不启用，默认构建仍关闭该选项。

所有PC响应均不附加`simulation`字段，PC无需发送或解析这个字段。当前联调配置仍使用模拟电源/测量，RK算法及结论仍来自真实RK；无标记不等于实测数据，显示/导出时按本次安装配置区分。固件/FRU信息仍从W25读取，心跳仍为 `{}`，LED和升级仍执行原业务。

## PC 操作

| 命令 | 此配置下的行为 |
| --- | --- |
| `0103` 开始测试 | 校验原有JSON、设置虚拟训练目标和Boot；真实RK队列受理后回 `0006/accepted`。仅支持现有FUNC算法1–20。 |
| `0105` 结果 | 只有匹配的真实RK最终结果才产生；`testStatus/resultDesc/testTimeMs`来自RK，18项`measurementData`为本次模拟统计。 |
| `0501 / 0502 / 0505` | 虚拟上电、下电、断电重启；真实RK运行状态不改变。初次上电回accepted，下电/循环完成回completed；OFF时0505回invalidState。 |
| `0503 → 0504` | 返回虚拟`powerState`和`powerCycleCount`（仅模拟重启计数，MCU复位后清零）。 |
| `0506 / 0507 → 0508` | OFF时设置虚拟Boot；查询模式及目标mV，不写/读真实DAC。 |
| `0509` | 四字段为memoryType及vdd1/vdd2/vddq字符串mV；VDD1步进10mV、VDD2/VDDQ步进5mV。OFF只能填写所选类型标称电压，ON只能同类型调压。不是硬件安全范围验收。 |
| `0601 → 0602` | 查询最新模拟快照和本次累计统计；测试期间可反复查询，不因算法在途返回busy。 |

真实RK应已独立启动并处于空闲。`bootMode`只设置虚拟配置，不能让真实RK切换介质。首次/虚拟OFF、训练目标/Boot改变、当前拉偏不符或`reboot:true`，均增加一次虚拟冷重启；不能用它证明RK训练成功。

开始测试示例（直接发送绝对mV字符串，须符合10/5/5mV原生步进）：

```json
{
  "algorithmType": "functional",
  "algorithmName": "gtest-stuck-address",
  "algorithmIndex": 1,
  "power": {"memoryType":"LPDDR4X","vdd1":"1800","vdd2":"1100","vddq":"600"},
  "reboot": false,
  "bootMode": "emmc"
}
```

也保留`power:null/bootMode:null/reboot:false`，按外部已启动处理、不计虚拟冷重启，采用当前虚拟目标生成测量。Parametric/IDD和SLT仍回notSupported，不能靠模拟测量冒充RK已经实现这些算法。

## 测试期间轮询

20个FUNC开始包与0601查询HEX统一见[算法发送包清单](func_algorithm_network_packets.md)：LPDDR4X前10项三轨+1档（1810/1105/605mV），后10项三轨-1档（1790/1095/595mV），不逐算法累加。此原生步进定义于2026-10-12更新，下方旧安装记录仍描述当时的20mV固件；同一连接中查询、心跳和开始命令共用PC递增发送Seq，算法index不随查询改变。

1. PC收到`0006/accepted`后，可先按1秒一次发送`0601 + {}`；心跳、其他只读查询也继续可用。
2. 模型由TCP任务有界轮询约每500ms更新一个样本；PC查询只复制快照，查询快于采样时会重复看到同一数据，不重复计入统计。任务阻塞时不补造遗漏样本，不声称精确采样率。
3. **只有新的0103被成功受理才清零统计**。重复开始、调压、上下电、修改Boot及升级写操作在测试期间回busy；查询不能中断测试。
4. RK最终结果使统计冻结为complete；发送失败/显式等待超时等为cancelled，返回0006失败，不制造0105/PASS。断网不取消RK，统计继续；旧0105不转给新连接。
5. `0602.statistics`与`0105.measurementData`使用同一组累计值；0602还保留最新值，完成后最新值可继续变化，但累计结果不变，直到新测试受理。

0602示例（所有测量单位为mV/mA/mW；示例值不是实测）：

```json
{
  "sampleAgeMs": 0,
  "samplePowerState": "on",
  "validMask": 3,
  "voltageMv": [1800,1100,600],
  "currentMa": [300,140,85],
  "powerMw": [540,154,51],
  "statistics": {
    "state": "running",
    "voltageAvgMv": [1800,1100,600],
    "voltagePeakMv": [1810,1110,610],
    "voltageMinMv": [1790,1090,590],
    "currentAvgMa": [300,140,85],
    "currentPeakMa": [330,155,95],
    "currentMinMa": [270,125,75]
  }
}
```

从未开始测试时`statistics:null`；虚拟OFF时`validMask:0`且省略当前值数组。统计为整数算术均值，向零截断，最大/最小值仅为样点极值。一次窗口复用既有统计上限约24.8天，超过为timeout；这是联调模型，不是JEDEC IDD测试窗口。

0105示例（只有真实RK首错地址为0时才能生成这里的PASS）：

```json
{
  "algorithmType": "functional",
  "algorithmName": "gtest-stuck-address",
  "algorithmIndex": 1,
  "testTimeMs": 42000,
  "testResults": [{
    "siteId": "",
    "mirrorIndex": 0,
    "testStatus": 1,
    "resultDesc": {"firstErrorAddress":null,"errorType":null},
    "physicalAddr": "",
    "measurementData": {
      "Vdd1Ave":1800,"Vdd1Peak":1810,"Vdd1Min":1790,
      "Vdd2Ave":1100,"Vdd2Peak":1110,"Vdd2Min":1090,
      "VddqAve":600,"VddqPeak":610,"VddqMin":590,
      "Idd1Ave":300,"Idd1Peak":330,"Idd1Min":270,
      "Idd2Ave":140,"Idd2Peak":155,"Idd2Min":125,
      "IddqAve":85,"IddqPeak":95,"IddqMin":75
    }
  }]
}
```

`siteId`仍读取W25站点标签；无B2/芯片映射时保持0和空字符串，不借模拟伪造编号。

`testTimeMs`是RK上报的整个算法耗时，单位ms，位于`algorithmIndex`之后；PASS和FAIL均带回，不等于PC通信等待时间或测量统计窗口。0是有效值，以uint64十进制JSON整数原样输出。

## 网络助手完整HEX

安装本配置并确认RK空闲后使用TCP Client连接测试地址`192.168.1.201:8888`。按十六进制发送，复制为**单行、无空格**；不要只发JSON。以下发送序号为示例1/2/3，PC实际每发一帧独立递增。解析接收时按CommandID区分0602和异步0105，不能假定下一包必定是轮询应答。

0103，上面的完整电源示例，213字节：

```text
000000D500010008000001030000000100007B22616C676F726974686D54797065223A2266756E6374696F6E616C222C22616C676F726974686D4E616D65223A2267746573742D737475636B2D61646472657373222C22616C676F726974686D496E646578223A312C22706F776572223A7B226D656D6F727954797065223A224C504444523458222C22766464315374657073223A302C22766464325374657073223A302C22766464715374657073223A307D2C227265626F6F74223A66616C73652C22626F6F744D6F6465223A22656D6D63227D
```

0601查询，20字节：

```text
0000001400010008000006010000000200007B7D
```

0503查询虚拟电源，20字节：

```text
0000001400010008000005030000000300007B7D
```

## 构建与初版修改记录（0.1.18）

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-build -Version 0.1.18 -Configuration Release -PowerSimulation
```

此命令只构建。继续使用既有OTA安装与备份流程，下载前须确认RK空闲；不能覆盖完整B或绕过启动日志直接刷App。普通构建/未加`-PowerSimulation`时此功能关闭，恢复普通版本须使用更高版本号。带模拟产物位于`build/ota-app-power-sim-0.1.18-release-a1/`，与普通产物分开，任务/网络服务并未拆成两套。

本轮修改：新增`v2_power_sim`内存模型；正式PC命令入口按构建选项切换电源依赖；测试服务仍投递真实RK队列；复用`idd_stats`累计并新增提前结束接口；模拟响应容量扩为1024字节，PC完整PDU上限仍4096；新增模拟来源标记、OTA维护互斥和构建开关。

离线开发阶段验证（以下仅为离线范围，后续实板结果见文末）：

- 实际C/QEMU：100次在途轮询不busy、不重复计样；18项最终统计与0602一致；新受理清零、结束冻结；8小时及毫秒时基回绕；拒绝/队列失败无硬件副作用；真实RK结果入口、PASS/首错地址、断线归属、超时恢复及独立Seq回归通过。所有电源/DAC/真实采样桩一旦被调用会立即失败。
- 原模式与B1条件门禁回归、PC开始请求解析、UART/IDD窗口、PC协议24项、代码边界19项、OTA工具离线用例通过。
- 模拟Release A1/A2逐字节修正及反向还原通过；App为147924字节，1483处地址修正，`.ota`为153972字节；构建无编译警告。
- 关闭模拟的普通Release双槽也通过逐字节修正及反向还原；App为146872字节，1433处地址修正，未打包为本轮联调安装包。
- 此离线阶段未连接串口、TCP或ST-Link，未烧录、未同步B1/Word/飞书、未提交或推送；当时未重新读取实板版本。后续0.1.18安装及网络查询实测见下文，不将离线用例等同于实板结果。

回归入口：`python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation`；去掉选项验证普通模式。数据生成与统计是联调替身，不验证真实电流、电压、采样率、安全掉电、启动介质或训练时序。

## 2026-09-28 14:30 实板升级被安全拒绝

用户确认目标为MCU后，网络只读查询确认板上0.1.17、confirmed=true、activePackageId=8302B802。先留存与该版本对应的既有OTA包、ELF、清单及本轮查询记录到`build/v2-power-sim-0.1.18-upgrade-backup/`；这是构建产物留存，本轮没有整片读取内部Flash或W25。原运行槽仍由完整OTA保留，未覆盖。

随后申请安装模拟版0.1.18（packageId=A293ECE9，153972字节）时，`0901/mcu_begin`收到`0006/failed/busy`，立即停止，**没有发送mcu_write或mcu_finish，没有复位、没有访问ST-Link或串口**。本机TCP连接已关闭，仍运行原0.1.17。

补充无副作用核对：0503返回电源off；预留只读动作`rk_query`也返回busy，而该动作正常应由预留处理器返回notSupported且不访问RK/Flash。对照0.1.17的公共在途拦截，说明MCU仍保留测试忙碌状态；这不证明RK实际上仍在运行。须用户确认RK已结束/停止，并明确允许复位MCU清理状态后再升级；不绕过门禁或自动重试。

证据：`build/v2-power-sim-preflight-20260928.json`、`build/v2-power-sim-upgrade-20260928.json`、`build/v2-power-sim-busy-check-20260928.json`。待装包SHA256：`81528409FEA83557A821E21B47D4446E1738B7BB6C6282101AC2105F51968D22`；留存的0.1.17包SHA256：`933E4FC19569F61EEB701687D5B49DB3E5A76F9A582475DCCDEEB4610205EE87`。

## 2026-09-28 14:32 网络升级与模拟电源查询通过

用户再次要求“现在升级看看”后，沿用正常网络OTA重新申请，同一0.1.18包已被受理。按3000字节块上传全部153972字节，提交READY后由Boot安装；30秒健康确认通过，随后从W25查询返回`firmwareVersion:"0.1.18"`、`state:"confirmed"`、`confirmed:true`、`packageId:"A293ECE9"`，心跳正常。没有绕过busy，也没有使用ST-Link或串口；受理成功不证明前一次RK测试通过，未追认其结果。

新固件随后完成20次实际TCP交互：

| 检查 | 实际结果 |
| --- | --- |
| 版本、心跳、0503 | 0.1.18已确认；初始虚拟OFF、循环计数0 |
| OFF时0601、0507、0702 | validMask=0、statistics=null；Boot为notConfigured；LED状态可查询，未修改LED |
| 0501、0509 | 虚拟上电成功；LPDDR4X档位(+1,-1,+1)设置成功，目标1820/1080/620mV |
| 连续8次0601 | 三轨分别覆盖1810/1820/1830、1070/1080/1090、610/620/630mV，跟随目标±10mV变化 |
| 电流与模拟来源 | 电流保持预设波形范围，不按真实RK负载或电压计算；标记power=true、measurement=true、rk=false |
| 结束清理 | 0502恢复虚拟OFF，0509恢复LPDDR4X三轨0档；最终0503=off、循环计数1，心跳正常，TCP连接已关闭 |

这次没有发送0103或任何RK算法，因此statistics保持null；真实RK四TLV结果到0105、真实算法运行期间的连续查询与最终统计仍待实板闭环验证。电源动作、测量值均为联调模拟，不是TPS/PAC/AD实测。正式TCP、RK等任务继续使用原任务入口，模拟模块按本构建选项接入；其他自检/波形及旧RK模拟开关未打开。

完整升级记录：`build/v2-power-sim-upgrade-retry-20260928.json`。网络查询检查退出码0、断言通过；`build/v2-power-sim-network-20260928.log`保留终端输出及8组实测读数，但中段输出有截断，不是完整逐帧抓包。板上现为0.1.18模拟配置，虚拟OFF/标称目标，未发送新测试；未同步B1、Word/飞书，未提交或推送。

## 2026-09-28 RK心跳2秒版升级

用户要求把心跳改为2秒并烧录MCU。本次按当前串口联调话题处理MCU→RK方向：检查时`RK_HEARTBEAT_INTERVAL_MS`已为2000U，保留此配置，补齐中文说明及仍按1秒判断的既有回归；不新增任务，不改变PC心跳、算法超时、RK结果判据、测量节奏或采样查询。B1独立工程未修改。

验证先复现旧用例在1100ms期望发送而失败，再通过更新后的实际C/QEMU：V2固化2000ms合同；验证到期前1ms不发送、到期仅尝试一帧、失败等下一周期、错过周期不补发、tick回绕、心跳不改在途请求，以及连续RX/GAP时仍推进心跳和OTA健康。本源码B1条件门禁、PC/RK电源模拟和连续查询回归均通过；不是独立B1镜像验收。

随后构建0.1.19 Release并验证A1/A2逐字节地址修正及反向还原：App147924字节，1483处修正，OTA153972字节。模拟配置保持开启，旧网关联调、器件Mock、自检及波形开关保持关闭。升级前确认板上0.1.18健康、虚拟OFF，预留rk_query正常返回notSupported而非busy；正常OTA门禁仍保留，没有强制清理测试状态。

- 15:54:08开始网络升级，3000字节一块上传，packageId=`5F73E7FA`；Boot安装后30秒健康确认通过，W25查询版本0.1.19、confirmed=true。
- 升级后0503返回off，0601返回validMask=0/statistics=null及power=true、measurement=true、rk=false标记；TCP心跳正常，连接已关闭。
- 原0.1.18包/ELF/清单及升级前查询记录留存于`build/rk-heartbeat-2s-backup-20260928_155312/`，这是构建产物备份；没有新读整片内部Flash/W25，继续依靠完整OTA的旧槽与日志保护。
- 新包SHA256：`84B22AF183053DEB1E88FBDC83F4ADF1CF385D5F1BC2C36E85389683D35705E5`。
- 实板证据：`build/rk-heartbeat-2s-before-20260928.json`、`build/rk-heartbeat-2s-idle-20260928.json`、`build/rk-heartbeat-2s-upgrade-20260928.json`、`build/rk-heartbeat-2s-after-20260928.json`。

本轮未连接串口/ST-Link、未下发测试算法、未切真实供电；串口物理发送间隔没有重新采集，2秒边界验证来自实际C离线回归。未同步独立B1、Word/飞书，未提交或推送。

## 2026-09-28 移除PC响应标记并安装0.1.20

按用户要求删除`v2_power_sim_tag`、共享标记函数及全部调用点；0006、0105、0504、0508、0602均不再追加`simulation`对象，不增加兼容开关。电源/Boot/测量内存模型仍开启，真实RK队列、四TLV结果匹配、2秒心跳、平均/最大/最小值、在途互斥及OTA门禁不变。当前示例与接手说明已同步，历史原始记录不改写。

验证与安装：

- 先在统一响应解码处增加缺省字段断言，旧代码实际C回归失败；移除入口后，模拟配置、普通配置、本源码B1条件门禁全部通过。模拟用例仍覆盖100次在途查询、18项最终统计、8小时/时基回绕和无硬件调用；0103受理/失败及0105的无标记输出均已离线执行。
- RK实际C心跳/事务回归、PC协议24项、代码边界19项、20组FUNC报文及3个本页JSON示例校验通过。Release A1/A2逐字节修正、反向还原及打包通过：App147724字节、1479处修正、OTA153756字节。
- 首次只读查询超时，未写Flash；用户断开网络助手后读到0.1.19健康、虚拟ON。确认无在途门禁、保存上一轮complete统计快照后，通过0502正常虚拟下电；没有停止算法、复位RK或切实际供电。
- 原0.1.19包、双槽ELF、镜像/清单及升级前状态留存在`build/pc-no-simulation-backup-20260928_162942/`。这是构建产物及查询记录备份，不是本轮整片内部Flash/W25回读；网络OTA仍使用原运行槽和启动日志保护。
- 16:30:14开始按3000字节分块网络升级，packageId=`3C855755`；Boot安装后通过30秒健康确认，0903及W25查询均为0.1.20/confirmed。
- 升级后20次TCP交互通过，覆盖0006成功/拒绝、0504电源状态、0508 Boot、0602 OFF/ON连续4次采样、LED只读查询、OTA查询与心跳，所有响应均无`simulation`字段。读数仍按标称±10mV及预设电流波形变化；未开始算法，statistics保持null。结束虚拟OFF、LPDDR4X标称目标、虚拟Boot=emmc，TCP已释放。

用户说明RK此时未桥接，因此本轮不发送0103、不验证真实RK到0105闭环，不把离线统计或模拟值称为硬件测量；未使用ST-Link或串口、未同步独立B1/Word/飞书、未提交或推送。

升级包：`build/ota-app-power-sim-0.1.20-release-a1/test.ota`；SHA256：`AA63A64516D94198ECC87B1884A91A1D335299864FBA5DDFC70ADEB793A0AC54`。证据：`build/pc-no-simulation-before-20260928.json`、`build/pc-no-simulation-before-retry-20260928.json`、`build/pc-no-simulation-idle-20260928.json`、`build/pc-no-simulation-upgrade-20260928.json`、`build/pc-no-simulation-after-20260928.json`。

## 2026-09-28 新增算法耗时与协议同步（未烧录）

- 正式`pc_test_service`将已有RK结果`elapsed_ms`传给统一组包函数，新增顶层`testTimeMs`；0006受理/失败不带此字段。模拟电源配置同样使用真实RK耗时，不产生虚拟测试时间。
- 实际C/QEMU覆盖普通及电源模拟结果、PASS/FAIL、在途轮询、统计冻结等；耗时0、42000、超过u32、超过double精确整数范围及UINT64_MAX均按完整十进制数字输出。本源码B1条件门禁、RK协议、PC24项、边界19项、20组发送示例、OTA字段合同检查通过；不是独立B1构建或真板验证。
- 离线构建`0.1.21`：App147900B，1480项地址修正，OTA153936B；A2逐字节及反向A1校验通过。候选为`build/ota-app-power-sim-0.1.21-release-a1/test.ota`，未发送到MCU，原0.1.20产物保留。
- PC协议Markdown与固定名Word同步；旧Word归档到`doc/history/pc_tcp/pc_tcp协议_20260924_171548.docx`。默认渲染器缺少LibreOffice，改用WPS隐藏空实例只读导出；检查19页，新增字段和OTA说明未溢出或截断。
- 飞书协议`GWdYdgSArojCm3xPAPzcJxbxnqh`仅局部更新0105示例、字段表和OTA `download`说明，版本141→144并回读核对。`download`仍由`pc_ota.c`在`mcu_query`且`receiving`时输出进度，网络验证脚本也读取`receivedBytes`，因此保留字段并纠正“已舍弃”；文件数据仍由`mcu_write.data`传送。未改OTA代码或其他在线章节。
- 本轮未连接TCP/串口/ST-Link、未烧录、未同步独立B1、未提交或推送。文档差异与飞书回读证据在`tmp/pc_test_time_20260928/`。

## 2026-09-29 0.1.21网络升级与健康确认

- 用户确认断开网络助手、RK空闲并允许升级。升级前W25查询为0.1.20/confirmed，包标识`3C855755`；只读预留`rk_query`按合同返回notSupported而非busy，0503为OFF。没有清理在途状态或改动电源。
- 复用已验证候选`build/ota-app-power-sim-0.1.21-release-a1/test.ota`，重算封装内容与既有包逐字节一致；SHA256为`A87C675E4241E9A41AABED7F701C39A5E7ADA7023BF735E0E957BE974918E376`，packageId=`60BAEF0B`。保留原0.1.20构建包和ELF；本次未另做内部Flash/W25整片备份，正常网络OTA依靠既有旧槽及启动记录保护。
- 09:18:14开始升级，blockSize=3000，全部153936字节提交成功；末块offset=153000、nextOffset=153936。Boot自动安装后，新App通过30秒健康确认，目标包查询返回0903/confirmed/0.1.21，心跳正常。
- 09:20:01独立查询确认W25版本、当前包一致；0503为OFF/powerCycleCount=0，0602为OFF/validMask=0/statistics=null，0102板卡信息正常，0703三组结果灯为off，0001心跳正常。未设置Boot/LED、未下发0103/算法、未打开串口或ST-Link、未更新RK固件。
- `testTimeMs`随0.1.21安装，但本次没有真实算法结果，不能把安装健康或查询通过写成结果转发已实板验证。电源/Boot/测量仍是V2内存模拟，RK保持真实链路，不代表真实TPS/PAC/AD验收。
- 本轮连接均已通过上下文退出关闭，未提交/推送、未同步独立B1。证据：`build/pc-test-time-before-20260929.json`、`build/pc-test-time-idle-20260929.json`、`build/pc-test-time-upgrade-20260929.json`、`build/pc-test-time-after-20260929.json`。
