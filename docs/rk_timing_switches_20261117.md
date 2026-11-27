# RK普通时序与启动频率开关

FUNC 0103、PARA 0108、SLT 010A 共用 `Core/Inc/board_rk_config.h` 的两个编译开关。11 月 20 日起普通时序默认关闭，启动频率查询默认启用。PC 仍发送原 JSON，MCU 完整校验 `TimingParameters` 对象及字段格式。

| 普通时序 B1_RK_TIMING_ENABLE | 频率 B1_RK_FREQ_ENABLE | 行为 |
|---|---|---|
| 1 | 1 | 15项普通时序按请求发送；0x35回复本轮Freq |
| 0 | 1 | 不发普通设置；0x35仍回复本轮Freq |
| 1 | 0 | 普通设置照常；0x35统一回复int64的-1 |
| 0 | 0 | 不发普通设置；0x35统一回复int64的-1 |

普通设置是0x25、0x26及0x28～0x34，携带701B/u32。Freq只在RK查询0x35后，通过5001+801C/int64回复；关闭Freq也保留查询响应，不主动发频率包。启用Freq时仍保留PC提供的负值，缺省为-1。

开关在PC格式校验后作用于本轮数值副本，保存的JSON和`PC RX`日志仍显示PC原始请求。每次受理另记录：

```text
[RK CONFIG] cookie=1 timingEnable=0 freqEnable=1
```

频率关闭时，启动复用按有效值-1比较，不因PC的Freq变化额外冷重启；外部已启动模式允许携带被忽略的非负Freq。显式`reboot:true`、电压/Boot变更、FUNC/PARA的原ready门禁及SLT冷启动仍按原合同处理。

普通时序关闭时，MCU 跳过普通设置。重新冷启动后，可通过下面的回读查看 RK 报告的默认时序。负频率表示使用 RK 默认频率。

## 启动时序回读

B1 每次实际给 RK 上电，收到本次 SPL 普通心跳后，在协议串口按索引查询一次全部 15 项普通时序。有写入时，先完成本次全部时序写包，再沿用最后一帧的处理间隔开始回读。仅复用已启动的 RK 时不会重查。手动 `start rk`、PC 电源启动和 Shmoo 每个冷启动点也走此流程。

请求包含 `5000=0x0043` 和 `3021=索引`。RK 在一帧应答中返回 `5001=0x0043`、相同的 `3021`，以及 1 至 4 个通道的读数。通道标签 `0x7022` 至 `0x7025` 分别对应 CH0 至 CH3，值为小端 u32。通道号可以不连续，每个标签只出现一次。MCU 收到一帧有效应答就完成该项查询。索引 0、1 分别为 CAS 写、读延迟，3～15 为 tREFI 到 BL，2 保留。Freq 走启动阶段的 `0x35`。

串口或 TCP 8889 选择 `log mcu` 后，在启动前开始接收。日志按通道号升序显示实际返回的读数，包括 0。例如 RK 读回 tRRD，仅返回 `7022[4]=00000000` 和 `7024[4]=16000000` 时，日志如下。

```text
[RK TIMING] index=14 tRRD CH0=0 CH2=22
```

每项最多等候 500 ms，由 `RK_TIMING_READBACK_TIMEOUT_MS` 配置。超时记 `unavailable=timeout`，本地发送失败记 `unavailable=txFailed`，随后继续下一项。若 15 项全部超时，等待合计约 7.5 秒，另加任务调度间隔。缺失值不会写成 0 或计入 PASS/FAIL。若前级已自行进入 EDK 并发来 HELLO，MCU 结束 SPL 回读，把串口交给 EDK。

### 手动查询时序

在串口或 TCP 8889 的 `log mcu` 模式输入 `check CASReadLatency` 并换行。控制台回显 `[RK TIMING] check CASReadLatency queued` 表示 MCU 已受理。读延迟对应索引 1，查询帧含 `5000[2]=4300 3021[1]=01`。写延迟用 `check CASWriteLatency`，索引为 0。其余普通时序使用对应字段名，例如 `check tRRD`，区分大小写。

输入 `check all` 并换行，可一次查询全部 15 项普通时序。MCU 逐项发送 `0x43`，在日志里显示各项实际收到的通道值。单项超时会标出参数名和 `unavailable=timeout`。最后的 `check all received=N/15` 表示有 N 项收到了有效应答，每项只计一次。查询期间再次输入 `check all` 会共用当前一轮。启动频率仍走 `0x35`，不计入这 15 项。

RK 电源到达 ON 且协议串口仍可发送 SPL TLV 时，就可以手动查询。等待本次启动心跳、自动回读、FUNC 算法或 PARA 电流采样期间均可使用。不同参数可连续提交，无需等待上一项的应答；重复查询同一在途参数时，共用这次查询及其结果。MCU 日志按参数显示 RK 实际返回的通道和读数。查询帧入串口队列后 500 ms 内未收到有效应答，就记录 `unavailable=timeout`，测试继续。

站点关闭时报 `siteDisabled`，RK 电源未到 ON 时报 `powerOff`，测试停止清理期间可见 `stopping`。串口转入 EDK 交接或数据传输、无法发送 SPL TLV 时回 `uartHandoff`。未知字段及 `Freq` 回 `invalidTiming`，启动频率由 RK 通过 `0x35` 查询。

## 验证

四种组合均已通过实际C/QEMU专项，覆盖FUNC/PARA/SLT、15项设置的数量/顺序/原值、可选0x35、关闭Freq回复-1、负值保留、普通时序关闭后的SLT迟到查询、频率变化是否冷重启、外部模式、原文副本及非法字段仍拒绝。复用同级test的公共夹具；测试入口为：

```powershell
python -B -X utf8 ../test/tests/verify_pc_rk_service.py --app-root . --power-sequence --round --timing-switches 0 0
python -B -X utf8 ../test/tests/verify_pc_rk_service.py --app-root . --power-sequence --round --timing-switches 0 1
python -B -X utf8 ../test/tests/verify_pc_rk_service.py --app-root . --power-sequence --round --timing-switches 1 0
python -B -X utf8 ../test/tests/verify_pc_rk_service.py --app-root . --power-sequence --round --timing-switches 1 1
```

11 月 17 日的 1/1 配置还通过原整轮回归、B1 `check` 与完整 Boot＋App 构建。当日的 `.201` 镜像为 418744 字节，CRC32 为 `40580AAC`。修改宏后须重新编译并下载才会改变板上行为。

本轮未烧录、未访问RK服务器或改动电源时序；尚未取得四种组合的实板对照结果，不能据离线通过认定时序就是测试失败原因。V2与手动TLV分支未加入本开关。
