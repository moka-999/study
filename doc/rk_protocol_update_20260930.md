# RK协议增量：空心跳、进度和分包结果

## 确认来源与本次范围

依据RK工程师的新文档 `D:/Backup/Downloads/uart-rk3588-protocol.md`，以及用户在2026-09-30对完整Type、数值和Seq的补充确认。以最后确认的“RK所有上行统一使用最近收到的MCU Seq”为准，取代先前的测试Seq回显/心跳独立Seq约定。修改V2及独立B1的共享协议/业务代码，保留板级驱动、B1 OFF门禁、1秒心跳和原有PC JSON字段；不改Boot、W25布局、电源控制或RK算法。

| 上行内容 | 已确认格式 | MCU处理 |
|---|---|---|
| 普通心跳 | Length=0，零TLV，整帧16字节 | 完整检查双CRC/Seq/帧尾；接收也兼容旧0x0000空Value，本端只发送16字节格式 |
| 测试进度 | 0x3016，1字节uint8，0..100 | 单在途测试期间缓存参考进度及最近已收MCU Seq，新测试清零，不触发完成 |
| 最终详情 | 同帧0x5001/0x9001/0x3001/0x9002，各一次 | 命令、地址、die、耗时；检查接收Seq窗口及详情算法后缓存 |
| 最终结论 | 独立0x1017，1字节bool，0=PASS、1=FAIL | Seq可与详情不同；单在途收齐后一次性生成PC 0105 |

上表全部上行都填写RK发送时最近收到的MCU Seq，MCU下行命令与心跳共用递增u64流水号。MCU接收不要求等于命令Seq/自己最新发送值；仅过滤早于本次命令或超出最近成功入UART队列的Seq。这样允许心跳仍在传输时RK使用较早的已收Seq，uint64计数回绕也按窗口处理。该过滤不是完整防重放：同算法旧结果若重新标为当前窗口Seq，协议无法区别执行实例；未来若需要可靠重试/并行，须另定测试实例ID，当前不增加字段。

用户心跳样本 `5A 00 00 51 D3 00 00 00 00 00 00 00 05 80 08 A5`：Seq=5、HCRC=0xD351、FCRC=0x0880，独立CRC计算及实际C黄金帧一致。原粘贴末尾额外5A不属于这一帧。

## 实际修改

- `frame`与接收状态机允许Length=0，但仍拒绝1..3及大于1024；空帧成功返回0个TLV，负数才是解析错误。
- TLV规则表统一检查长度和值域；进度大于100、FAIL大于1、重复已知字段、残缺详情、混入下行命令均不作为有效上行报告。
- `rk_report_decode`解析单帧，不跨帧拼凑残缺的四详情；`rk_task`负责在发送窗口检查及详情命令匹配后，按唯一在途请求合并完整详情与FAIL，先后不限，Seq无需相等。
- 相同重复幂等；当前测试的重复详情或FAIL内容冲突时继续阻止下一测试，不猜测哪份正确。超时策略不变，B1确定掉电仍能解除未知状态。
- 缺少任一结果包时保持busy；timeoutMs=0仍无限等待，不重发算法，不用心跳、进度100或地址0补成PASS。显式超时后两部分迟到收齐只发RECOVERED，不再补报0105。
- PC的0105字段和顺序不变，testStatus按FAIL位映射为1/PASS或2/FAIL；首错地址为0仍为null，不会覆盖明确的FAIL。testTimeMs及18项测量统计保持原合同。
- 进度心跳不含测试请求身份，因此这里只记录接收时的参考进度，不能排除延迟旧进度；不据它控制电源、确认RK-ready或推进测试计划。本次未新增PC进度字段/命令。
- 原隔离网关联调适配器及辅助串口脚本同步为两包结果；未启用或运行该硬件测试镜像。
- 新增/修改注释使用中文；变更过程放本文件，不在业务代码里写历史里程碑。

## 验证与交付边界

先加入用户16字节心跳黄金帧用例，旧实现失败于打包长度断言；追加最新MCU Seq用例后，原命令Seq强匹配确实无法完成，修改后V2/B1实际C/QEMU回归通过。覆盖空帧CRC/截断/粘包、1..3非法长度、标量长度/值域、上报滞后于MCU最新发送、两包不同Seq、发送失败不扩大窗口、Seq回绕、进度100不完成、任意先后、缺包、重复/冲突、窗口外Seq/错算法、队列背压、显式超时后迟到恢复及B1 OFF门禁。

PC实际C参数/组包与服务回归覆盖V2正式路径、V2电源模拟和B1实际采样门禁；特别验证零地址加FAIL仍输出testStatus=2。UART路由/恢复/IDD统计回归同步16字节心跳。两板Debug构建、V2边界19项/PC协议24项及B1工程检查通过。

复跑入口（本机使用既有GNU工具链及QEMU，不连接硬件）：

```powershell
python -B -X utf8 tests/verify_rk_protocol_c.py
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root ../b1
python -B -X utf8 tests/verify_pc_rk_service.py
python -B -X utf8 tests/verify_pc_rk_service.py --power-simulation
python -B -X utf8 tests/verify_pc_rk_service.py --app-root ../b1
```

若python不在PATH，使用 `C:/Users/Administrator/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe`。参数/组包回归 `verify_pc_start_c.py` 和UART回归 `verify_uart_idd_c.py` 另显式传 `--compiler` 指向原GNU编译器。

代码阶段未连接串口/ST-Link，不运行真实算法。本轮用户随后要求烧录V2，并确认网络助手断开、RK空闲，已按下节完成升级。B1和旧首烧交付包不随V2升级改变，没有提交或推送。真实RK进度及分包结果仍须联调验收；离线通过不是实板算法通过。Word与飞书未更新本次RK增量；当前内部合同见 [protocol_spec.md](protocol_spec.md)，B1同步见 [B1修改记录](../../b1/docs/rk_protocol_update_20260930.md)。

## V2升级验收（2026-09-30）

- 升级前只读查询：0.1.24，activePackageId=016DC90E，state=confirmed、confirmed=true；0504为powerState=off，PC心跳正常；查询后关闭连接。
- 通过正式OTA入口安装0.1.25 Release，保持`ENABLE_OTA_BOOT=ON`、`ENABLE_V2_POWER_SIMULATION=ON`、`ENABLE_RK_GATEWAY_TEST=OFF`，即真实RK＋电源/测量模拟。两槽地址修正逐字节及反向验证通过：App 148468字节，1481处修正，包154508字节。
- 包ID=BE884D0E，SHA256=`827a9c19ef9dfa29cf76ebba1e6fc8aa9bf940535f6cf00f7a858c7a228af4f7`；每块原始数据3000字节，本轮完整PDU最大4089字节，不超过4096。
- 重启后按包ID查询获得0903：firmwareVersion=0.1.25、state=confirmed、confirmed=true；30秒健康确认与TCP心跳通过，升级进程正常退出，连接已释放。
- 升级包：[test.ota](../build/ota-app-power-sim-0.1.25-release-a1/test.ota)；实板升级证据：[network.json](../build/ota-dev/20260930-163852-434708/network.json)。没有连接ST-Link或串口，没有下发0103、重启RK或操作B2；新RK进度/结果实测待后续实际算法联调，不以升级成功代替算法验收。
