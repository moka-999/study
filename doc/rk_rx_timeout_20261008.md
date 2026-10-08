# V2 RK串口半帧门限调整（2026-10-08）

## 修改范围

用户要求将串口分段等待放宽至100ms。本轮仅修改V2的`App/protocol/rk/state_machine.h`：`RK_BYTE_TIMEOUT_MS`由50改为100，保留既有状态机与正式RK任务。

- 相邻两次喂入解析器的字节间空闲小于100ms，继续拼接半帧；达到100ms清理半帧。
- 完整帧立即校验、交付，不额外等待100ms；没有增加阻塞延时、任务、队列或缓冲区。
- CRC、帧长、TLV和Seq校验不变；PC JSON及帧格式不变。
- RK心跳仍为1秒；`timeout_ms=0`仍无限等待算法结果，不能与半帧超时混淆。
- 代价是残缺帧的空闲超时恢复比原先晚50ms；间隔达到100ms仍会丢半帧，并非取消保护。
- 独立B1仍为50ms，本轮未同步；Word、飞书、Bootloader未改。

## 依据与证据边界

用户提供的两次MATS日志都具有完整、CRC正确的56字节最终结果，命令/结果Seq分别匹配；失败次5639ms，成功次5645ms。失败次桥接记录将结果拆成32字节和24字节，后24字节与16字节心跳一起转发；成功次一次转发56字节。

原50ms代码的离线故障注入能复现：分段间隔达到50ms时丢掉半帧，事务仍等待结果。原日志没有分段时间戳或MCU解析拒收计数，桥接分段也不等于MCU实际DMA分段，故尚不能确认现场一定由该超时导致。本次是用户批准的容错门限调整，不能写成实板根因已修复。

两次均有`0x5001＋0x3016`进度报告被当前完整详情校验拒收的问题。它是独立待修项，本轮没有顺带改变报告分类，也不将其作为最终结果丢失的已确认原因。

## 验证

1. 在原50ms配置下运行新增回归，`limit == 100U`按预期失败，确认测试能识别配置未改。
2. 修改后`tests/verify_rk_protocol_c.py`实际C/QEMU回归通过。新增MATS黄金帧的全部55个切分点、0/50/99/100/101ms间隔、主动检查/继续喂字节两条路径和tick回绕两种起点，共1100组；超时后下一完整帧仍可接收。
3. 保存的成功/失败两份混合串口流在100ms版本均可交付MATS结果，无须先更新心跳Seq。两份结果的770组切分检查通过：0/10/49/50/99ms接受，100/101ms丢半帧；实际32＋40字节桥接分段也通过相同边界检查。
4. `tools/dev.ps1 build`正式Debug构建通过，App为235416字节；该普通构建仅验证编译链接，不是已部署OTA板的下载包，不得直接覆盖板上App。
5. `tools/dev.ps1 style-test`分层/事件模型19项及纯接口头GCC检查通过。

复跑入口：

```powershell
python -B -X utf8 tests/verify_rk_protocol_c.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 build
powershell -NoProfile -ExecutionPolicy Bypass -File tools/dev.ps1 style-test
```

本机对照快照、原50ms诊断产物及新100ms回放源文件保存在`build/mats-log-review-20261008/`；`compare.elf`是原50ms产物，`compare_100ms.elf`为本次100ms产物，均仅供QEMU，不得烧录。

## 离线阶段边界与后续验收

上述修改及离线验证阶段没有连接串口、TCP或ST-Link，没有下发算法、复位或烧录。用户随后授权安装，结果见下一节；现场分段丢帧是否改善仍需真实算法复测，必要时补充分段时间戳和解析错误计数。

## V2网络安装（2026-10-08）

用户要求烧录后，先从W25查询确认板上为0.1.25、健康状态confirmed、包ID为BE884D0E。本机未发现其它到192.168.1.201:8888的TCP连接；保留的0.1.25包ID和构建配置匹配，确认该版启用了电源/测量模拟、未启用RK算法模拟。只读预留rk_query返回notSupported而非busy，说明MCU未保留在途测试标记，不据此宣称独立RK状态已被读取。

0503查询虚拟电源原为ON。经现有0502接口关闭内存模拟状态并复查OFF，未操作真实RK供电、DAC或TPS，未给RK发送算法或取消命令。

- 使用`tools/dev.ps1 ota-build -Version 0.1.26 -Configuration Release -PowerSimulation`构建。保留正式TCP/RK任务、真实RK链路和1秒心跳；仅电源/Boot/测量维持原模拟配置。
- App为148468字节，1481处地址修正；A1修正到A2逐字节一致、反向A1一致。包为154508字节，ID为`0D5DCFAB`。
- 复用日常OTA入口调用的`tests/pc_ota_network_test.py --upgrade`，以3000字节分块传送，Boot完成安装；目标包0903、30秒健康确认与TCP心跳通过。
- 安装后重新从W25查询为0.1.26/confirmed/0D5DCFAB；0503为OFF、powerCycleCount=0；0602为validMask=0、statistics=null；TCP心跳正常。所有本轮网络连接已释放。
- 未接入ST-Link或串口，未另读整片备份；网络升级依靠现有旧槽/启动日志保护，未更新Bootloader、FRU、稳定包或B1，也未提交、推送。

证据：`build/ota-preflight-20261008.json`、`build/ota-upgrade-0.1.26-20261008.json`；升级包和双槽校验报告位于`build/ota-app-power-sim-0.1.26-release-a1/`。升级包SHA-256为`edbb32d4e823e2ca57fb44aea7691f7c938e156ef5f0f18878882adc6a6ea1c3`。本轮验证确认安装和基本查询链路正常，没有运行真实算法，因此不宣称MATS现场丢结果已复测解决。
