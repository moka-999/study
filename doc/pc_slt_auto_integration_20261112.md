# B1正式SLT自动接入（010A与RELEASE自动收尾）

2026-11-13：按用户确认取消B1正式SLT对`010B`的依赖，以EDK `RELEASE`自动收尾；本页当前合同覆盖11月12日的010B登记方案。`010A`字段不变，`010B`编号保留但不再支持，V2仍保留原010A/010B基线。本轮不修改上位机源码、EDK、前级、Boot或电源时序，不修改Word；代码、离线及实板状态分开记录于末节。

## 整轮与入口

同一PC连接先发`0106 {}`，随后由PC任意排列已支持的FUNC `0103/0105`、PARA `0108/0109`和SLT `010A`，允许同类或同名算法重复，包括连续SLT。硬件执行串行：FUNC等0105、PARA等0109、SLT由RELEASE及本地收尾完成后自动释放占用，不等PC解析报告。下一项算法执行可与上一份SLT报告解析并行；全部测试及结果解析完成后，PC把三类测试失败颗粒汇总进`0107.failChips`，MCU只按此数组设置最终灯态。查询、心跳及`0601`缓存采样可以继续。FUNC→SLT→PARA只是验证示例，不是固件规定的顺序。

正式SLT从PC `010A`进入，由测试服务持有本轮占用；串口`net rk`、`start rk`、`end rk`仍是独立调试入口，正式流程不调用或模拟这些文本命令，也不借它们的标称电压替代PC目标。已有调试会话与正式测试互斥。

要求`B1_SERIAL_NET_KEEP_TCP=1`。设0时旧独占IP交接只保留手动用途，正式`010A`返回`notSupported`，不能通过断开PC绕过归属检查。V2业务不由本次B1接入改变。

## 开始到完成准备

| 阶段 | MCU行为与响应 |
|---|---|
| 收到合法010A | 校验本轮、类别/名称、参数和互斥，返回`0006 accepted`，登记原连接为所有者 |
| 冷启动 | 走既有电源/Boot流程，以本次`power`目标设置训练电压；SLT每次冷启动 |
| 启动频率 | 本轮RK发`0x35`才回复Freq；RK不查询也继续，不等频率请求 |
| 普通时序 | 本轮SPL普通心跳后逐项发送非空普通时序，不等待RK设置应答 |
| 准备完成 | 普通时序发送完成后返回`0006 completed`，`requestCommandId=266`；这不是MemTest86完成或时序已生效的证明 |
| 进入后级 | 通过共享底层发送`0x003E`、完成EDK授权、协商数据速率并自动PXE；保留MCU同IP和TCP连接 |

`TimingParameters`必须为对象，可用`{}`表示默认值，也可部分填写。`Freq`仅用于启动查询，其余15项为SPL阶段主动发送；空串/省略不发送，不继承上一次参数。PC负责参数取值和训练适用性，MCU沿用格式、位宽及电压编码校验。

末条普通时序与003E之间保留原有20ms发送间隔。已准备完成的空普通时序SLT，频率快照保留到实际收尾，提前0107、被拒绝的010B或断线不会把稍后0x35的回复改成默认值。准备前发现EDK已运行时拒绝继续设置普通时序；prepared后60秒内未完成EDK授权按启动失败处理，MemTest86执行本身不设此超时。

下列为`010A`完整JSON结构示例，时序数值须由PC按实际DDR确认；PC固定18字节头的Length仍按实际UTF-8正文重算。

```json
{
  "algorithmType": "slt",
  "algorithmName": "memtest86",
  "algorithmIndex": null,
  "power": {
    "memoryType": "LPDDR4X",
    "vdd1": "1800",
    "vdd2": "1100",
    "vddq": "600"
  },
  "TimingParameters": {
    "Freq": "2133",
    "CASReadLatency": "36",
    "CASWriteLatency": "36",
    "tREFI": "4",
    "tWR": "40",
    "tRTP": "16",
    "tRCD": "35",
    "tRP": "40",
    "tRASmin": "81",
    "tRASmax": "160",
    "tWTR": "20",
    "tRFCmin": "729",
    "tFAW": "77",
    "tCCD": "16",
    "tRRD": "16",
    "BL": "8"
  },
  "reboot": true,
  "bootMode": "fspiM1"
}
```

## RELEASE自动收尾与下一项

EDK结束网络阶段后发送`RELEASE`。MCU等待同一会话的最终OK应答实际发完至UART TC，再自动走既有OFF流程；确认真实OFF、115200及串口解析器恢复后，解除该次SLT执行占用。这里的TC是MCU UART发送完成，不是新增RK应答。此流程不依赖PC提交结果，断线后也继续按同一边界收尾。

正常自动收尾不新增PC通知，也不发送`010B completed`；`010A accepted/completed`仍只表示启动受理/准备完成。PC收到下一项请求的`busy`时应等待后重试，MCU不缓存或静默丢弃该请求。未提前发0107时，收尾完成可直接接收本轮下一项FUNC/PARA/SLT，不重新发0106。

`RELEASE`仅表示EDK释放网络阶段。PXE启动失败或应用失败退出也会释放，不能把它当成报告全部上传成功或测试PASS。PC继续负责检查XML/HTML是否收齐、解析判定及本轮最终汇总；MCU不根据RELEASE伪造SLT结果。

- 最终OK未发完、OFF或串口恢复未完成时保持busy，不能因下一条测试请求到达就提前终止SLT。
- 提前`0107`不取消SLT、不提前释放占用；PC应在本轮全部结果解析完成后提交0107。
- PC断线不取消SLT，新连接不能认领旧测试；后续RELEASE仍自动完成收尾，不向新连接发送旧响应。
- RELEASE缺失或收尾失败时保留故障/占用边界，不自动重发算法，也不伪造完成；业务错误沿用`0006 requestCommandId=266 failed`向原连接单次报告。

## 010B保留编号

B1正式流程不再发送或等待`010B`。编号保留，合法JSON请求返回`0006 failed/notSupported`；不解析SLT结果、不修改当前算法、不触发关电，也不更新结果灯。晚到或重复的旧010B因此不会被当作下一次SLT的完成报告。帧、UTF-8和JSON本身非法时，仍按通用协议恢复及`invalidArgument`规则处理。

V2尚未接入EDK交接，仍保留原010A/010B实现；不能将B1的新收尾行为当作V2已同步。本轮未修改PC协议Word。

## 验证状态与入口

11月13日自动收尾调整已完成B1实际C离线回归、运输合同及完整构建/check；尚未烧录或实板验证。此前手动同IP链路的10.14秒下载、报告落盘及失败退出连接保持证据见[共存说明](../../b1/docs/net_coexist_4m_20261112.md)，不能算本次正式010A自动收尾实板通过。当前进度见[任务清单](driver_tasks.md#current-status)。

11月13日离线重点：不发送010B即可自动收尾，最终OK未TC禁止OFF、OFF及串口恢复未完成保持busy；同一0106内三类六种排列及连续SLT；010B在开始前、SLT中和下一项算法中均无副作用；提前0107、原连接断开后仍自动清理且不交付旧响应；空时序、无0x35继续、频率快照、故障与独占宏0门禁。沿用实际C的PC业务、会话、RK/UART合同及B1 `check`/`build`，不自动烧录。

### 11月13日新合同验证记录

- B1 PC基础实际C：`verify_pc_start_c.py --app-root ../b1`通过。
- PC业务实际C/QEMU共7组通过：[B1主整轮](../../b1/build/pc-rk-service-power-sequence-round/run.log)与同前缀`-slt-fault-off/transport/transport-after/power-off`四个独立故障进程，[V2真实电源整轮](../build/pc-rk-service-power-sequence-round/run.log)及[模拟整轮](../build/pc-rk-service-power-sim-round/run.log)；V2源码行为保持原基线。
- 运输实际C：McuOverlay共6项通过，保持RELEASE应答入队与UART TC分离、OFF及串口恢复门禁。
- V2 PC合同24项、代码边界20项及纯接口头独立编译通过。
- B1完整Debug构建与`check`通过。新BIN为413224字节，CRC32 `24495B37`；沿用当前本地配置`192.168.1.202:8888`，未改IP宏或B2正式运行地址规则。
- 未连接实板、烧录或验证新版全流程；下一步按无010B路径验证同轮连续算法及PC报告解析后0107灯态。

### 11月12日旧合同验证记录

以下是010B登记方案的历史验证，不代替11月13日新规则回归：

- PC实际C：B1基础、电源、整轮与四个独立故障进程，V2真实电源/模拟整轮共9组通过。主要日志为[B1整轮](../../b1/build/pc-rk-service-b1-guard-power-sequence-round/run.log)，故障分别在同级`-slt-fault-off/transport/transport-after/power-off`目录；V2行为保持原合同。
- 顺序与快照补测：同一0106内三类六种排列、连续两次SLT、同名FUNC重复及非递增/重复index均通过；0107/010B先后及断线后的晚到0x35仍使用本轮2133和cookie，OFF_READY后才清除。
- 运输实际C：McuOverlay共6项通过，包含[共存自动SLT](../build/mcu_pxe/handoff/tests/result-shared.log)与[历史独占模式](../build/mcu_pxe/handoff/tests/result.log)。自动预约不抢普通时序，RELEASE入队与TC分开验证。
- [RK实际C](../../b1/build/rk-protocol-test/run.log)通过，含原FUNC/PARA事务、已发SENT保留、未发设置TX_FAILED与队列背压，末项时序19/20ms和Tick回绕门禁。
- [B1 check](../../b1/build/slt_auto_check_20261112.log)、[完整Debug构建](../../b1/build/slt_auto_build_20261112.log)通过；V2代码边界20项和纯接口头编译通过。板级电源时序没有修改。

11月12日旧合同BIN为414664字节、CRC32 `B5D0576D`，不是11月13日调整后的校验值。11月13日稳定产物：[完整Boot＋App ELF](../../b1/build/slt_release_20261113/b1-slt-release-20261113-ip202.elf)、[BIN](../../b1/build/slt_release_20261113/b1-slt-release-20261113-ip202.bin)、[校验清单](../../b1/build/slt_release_20261113/artifacts.json)。ELF MD5为`3c144e4208b81cf9eb0269ad801adc71`，BIN MD5为`15ad8764140600c48f3b006743ac9439`，当前本地地址为`.202:8888`。当前EDK H1C镜像可继续使用，无须本轮重编EDK/Loader。本轮未访问实板或烧录。
