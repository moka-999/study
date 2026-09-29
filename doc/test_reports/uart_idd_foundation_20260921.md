# UART接收恢复与IDD统计基础（2026-09-21）

## 交付范围

按用户“1、2都做”完成V2/B1的UART接收健壮性及三轨采样统计基础；复用现有UART、RK状态机和监测任务，不新增任务、依赖库、PC命令或JSON字段。本轮未烧录、未网络升级、未操作板卡Flash/W25、未提交或推送。

### UART与RK帧边界

- 只支持普通模式DMA；每次启动关闭HT半传输中断，路由也忽略HT，避免前半块被重复交付。IDLE/TC继续交付接收块，不把块当作协议帧。
- 队列满、非法接收长度、HAL错误或重新启动失败均登记诊断和断流。`uart_poll_rx()`返回`UART_RX_GAP=-1`时已保守清空该槽队列，RK任务立即重置半帧状态；不能将断流前后的字节拼成一帧。
- 错误回调仅记录状态；唯一接收任务周期poll，在任务上下文停止/清错/重启RX，同一槽尝试间隔至少100ms。Abort不在ISR或临界区内执行，保留TX；Abort可能等待HAL自身超时，不能宣称接口完全非阻塞。
- `uart_get_rx_diagnostics()`复制本次开机的溢出、丢块、HAL错误码、启动失败、恢复失败/成功和恢复等待状态。累计计数饱和；丢块数不是物理丢失字节数，也不是对端离线判定。
- 每槽必须有唯一周期消费者，未被poll的槽不会自行恢复。V2现有slot1没有RK消费者，且未配置RX DMA；初始化会安全拒绝并留诊断，不新增任务替它恢复。
- `tlv_pack_cmd()`在16位长度相加前检查输出指针、非零参数的指针及1018字节参数上限；心跳检查输出指针。失败返回0且输出不变；有效帧、CRC覆盖范围、Seq、Type、重传和未匹配响应处理均未改变。

### 三轨采样统计

`HAL_Drv/idd_stats.c/.h`仅处理输入快照；`monitor_task`负责现有器件事务锁及生命周期。

| 项目 | 当前合同 |
|---|---|
| 入口 | `monitor_idd_start(duration_ms, minimum_samples)`、`monitor_idd_get()`、`monitor_idd_cancel()`；仅内部C接口，本轮未接PC/RK |
| 启动 | 仅电源ON、App已健康、非OTA维护且无运行窗口；取锁最多500ms，锁忙返回IF_TIMEOUT；其它拒绝返回IF_ERR |
| 窗口 | 同一uint32毫秒时基，允许回绕；持续1..INT32_MAX毫秒；最低样本数1..duration_ms |
| 纳入规则 | 整次器件读取的开始和结束均在半开窗口内，完成时间递增；旧、重复、逆序或跨边界快照丢弃；不回灌最新缓存 |
| 数据 | VDD1/VDD2/VDDQ各自电压mV、电流mA的有效/失败样本数、均值、最小值、样点最大值；电压和电流按现有valid_mask分组有效性处理 |
| 均值与峰值 | 有效样点的算术均值，整数除法向零截断；不是时间加权均值；最大值不是未被采到的瞬态峰值；保留现有mV/mA分辨率 |
| 到期 | 六项电压/电流均达到最低有效样本数才为COMPLETE，否则TIMEOUT；COMPLETE不代表电流满足芯片规格，也不代表没有失败采样 |
| 取消 | 下电、监测到非ON、实际尝试重新调压、进入OTA维护或主动取消后CANCELLED，保留部分统计；参数/类型拒绝不取消旧窗口 |
| 无数据 | valid_count=0时mean/min/max的0不能解释为实测零；调用方必须检查state及各计数 |

现有监测节奏仍为循环等待2000ms再加器件读取/取锁时间，没有新增高频采样任务。短窗口可能没有足够样本；电压和电流由不同器件依次取得，不是三轨同步高速采样。本层只是软件收数窗口，尚未保证器件内部转换/平均窗口与RK测试条件严格对齐。

**未完成的完整IDD环节**：具体料号的测试条件与限值、RK进入/保持/退出IDD状态、双方就绪与测量时窗、足够的采样带宽和精度、PC结果字段/业务接入及最终PASS/FAIL。没有实现这些之前，不能称为JEDEC IDD验收完成；B2温控职责不变。

## 验证结果

以下均为本次执行，QEMU运行实际ARM C，但队列、HAL和模拟器件为故障注入桩，不是STM32外设实测：

- V2/B1 `verify_uart_idd_c.py`：通过。覆盖HT忽略、IDLE/TC、队列溢出与断流、半帧清除及下帧恢复、启动/Abort失败与重试、错误码保存、计时回绕；RK空指针、1018/1019/65535长度、容量和黄金帧；统计部分有效数据、窗口边界、重复/旧数据、冻结、取消及整数极值。
- V2/B1 `verify_monitor_boot.py`：通过。执行真实监测任务循环及统计模块，覆盖锁忙、OFF/未健康/OTA拒绝、无样本按时超时、重新调压失败/下电取消、读取跨截止点、读取中取消及新窗口不使用旧缓存；原Boot DAC缓存拒绝/写失败/恢复回归保持通过。
- V2/B1实际AD7124及组合采样C故障回归：通过。
- V2/B1普通Debug、V2隔离Mock、V2与B1 UART4/UART2的OTA槽限Release：构建通过。仍使用原GNU工具链；未覆盖历史`ota-app-*`实板镜像目录。构建产物不是已经安装或发布的固件包。
- 既有PC协议24项、RK模型96项、代码边界19项及纯接口头、电源/LPDDR/Boot模型、B1完整check：通过。

本轮没有在开发板注入真实串口过载/硬件错误，也没有AD/PAC真实电流或高频波形验收；这些保留为实板待办。PC协议、引脚、B1默认OFF/安全门禁、D-Cache关闭和W25信息来源均保持。

## 复跑

从V2工程执行；B1共用测试入口，以`--app-root`切换实际编译源码，不在B1生产任务目录复制测试代码：

```powershell
$taskCompiler = 'C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-gcc.exe'
python -B -X utf8 tests/verify_uart_idd_c.py --compiler $taskCompiler
python -B -X utf8 tests/verify_monitor_boot.py --compiler $taskCompiler
python -B -X utf8 tests/verify_uart_idd_c.py --app-root ../b1 --compiler $taskCompiler
python -B -X utf8 tests/verify_monitor_boot.py --app-root ../b1 --compiler $taskCompiler
```

依赖本机`D:/qemu/qemu-system-arm.exe`及同级`bootloader/tests/qemu_start.s`、`qemu_test.ld`；可通过`--qemu`覆盖QEMU路径。各工程的`build/uart-idd-test`、`build/monitor-boot-test`保留编译与运行日志。
