# RK协议替换与双链路验证记录（2026-09-23）

## 结论

按用户指定的RK工程师`D:/pro/tlv_proto`替换MCU旧RK协议，已同步独立B1。V2真实开发板通过TCP＋COM5全链路8组测试；PC和RK均由脚本模拟，未验证真实RK/DDR算法、供电和采样。

板卡收尾：A1运行正式0.1.15，A2回滚槽为正式0.1.14，两个槽均无联调旁路；完整内部Flash回读逐字节核对通过，B及末尾保留区与本轮备份一致。正式TCP业务21项通过，W25查询当前0.1.15且confirmed=true。串口、ST-Link均已释放；本轮没有提交或推送。

## 1. 改了什么

| 层 | 修改 |
|---|---|
| RK编解码 | HCRC覆盖前3字节；FCRC改为Seq＋TLV；Type/Length/Seq大端，Value整数和CRC小端；保留原CRC实现，不引入第二套串口驱动 |
| TLV | 追加式多TLV组帧；完整Value存储；长度/容量/数量/已知字段宽度检查；任何残缺帧清空输出，不交付有效前缀 |
| 字节流状态机 | 50ms字节间超时、连续帧自动复位、伪帧头恢复、缺帧尾不吞下一帧头 |
| 最终结果 | 0x5001/0x9001/0x3001/0x9002同帧聚合，每种恰好一次；按Seq＋命令号匹配，保留64位地址/毫秒值 |
| RK事务 | 请求/结果对象与队列；单在途测试；结果队列满保留；不再500ms自动重发；超时只报一次且暂停后续发送，迟到完成仅解除等待 |
| B1 | 同步通用源码，保留UART4/USART2、DMA、OFF/POWER_OFF及OTA五任务健康逻辑，不借用结果灯，不复制V2硬件映射 |
| 旧入口 | 删除未使用的`App/protocol/rk/dispatch.c/.h`及其构建引用；旧RK Python脚本以`.py.txt`移到`tests/legacy_rk/`，可恢复但不再运行 |
| PC侧 | 正式18字节头/JSON及门禁不变；仅在默认关闭的`ENABLE_RK_GATEWAY_TEST`下接入`App/tasks/test/rk_gateway_test.c`。日常构建显式关闭开关，防止缓存误带入 |

新注释说明接口、单位和失败语义；更新历史集中放本文件。旧协议文档归档到`doc/history/rk/`，当前入口固定为[protocol_spec.md](protocol_spec.md)。

参考包的编解码不检查真实输出容量、TLV长度/数量等问题已在MCU端补齐；演示接收循环的连续帧reset、伪头长度和POSIX时间依赖不照搬。原`D:/pro/tlv_proto`未修改；建议RK工程师同步检查其自身接收边界。该目录没有实际SPL/算法/定时器源码，文档中的“已实现”不作为本次真实RK执行证据。

## 2. 离线与构建证据

| 检查 | 结果/范围 |
|---|---|
| V2、B1 `verify_rk_protocol_c.py` | 两板实际C在QEMU通过；黄金帧、最大长度、坏字段、连续帧、Seq位翻转、超时、单在途、队列满、TX失败、迟到结果和B1断电 |
| 两板 `verify_uart_idd_c.py` | UART恢复、RK边界、IDD统计回归通过；不代表外设真实测量 |
| V2 `style-test` | 19项代码边界/事件模型及纯头GCC检查通过 |
| V2 `pc-protocol-test` | 24项通过 |
| B1 `tools/dev.ps1 check` | 引脚/网络/协议/电源门禁等检查通过 |
| 普通Debug | V2及B1构建通过 |
| V2隔离驱动Mock | ARM构建通过；本轮不另烧此镜像，不能记成板上Mock运行 |
| V2正式OTA Release 0.1.15 | A1/A2精确地址修正及反向一致，镜像144096字节，1359项修正，`.ota`149648字节 |
| B1正式OTA Release 0.1.15 | UART4/USART2各自A1/A2及反向一致，镜像139452字节、1346项修正；无B1实板 |

可复跑入口：

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 protocol-test
python -B -X utf8 tests/verify_rk_protocol_c.py --app-root D:/pro/b1
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 pc-protocol-test
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 style-test
powershell -ExecutionPolicy Bypass -File D:/pro/b1/tools/dev.ps1 check
```

## 3. 实板全链路

- V2：`192.168.1.201:8888`，COM5 CH340接USART1，115200/8N1。
- ST-Link序列号：`54FF6D067182505239431087`。
- 用户已授权备份后临时联调镜像，完成后恢复正式镜像。
- 初始正式版0.1.11；0.1.12首次联调发现精简C库不支持`%llX/%llu`，结果原始数据正确但描述乱码。仅修正临时适配器为高低32位拼接十六进制，0.1.13重跑全部通过，失败记录保留。
- 临时适配器只模拟0103的电源/RK-ready条件，不写DAC/TPS、不启动RK电源；返回`testOnly:true`，siteId固定`RK_SIM`，die临时映射mirrorIndex，不作为生产合同。

`tests/rk_gateway_network_test.py`通过8组：

1. PC半包/粘包解析，下发RK小端命令，超过500ms不重发。
2. 错Seq、结果缺字段/重复字段、坏CRC均不冒充结果，PC心跳正常。
3. 伪帧头恢复、串口逐字节传输，四TLV合成PC 0105。
4. 重复最终结果不重复回包；PC发送Seq不被RK Seq替代。
5. 17=MATS、结果TLV乱序、串口心跳粘包、`0x0000000100000008`失败地址正确。
6. 10秒测试用超时返回执行失败，不自动重发；迟到结果不假报成功。
7. 匹配的迟到结果解除RK等待，下一条新请求正常执行。
8. 全过程电源状态为OFF，没有真实上电/调压。

原始收发证据：`build/rk-gateway-hw-20260923/full-chain-02.json`；首次失败为`full-chain-01.json`。脚本关闭TCP和串口，不保留后台进程。

## 4. 备份、恢复与安全收尾

本轮备份目录：`D:/pro/test/build/rk-gateway-hw-20260923/`。

- 内部Flash完整2MiB：`original-internal.bin`，SHA256 `b5b38ae65a4c485d51daf1052f2098290a4791e9022c00cab5556f8066d7e031`。
- W25从0到0x410000：`original-w25-0-410000.bin`，两遍读取逐字节一致，SHA256 `d64cf981dbb0cc34e66875bd932acee4e8315b10d4a20f04317bd5b6903c27da`；包含版本/FRU/日志/下载区/稳定包相关区，未称整颗32MiB备份。
- `backup.json`记录完整哈希和RAM助手哈希；这些板上数据留在本机，不当作源码提交。

SWD入口的备份通过，但连接/复位期间日志变为NORMAL、pending=1，请求生成器因此安全拒绝；尚未写升级包或提交READY。释放调试连接后0.1.11重新健康确认。本轮改用现有0901 TCP OTA入口完成安装，不删除安全检查、不手改启动标志。**一键SWD安装流程仍需修复复位/健康确认衔接，不能宣称已验收。**

升级日志依次为`install-test-network.json`（0.1.12）、`install-test2-network.json`（0.1.13）、`restore-formal-14.json`、`restore-formal-15.json`。最后连续安装两份正式构建，避免回滚槽遗留临时镜像；均经B安装及至少30秒健康确认，不直接覆盖内部App或B。

最终以HOTPLUG只读回整片内部Flash，`final-internal.bin`验证：

- `[0x08000000,0x08020000)`与原始备份一致，B未变。
- A1的镜像内容与`build/ota-app-0.1.15-release-a1/test.bin`一致。
- A2的镜像内容与`build/ota-app-0.1.14-release-a1/a2_from_a1.bin`一致。
- `[0x081E0000,0x08200000)`保留区与原始备份一致。
- 完整回读SHA256：`28c4f0d528b2983f9c3e9f7c3fab48e713ed80d0bfe7cfdf8d0ed4411d6a8bd7`。
- 正式ELF不含`rk_gateway_test`符号；正式业务21项回归通过，包括0103合法请求仍返回notReady、未配置时序禁止上电。测试后结果灯全灭。
- `final-query.json`：版本0.1.15、confirmed=true、activePackageId=`9818FAD7`。COM5可重新打开并关闭，无本轮ST-Link/GDB进程残留。

## 5. 仍需双方确认/真板验收

- RK启动ready、安全下电/取消、掉电前TX排空与超时后主动恢复接口。
- 实际算法运行时长、一个算法是否只产生一次最终结果，以及die如何映射PC颗粒/板卡标识。
- IDD21..36及测量窗口；本次只支持已定义的功能测试线格式。
- 正式0103的训练安全电压、冷重启编排、算法映射和PC最终结果生命周期。临时适配器不得用于生产。
- B1最终UART选择、真实电源/器件/DDR/LED验收。

## 6. 参考文件标识

原参考目录未修改。关键文件SHA256：

```text
proto.c: AC27C4506514F85362D035E77CE9E690279505BBB4405293020CAFB07F413FB2
proto.h: EA48FB89864B0C059ABDAE27F4CB3F4C191C2F11390583D8425090712D824075
proto_util.c: BD935BF2D4B0BC9A497006376EC42597CE2DD205953BB3E5CFD6BA2627348234
crc_modbus.c: 2767CB59F4DF344E109B1BD65D17A6012CC6C7EEABBADC1D329E222E254E784C
uart-rk3588-protocol.md: B51CF24AA38AC3092BE044590ACEC64CFD1167EA7693341B727E999EE473AF7B
```

## 7. 新版 RK Word

- 新建 `doc/mcu_rk协议.docx`，参考旧版 `E:/STM32与RK3588_UART通信协议规范--20260708.docx` 的章节组织；旧 Word 及 RK 参考目录均未修改。
- 按新协议重写帧偏移、BE/LE、双 CRC 覆盖范围、命令 1..20、四 TLV 最终结果、心跳与单在途超时行为；明确未实现的 IDD 和未冻结的 RK-ready、die 映射等业务接口。
- 不复制参考文档残留的零 TLV、通用 Value 上限 1018 字节、500ms 自动重传、3s 心跳断链/复位或错误 CRC 查表项；通用 Value 上限为 1020 字节，扣除 CmdID 后参数理论上限才是 1018 字节。
- 新增请求、PASS、FAIL、心跳四组完整十六进制示例。生成时及从最终 DOCX 提取后均独立验证 Length、整帧长度和两段 CRC；只代表离线算例，不代表真实 RK 算法验证。
- 文档工具缺少 LibreOffice，改用本机 WPS 后台只读导出 PDF，再用 Poppler 渲染；最终 8 页已逐页检查，无跨页残段、裁切或表格溢出。检查中间件在 `tmp/rk_word_20260923/`，不作为正式交付。
- 本轮仅文档制作与检查，未修改固件、未连接板卡、未提交或推送。

## 8. 查表整理与中文注释（同日后续）

- 将RK接收状态的`switch`改为5态函数表，索引前拒绝非法枚举；保留半包、连续帧、坏头恢复和50ms字节间超时规则。
- 用同一TLV规则表检查6种已知Type的Value长度，并聚合四个最终结果字段；未知扩展仍按完整帧容量检查，不补齐必需结果、不接受重复结果字段。
- 在`rk_link.c`集中维护1..20的编号/参考名称目录，请求校验、任务发送及V2临时适配器共用；删除适配器中重复的名称数组。不是将算法移到MCU执行，也未放开IDD21..36。
- 补充RK帧宏、状态、请求/结果字段、接口返回值和测试开关的中文注释，说明Type与CmdID、字节数、大小端、有效范围以及PC/RK Seq的区别。更新历史只记在本节，不塞入源代码注释。
- 7个公共文件同步独立B1；心跳文件仅同步帧长宏和注释，保留B1供电门禁、UART选择及健康上报，不移植临时测试适配器。
- 验证通过：两板实际C/QEMU回归（新增20条命令映射、6类字段宽度、未知扩展、原子结果及非法状态下标）；两板普通Debug；V2 PC协议24项、style19项及纯接口头检查；B1板级检查。V2临时OTA联调配置也重新构建通过，未执行安装或硬件测试。
- 临时配置复用了`build/ota-app-rk-test-0.1.13-release-a1/`构建目录，其中产物已按本次源码更新，不能将当前产物当作第3节当时已烧录镜像；原始实测JSON、板上备份和恢复记录未修改。
- 本次查表修改未连接ST-Link/串口/网络、未烧录、未提交或推送；第4节板上状态是此前实测记录，不代表本次代码实测。正式0103仍保留notReady/otaNotReady，内部入口说明见[协议第5节](protocol_spec.md#5-查表入口与业务接入现状)。

## 9. 心跳合并进RK任务（2026-09-24）

- V2/B1删除独立`hb_task.c/.h`、任务声明/创建及CMake构建项；由`rk_task.c`中的静态`process_heartbeat`每1000ms尝试发送。两板各减少4KiB任务栈分配和一个任务控制块，不减少原有RK栈，不声称已实测新镜像堆余量。
- 心跳仍用独立于PC的RK序号，不进入测试pending；发送失败等下一周期，错过周期不补发。B1非ON不发送，OFF刷新计时，发送前复核电源；不改变已排队UART字节的下电处理边界。
- ON/OFF每轮均最多取8个接收块，断流通知也计入配额。未取走的数据留在路由队列，半帧保留；防持续RX/GAP饿死事务超时、心跳、OTA健康及调度让步。
- OTA必要健康任务改为默认/TCP/监测/RK四项，按`OTA_TASK_COUNT`生成掩码，拒绝越界进度编号。保留30秒确认、失活停喂狗及故障锁存；通用健康引擎和独立Bootloader未修改，五槽容量不等于本App仍有五个必要任务。
- 日常下载工具只兼容新四任务`0x0F`与旧五任务`0x1F`完整位图，不能接受缺RK的`0x07/0x17`。此处是RAM诊断核对，不改变PC/RK帧或W25存储布局；此前SWD复位/pending衔接待办仍未解决。
- 验证：两板实际RK C/QEMU通过，覆盖1000ms边界、失败、回绕、Seq/pending隔离，以及真实任务循环下持续RX/GAP和B1 OFF→ON；两板实际OTA C/QEMU各6例通过，含缺RK拒确认、确认后RK卡死停喂狗；下载脚本离线回归通过。V2 PC24项、style19项和B1完整检查通过。
- 两板普通Debug通过；V2和B1 UART4/USART2的OTA Release双槽均完成A2逐字节对照及反向A1一致。离线候选版本使用0.1.16，仅构建，未安装：V2镜像144632B/1384处修正，B1两UART均140012B/1371处修正。报告在各工程`build/ota-app-0.1.16-release-a1*/relocation_report.json`，不覆盖先前实板0.1.14/0.1.15证据。
- 删除前的心跳文件及本轮相关原文件保存在`tmp/hb_merge_20260924/before/`，可恢复。本轮没有连接烧录器、串口或板卡网络，未烧录、提交或推送；正式0103门禁不变，协议分层不合并。
