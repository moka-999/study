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

普通时序关闭时，不进入普通设置队列或等待其SENT事件，直接沿原准备流程启动算法或EDK。停止发送不会清除RK上一次写过的寄存器；要比较RK默认时序，应执行冷启动。负频率只表示让RK使用自身默认，实际频率需由RK侧核实。

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
