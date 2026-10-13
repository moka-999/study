# RK双颗粒最终结果与PC上报（2026-10-10）

## 本次范围与结论

前一阶段仅完成纯解码。本阶段经用户确认，V2/B1正式RK任务、结果队列和PC 0105已整体接入3/5/7 TLV单包结果；siteId与共轨measurementData放顶层，testResults固定两颗。V2已安装0.1.33并通过健康确认；B1只做构建/离线验证。

V2保留真实RK执行、电源/测量模拟及每次MATS假FAIL：只将第一颗mirrorIndex=0的PC视图设为FAIL/SAF，第二颗保留RK真实结果。B1不含假FAIL。未启动真实RK算法，不能把本次安装当作双颗粒串口→TCP实测通过。

## RK单包合同

| TLV数量 | 内容 | 结论 |
|---|---|---|
| 3 | 5001 + 1017 + 9002 | 1017=0，两颗均PASS |
| 5 | 5001 + 1017 + 9001 + 3001 + 9002 | 1017=1，一颗FAIL，3001指定失败颗粒 |
| 7 | 5001 + 1017 + 9001 + 3001 + 9001 + 3001 + 9002 | 1017=1，两颗FAIL，3001分别为0和1 |

5001为u16LE算法，1017为1字节0/1，9001/9002为u64LE地址/耗时ms，3001为1字节0/1。完整帧分别39/56/73字节；外层字节序、CRC和Seq不变。

- 每个9001后立即跟所属3001；失败对可反序，按die匹配，不按数组位置猜编号。
- 算法/整体结论/耗时各一次，标量可换序；die不可重复或超出0/1。
- 1017=0不得含失败；1017=1至少一组失败。失败地址0原样保留。
- 残缺、重复、矛盾、额外TLV或错误宽度/偏移整包拒绝并清空解码输出。
- 旧四详情与独立1017不再跨帧拼接完成；进度仅接受5001+3016双TLV，100%不代替结果。
- Seq仍为本次命令至最近成功入UART队列Seq窗口，允许滞后/重复；必须匹配在途算法。
- 有效结果交付一次后结束在途；无效包保持等待。无自动ACK、重发或用心跳解除busy。
- 默认无限等待；显式超时后完整迟到包仅恢复未知状态，不补报成功。队列满保留结果副本。

## 代码与JSON

| 位置 | 修改 |
|---|---|
| App/protocol/rk/frame.c/.h | rk_final_result_decode原子解析0～2组失败，纯解码不涉及Seq/硬件/JSON |
| App/tasks/rk/rk_link.h、rk_task.c | rk_response_t传完整双颗粒结果；删除旧分包汇总状态，复用生命周期 |
| App/protocol/pc/pc_test_service.c | 失败按die归位，未列者PASS；V2 MATS仅改第一颗PC视图 |
| App/protocol/pc/pc_test_message.c/.h | 两颗结果、顶层siteId/耗时/共享测量；完整u64地址与耗时 |
| App/protocol/pc/pc_dispatch.h | 统一1024字节响应缓冲，容纳双颗、转义站点和18项统计；线上仍4096字节 |
| App/tasks/test/rk_gateway_test.c | 隔离联调适配器跟随新类型/JSON，本次安装未启用 |

0105根字段：siteId、algorithmType、algorithmName、algorithmIndex、testTimeMs、testResults、measurementData。
每颗含mirrorIndex、testStatus、resultDesc、physicalAddr；固定依次0/1，testStatus=0失败/1通过。
PASS地址null、errorType空串；FAIL地址输出0x+16位十六进制（包括零地址），真实RK未给错误类型时为空串。physicalAddr始终空串。
共轨18项统计只出现一次；部分缺失为字段null，完全无有效样本为measurementData:null，不改变统计来源/窗口或0602。

MATS假FAIL只影响V2第一颗：原PASS时补演示地址0x0000000012345678；原FAIL则保留原地址，包括0；errorType=SAF。第二颗无论PASS或FAIL均保留真实结论/地址。不是RK诊断出了SAF，重启不取消该构建开关。

本地PC Markdown示例及字段表已更新，两板通用源码已同步；B1差异仅保留真实硬件/采样门禁，没有V2模拟宏。Word及飞书未同步本次外层结构。

## 验证

- 两板实际C/QEMU：3/5/7黄金帧、全部切分点/损坏/截断、0/1/2失败、反序、零及UINT64_MAX地址/耗时、重复/矛盾/错误字段通过。
- 正式RK任务：有效最终包才完成，旧分包拒绝、算法/Seq窗口、重复、队列背压、超时恢复、掉电门禁及心跳回归通过。
- PC JSON：根/逐颗字段、四种PASS/FAIL组合、零地址、u64精度、最长转义siteId和18项测量通过；逐个JSON分配点故障注入无泄漏。
- PC-RK服务：V2普通、电源模拟、MATS模拟与B1真实路径通过。MATS覆盖20算法×4种双颗结论=80组，证明只改MATS die0且不修改原始RK结果；断网/超时/拒绝不制造结果。
- V2/B1 Debug构建、V2分层19项/公共头、PC合同24项、B1板级检查通过。
- V2安装配置Release A1/A2逐字节正反向修正通过：App150036字节、1486项修正；B1 Release145636字节、1470项修正，两槽通过。
- 无B1实板、无真实RK最终包联调；隔离脚本未在硬件运行。

复跑：

```powershell
python -B -X utf8 tests/verify_rk_protocol_c.py
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root D:/pro/b1
python -B -X utf8 tests/verify_pc_start_c.py
python -B -X utf8 tests/verify_pc_start_c.py --app-root D:/pro/b1
python -B -X utf8 tests/verify_pc_rk_service.py
python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation
python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation --mats-fail-test
python -B -X utf8 tests/verify_pc_rk_service.py --app-root D:/pro/b1
```

## V2安装证据

用户确认RK空闲、PC已断开后进行正式网络OTA：

| 项目 | 结果 |
|---|---|
| 升级前 | 0.1.32、包778AF5E3、confirmed；虚拟电源OFF |
| 安装后 | 0.1.33、包1287728B、confirmed；30秒健康及0903通过 |
| 升级包 | build/ota-app-power-sim-mats-fail-0.1.33-release-a1/test.ota，156096字节 |
| SHA256 | f2abf0933991638a0637cf51fed85937ae4202a2d0aa865557975ed694b394f2 |
| 配置 | OTA ON，V2电源/测量模拟ON，MATS假FAIL ON，隔离网关测试OFF，真实RK |
| 重连检查 | W25版本、JMCT-4000/D00/RK3588_test、OFF、采样无有效值、心跳正常 |
| 副作用边界 | 未下发算法/电源命令，不接ST-Link/串口，不重启RK；TCP连接已释放 |

报告：build/dual_die_before_20261010.json、build/dual_die_ota_0133_20261010.json、build/dual_die_after_20261010.json。升级期间的重连超时属于安装/健康等待，最终以0903与confirmed为准。

未提交或推送仓库。下一步由PC按新版0105读取顶层站点/测量并遍历两颗，在真实RK发出3/5/7结果时验收；MATS第一颗仍是联调假FAIL，不能用来判真实芯片质量。
