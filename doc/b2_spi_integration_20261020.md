# B1–B2 SPI接入记录

建立：2026-10-20；更新：2026-10-21。协议基线为 [SPI v0.6](b1_b2_spi_protocol.md)。本记录是内部实现及验收说明，不向对外Word堆叠驱动细节。

## 1. 本轮范围和任务边界

已接入V2/B1源码，但未烧录、未推送、未连接网络/串口/ST-Link。原PC/RK线上字段、B1真实电源、V2电源模拟和第一颗MATS假FAIL的构建差异保持不变。

| 模块 | 职责 | 不负责 |
|---|---|---|
| `App/tasks/b2/b2_task.c` | 独立4KiB任务：取SPI事务、解析、重试去重、提交业务、发布应答、3秒离线诊断 | RK串口/算法/Seq；温控执行 |
| `App/protocol/b2/` | 纯C帧/TLV校验、CS状态机、应答编码 | GPIO、RTOS、网络 |
| `App/tasks/b2/b2_service.c` | 单槽请求/应答队列、有效编号/镜像模式缓存、调用网络和整轮接口 | SPI寄存器、RK队列 |
| `HAL_Drv/b2_spi.c` | 实际SPI从机、DMA、NSS上升沿事务边界、固定长度队列 | PC/RK业务 |
| `HAL_Drv/network_config.c` | 在lwIP线程执行运行时IPv4更新 | FRU持久化、TCP应用协议 |
| `App/tasks/rk/` | 原有独立RK任务 | 任何B2收发或心跳 |
| `App/protocol/common/crc.[ch]` | 共用无状态Modbus CRC16函数 | 任何一方帧、Seq或状态机 |

B2与RK是并列任务，不在RK任务中增加SPI轮询。B2也不增加一个独立心跳任务：B2主机定期发0004，B1从机被动应答。

业务状态只有一个写入者：B2任务经队列提交，TCP任务通过 `b2_service_poll()` 顺序处理，避免SPI任务与PC命令同时改整轮/灯态或关闭socket；网络底层再通过 `tcpip_try_callback()`进入lwIP线程。此安排不把B2报文交给PC帧解析器，也不复用RK协议业务。

## 2. 事务与异常处理

- 一个CS低电平事务只接受一个完整帧；不跨CS拼接、不在坏事务中扫描5A重新同步。短帧、尾部多字节、CRC或TLV结构损坏不响应。
- 命令可识别但参数不合法时回状态1；旧0001/0002/0003回3。不认识但结构合法的TLV跳过；已知字段重复、长度错误及重复镜像整包拒绝。
- 应答回显本次B2 Seq。仅保存上一份请求/响应：相同Seq且字节相同直接复用响应，不重复执行业务；相同Seq但内容不同拒绝，保留原缓存。
- ISR只收CS快照入4槽队列。SPI DMA收发缓冲放AXI SRAM的`.b2_dma`，NOLOAD区显式初始化；DMA1 Stream3/4不占UART的Stream0～2。
- 应答候选只在CS上升沿换入TX；首次读取可能为FF，主机须继续按协议轮询。一次读应答必须保持CS低：先13字节头，再读剩余字节。
- RX越界、DMA/SPI错误或队列满有只读计数。停止DMA有固定上限，异常时不重用仍运行的DMA缓冲。
- 100ms是主机单次观察窗口，不承诺所有业务拥塞时均在100ms内完成。已提交网络变更不因主机超时伪称未执行；重试依靠相同Seq缓存确认。下一轮周期交换使用新Seq。
- 3秒未收到结构完整且命令可识别的帧标记离线，不自动清IP、不推断常温、不改PASS，也不操作加热。

当前缓存不是跨复位去重日志；B2重启后若重复使用上一Seq却携带新内容，会先收到参数错误，后续新Seq才可执行。不增加Flash写入或无限历史表。

## 3. IP、siteId与灯态

| 项目 | 本轮实现 |
|---|---|
| testerId | B2的3003通知1～21，0仅用于尚未配置的响应 |
| siteId | B1读取PB5:PB4:PB3原始槽位码加1；V2构建项`B2_TEST_SITE_ID`默认1，范围1～8 |
| 地址 | `192.168.210.(10*testerId+siteId)`，掩码255.255.255.0，网关0.0.0.0，端口8888 |
| 切换时机 | 每次合法0004检查当前槽位；tester或slot不同才重配；slot不是B2请求中的可写字段 |
| busy | 改IP且已有被接受的PC连接、测试/电源切换或OTA下载/待提交在途，整包回2，不改灯或缓存 |
| 同IP | 无需网络重配，可单独更新温控显示模式 |
| 复位 | 配置只在RAM；首次通知前维持本地测试IP，FRU布局和记录不改 |
| PC结果siteId | 正式配置生效后使用对应槽位字符串；未配置时保留FRU本地测试标签 |
| 开始整轮0106 | 按已知镜像模式显示：常温双通道闪、高温FAIL通道闪、未知灭 |
| 局部模式更新 | 只更新本包列出的镜像；轮外缓存、轮内更新显示，不控制加热 |
| 收尾0107 | 按errorchip形成最终PASS/FAIL；后续模式通知只缓存，不覆盖结果 |

“PC无连接”以TCP任务已接受的连接为界；IP切换前仍须由现场流程停止连接尝试，不能将该门禁当成多主机网络仲裁。PC首次发现B2、正式上线前多板重名IP的隔离、B2真实八槽片选和贴装仍需系统联调，不能把默认`.201`作为多板同时接入的产线地址。

### 3.1 siteId到IP的实际调用链

`b2_board_site_id()`是读取接口，不是设置接口。B1返回`board_slot_code()+1`，V2返回构建配置`B2_TEST_SITE_ID`（默认1）；因此单独调用它不会写GPIO、更新网络或保存FRU。

| 顺序 | 代码位置 | 做什么 |
|---|---|---|
| 1 | `App/tasks/b2/b2_task.c` | 从SPI取得并解析0004请求，调用`b2_service_exchange()` |
| 2 | `App/tasks/b2/b2_service.c` | 请求入队；B2任务等待业务应答，不在SPI任务直接改网络 |
| 3 | `App/tasks/pc/tcp_task.c` | 无PC连接调用`b2_service_poll(0)`，有连接调用`b2_service_poll(1)` |
| 4 | `b2_service.c: apply_request()` | 调用`b2_board_site_id()`；把新testerId或已知testerId与当前槽位组合，比较是否需要改IP并检查busy |
| 5 | `HAL_Drv/network_config.c` | `network_config_set_tester(tester, site)`计算末段`10*tester+site`，向lwIP线程投递回调 |
| 6 | `network_config.c: apply_in_tcpip()` | `netif_set_addr()`真正改变运行时IP；完成后唤醒等待者 |
| 7 | `b2_service.c` | 成功后更新生效tester/site缓存并回应B2；PC测试结果通过`b2_service_site_id()`读取生效站点 |

例：PB5:PB4:PB3=`010`，原始码2→siteId=3；B2通知testerId=4→IP=`192.168.210.43:8888`。只读槽位没有testerId时不能算正式IP，仍保留本地测试地址。

testerId相同但槽位变化，也会在下一次合法状态交换时尝试切换；没有消息就不会主动轮询改IP。V2没有B1槽位硬件，改变site需修改构建配置，不能通过B2请求写siteId。PC已连接或测试/电源/OTA在途时改IP回busy；同地址不重新配置，断电后需B2再次通知。

以上为正式源码调用关系。独立回环镜像的实板进展见第8节，不替代正式镜像或B1八槽验收。

## 4. 两板接口与最小主机

- B1：SPI2，PB12 NSS / PB13 SCK / PB14 MISO / PB15 MOSI；AD7124仍SPI4。
- 网关V2：SPI3，PA15 NSS / PB3 SCK / PB4 MISO / PB5 MOSI；不占AD7124的SPI2或结果灯的PC10～12。
- 两者Mode0、8位MSB、硬件NSS低有效。新B2任务只进入正常业务镜像，不进入旧器件Mock/Flash/AD波形镜像。
- 第二块阿波罗V2使用 `tools/b2_spi_master/`，裸机USART1+SPI3主机，HSI64MHz、SPI1MHz。只编译不自动烧录，起始地址0x08000000，禁止覆盖网关的完整OTA Boot。
- 接线、命令和验收顺序见[裸机主机README](../tools/b2_spi_master/README.md)。

## 5. 首次接入验证（2026-10-20）

| 验证 | 结果 |
|---|---|
| V2/B1实际C：B2协议、CS状态机、任务重试、业务队列、裸机编解码 | QEMU通过；含黄金帧、CRC损坏、1024字节载荷、16/17 TLV、重复字段/镜像、Seq冲突/回绕、busy/失败及部分模式 |
| PC实际C：V2模拟、模拟+MATS、整轮；B1真实门禁、整轮及电源编排 | 通过；新增未知模式灭灯、局部更新、0107结果保持及B2站点号，V2 MATS80组隔离仍通过 |
| RK实际C V2/B1 | 通过；共用CRC移位不改线格式/算法匹配 |
| V2 UART/IDD实际C、两板普通构建 | 通过；保留原UART恢复、RK边界及IDD统计窗口规则 |
| PC合同、架构边界/纯接口头、B1完整check | 通过；边界新增“B2不依赖RK，独立创建任务” |
| V2 OTA Debug（电源模拟+MATS）A1/A2 | 267924字节，2065处地址修正；正反修正与独立链接逐字节一致 |
| B1 OTA Debug真实A1/A2 | 261188字节，1984处地址修正；正反修正逐字节一致 |
| DMA布局 | 两板链接map中`.b2_dma`均位于0x24020000，大小0x840；不是DTCM |
| 第二块裸机主机 | ELF/HEX/BIN构建通过，text24469、data96、bss11208字节 |

OTA回归版本0.1.0仅作离线地址修正测试，不是正式发布包或已安装版本。

复跑入口：

```powershell
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 b2-protocol-test
python -B -X utf8 tests/verify_b2_protocol_c.py --app-root ../b1
powershell -ExecutionPolicy Bypass -File tools/b2_spi_master/build.ps1
```

## 6. 待实板验收

1. 独立单板已验证query/CRC/Seq，用户确认testerId换IP成功；仍需恢复正式V2镜像复测部分镜像模式和PC业务，再测两块V2互联。
2. 逻辑分析仪核验1MHz/Mode0、CS间隔、13字节读头与续读、首次FF轮询、MISO未选中高阻和断帧恢复。
3. 测PC连接/测试/OTA在途改IP的busy，以及断开后新Seq成功；核验真正netif切换、ARP、socket重连。
4. 测长串口日志/网络负载下DMA队列及任务栈/RTOS堆余量，不把离线吞吐推断为实板吞吐。
5. B1八槽贴装、CS对应、真实灯色与多从机共享总线待板；B2真实固件及PC侧配置流程待双方联调。

不修改加热权、PC或RK协议字段；不把本轮离线结果写成真实SPI/网络/灯光验收。

## 7. TLV编码说明补齐（同日）

- 本地协议Markdown和Word第2节补齐Type高4位类型表、低12位B2 tag表及长度规则；0x0～0x9与RK的`lentable[]`宽度一致，保留B2的0xA字节数组扩展。
- 仅增加说明；现有6个Type、0004命令、请求/应答样例和固件均未改，RK测试参数tag不复制为B2字段。
- Word除第2节外正文XML不变；全文241个文字段与Markdown一致。两个37字节示例的长度、HCRC/FCRC与当前共用CRC表核对通过。
- 默认渲染器缺少LibreOffice，改用WPS后台只读导出并检查全部7页；校验和分页图在`tmp/spi_tlv_20261020/`。旧Word归档为`doc/history/b1_b2_spi/b1_b2_spi协议_20261020_153420.docx`，未同步云端或烧录。

## 8. 双SPI实测修正同步（2026-10-21）

### 8.1 证据及同步范围

- 独立工程`D:/pro/b2_spi_loopback`的查询、坏CRC恢复和重复Seq实测证据为`build/spi-fix-board-20261021-113741.log`；本次用户另明确确认“双SPI测试成功，而且可以正常修改IP”。这项确认仅覆盖独立单板SPI和换IP，不自动覆盖正式镜像、灯色、B1八槽或高负载。
- V2与B1原有独立B2任务、协议/CS状态机、业务队列、动态IP和0106/0107灯态接口继续使用。同步的是`HAL_Drv/b2_spi.[ch]`的DMA错误分类及诊断快照，不搬入回环主机、串口CLI、可写测试槽位、板载LED视图或裁剪后的PC命令表。
- 直接模式下FEIF单独出现仅记录；TE/DME、SPI OVR/MODF/TIFRE、停止DMA超时仍拒绝。FIFO模式下FEIF仍为错误；帧长、HCRC/FCRC、TLV及Seq校验不放宽。
- 新诊断字段只存在于本地结构体，不增加SPI、PC或RK线上字段。保存最近CS结束的SPI/DMA寄存器、收包长度、FEIF事务计数与停DMA超时计数，供后续现场诊断。

官方依据：[ST RM0433 Rev 8 §15.3.20、§15.5.10](https://www.st.com/resource/en/reference_manual/rm0433-stm32h742-stm32h743-753-and-stm32h750-value-line-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)说明直接模式也可能置FEIF，DMDIS区分直接/FIFO模式。因此本实现不把FEIF等同于“无错误”，而是保留诊断、依靠完整帧校验继续拒绝损坏请求；不能仅由CRC通过推定所有时序和负载已验收。

### 8.2 本轮验证

| 验证 | 结果与范围 |
|---|---|
| V2/B1实际B2 C回归 | 通过：帧/CRC/CS、重试去重、部分模式、tester/slot更新、PC/测试/电源/OTA busy、失败不提交缓存 |
| 新增实际驱动错误分类 | 通过：直接/FIFO模式、RX/TX的FE/TE/DME组合、SPI错误、无关流标志；只运行纯分类函数，不模拟DMA硬件 |
| 两板PC整轮回归 | 通过：未知模式灭灯、局部显示更新、0107最终结果保持、SLT收尾与占用 |
| 原有两板差异 | V2的80组MATS假FAIL隔离回归通过；B1真实电源编排回归通过，不移入V2模拟 |
| 普通构建/边界 | 两板构建通过；V2边界20项及纯接口头通过，B1 check通过。`.b2_dma`仍在AXI SRAM `0x24020000`，预留`0x840`字节 |
| OTA Debug双槽 | V2电源模拟+MATS：268108字节/2065处修正；B1真实：261380字节/1984处修正。两板A1→A2与独立链接逐字节一致，反向还原也一致；版本0.1.0仅为离线验证，不是发布/安装版本 |

复跑仍使用第5节的`b2-protocol-test`入口，新增分类结果保存在每板`build/b2-protocol-test/spi-errors/`。正式驱动只有板型选择分支，不引入测试任务。

上述源码同步阶段未烧录、未推送，也未连接硬件。随后用户授权恢复V2，实际安装记录见第9节；不能把该次离线构建与后续安装混为一次验证。

## 9. V2恢复与0.1.38安装（2026-10-21）

### 9.1 恢复基线

- 用户确认RK空闲、PC/网络助手断开后执行，只操作V2；没有烧录B1、下发RK算法或推送仓库。
- 正式完整备份为`D:/pro/b2_spi_loopback/build/flash-20261021-110516-712531/internal-before.bin`，共2097152字节，SHA256为`2dd689b6656ff90c6374853d2f7ce4d95f82f6bb62019319aed8296e7f5ae68c`。11:36的备份已经是独立双SPI固件，不是正式恢复源。
- 本次先备份当前双SPI镜像，再通过RAM助手读取W25的`0..0x410000`两次，逐字节一致。最新有效日志为代数399、A1、0.1.37、已确认，活动镜像CRC与待恢复备份精确匹配后才恢复内部2MiB。
- 恢复阶段不向W25发擦写或修改选项字节。内部全量回读与原正式备份一致，再复位启动；网络实查0.1.37/confirmed、逻辑电源OFF。
- 恢复证据和可恢复的测试镜像备份：`build/v2-restore-20261021-161844-527904/`；本次一次性编排留在`build/restore_v2_from_loopback_20261021.py`，不是生产下载入口。

### 9.2 最新源码安装

使用现有`tools/dev.ps1 ota-swd -ConfirmIdle YES -PowerSimulation -MatsFailTest`，自动由0.1.37递增为0.1.38；不绕过B或直接覆盖App。新包268108字节App、2065处地址修正、276484字节完整OTA，A1/A2独立链接及正反向地址修正逐字节一致。

W25下载区写入和回读一致后，由C接口提交READY；完整B安装到A2（`0x08100000`）。健康诊断为四任务位图`0x0F`、持续31549ms、状态成功；实际App与本次构建一致，内部B、旧A1及保留区未变。安装与备份日志在`build/ota-dev/20261021-162349-324600/`。

构建保留`ENABLE_OTA_BOOT=ON`、`ENABLE_V2_POWER_SIMULATION=ON`、`ENABLE_V2_MATS_FAIL_TEST=ON`；旧`ENABLE_RK_GATEWAY_TEST=OFF`。MATS仍只将第一颗的PC结果视图模拟FAIL，RK算法不模拟，B1没有假FAIL。B2任务为正式独立任务，没有引入回环主机或测试串口CLI。

### 9.3 安装后只读验证与边界

| 检查 | 实板结果 |
|---|---|
| 0901查询 | 0.1.38、confirmed=true、activePackageId=`79C8CF12`，与本次包一致 |
| 0503查询 | powerState=off，powerCycleCount=0 |
| 0002心跳 | 正常返回0001和空JSON |
| 0101查询 | JMCT-4000 / D00 / RK3588_test |
| 0601查询 | 正常返回0602；OFF时validMask=0、statistics=null，不伪报有效电源样本 |
| SPI3初始化 | 根据实际A2 ELF定位，接收队列非空，SPI3 SPE=1、CR2=0、CFG1=`0x0007C807`、CFG2=0 |
| SPI及内存状态 | 尚无CS事务，10项SPI诊断均0；空闲堆31408字节、历史最低30592字节，仅为本次轻负载快照 |
| 连接释放 | 本轮TCP客户端关闭，GDB/Programmer进程退出；最后状态读取使用Hot Plug，不复位或主动暂停CPU |

网络原始记录为上述安装目录的`network-readonly.jsonl`，完整寄存器快照为`runtime-readonly-full.log`；所有调试地址仅适用于本次实际A2 ELF，不可复制到其他版本。

恢复后默认地址为`192.168.1.201:8888`，并非独立回环之前的`.210.41`；收到B2合法testerId后仍按既定规则动态切换。本轮没有向正式镜像注入SPI请求，也没有运行RK算法，因此正式镜像的双机SPI、改IP/busy/LED联动、RK长测试和B1八槽仍须分别验收。四根旧回环线不再形成测试主机，不应把正式SPI2当成回环主机继续发送。
