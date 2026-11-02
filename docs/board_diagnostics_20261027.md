# B1 首板串口诊断（2026-10-27）

## 使用

只用于B1；V2的USART1仍用于RK协议，本次不改变V2。

- 本地调试口：USART1，PA9为MCU TX、PA10为MCU RX，115200/8N1，无流控。使用3.3V TTL转串口，TX/RX交叉、GND共地；不是RS232电平。
- 先打开串口助手再上电，文本模式发送命令，附加CR、LF或CRLF。
- 默认MCU日志；log mcu选择MCU，log rk选择RK原始日志，status在MCU模式下输出一份状态。RK模式下status只提示先切回MCU，不混入诊断报告。
- 启动完成后自动报告一次，之后不周期刷屏；电源流程原有[power]阶段日志保留。高频采样每次读取不打印。
- 只编译：在B1工程执行 tools/dev.ps1 build -LogSource MCU。当前产物 D:/pro/b1/build/standalone/Debug/b1.elf，从0x08000000启动，无MCU Boot/OTA。

## 可以看到什么

| 标记 | 内容与判读 |
|---|---|
| [boot] | USART1建立后依次记录其他UART/DMA、电源配置、I2C、SPI、QSPI、W25、看门狗/LED、MAC/PHY复位、内核创建和lwIP阶段 |
| FAILED at W25 device init | W25初始化返回错误并进入Error_Handler；不会继续启动业务。最后阶段也保存在s_boot_stage供SWD观察 |
| [slot] | GPIO原始码、加1的候选site、B2已生效site；assignedSite=0表示未收到B2正式配置，不冒充已经分配 |
| [net] | 当前IPv4/MAC、netif的up/link缓存；link=1也不等于PC已连接 |
| [power] state=... PC6=... PC7=... | 电源状态机及输出锁存；不是4V/3.3V实测，更不是DDR已训练成功 |
| [monitor] | AD/PAC最近发布的就绪状态、AD ID及错误；默认OFF时ready=0正常，不能据此判定芯片损坏 |
| [sample] | 快照年龄和有效位：1电压、2电流、4TPS；无效组只显示unavailable，不打印旧值或假零值 |
| [VDD1/2/Q mV]、[IDD1/2/Q uA] | 最新有效三轨值，顺序VDD1/VDD2/VDDQ；是最近采样，不是查询时即时测量，也不是测试平均/峰值 |
| [TPS status 1/2/Q] | 监测任务已读取的三个原始状态字节；查询不再读取/清除器件状态 |
| [UART4-RK]等 | 每路UART累计错误、最近HAL错误位、丢块、启动RX失败及待恢复状态；0错误不证明接线或对端正常 |
| [B2 SPI] | CS事务、收到请求、硬件错误、队列丢弃、合法/非法帧及在线状态；未接B2时0事务/离线正常 |
| [RK history] | 历史拒绝次数、最近拒绝原因和tick，不能当成当前busy；原因0无、1无在途、2Seq、3算法、4进度、5结果、6TLV |
| freeHeap | 查询时FreeRTOS剩余堆字节，不是任务栈高水位 |

ADlastError保留历史非零ERROR；PAClastError会被后续操作覆盖，0无、1参数、2IO、3ID、4配置、5数据、6量程。应结合ready、有效位和实际测量分析；不同模块在报告生成时分别读取，不宣称全板同一瞬间的原子快照。

## 代码边界

- Core/Src/board_diagnostics.c集中组织诊断。main把USART初始化移到安全GPIO/电源OFF之后，先建立日志口，再初始化其他外设；未增加上电、Flash写入或探测命令。
- 调度前按编译默认来源短时输出；每次HAL发送超时20ms，只在非ISR且中断未屏蔽时调用。在创建内核对象前永久关闭，后续全部经过原日志路由。
- 若时钟/USART1尚未建立，或内核创建/运行期进入Error_Handler，不能保证串口打印失败文字；应结合最后阶段与SWD。HardFault也没有新增不安全的printf。
- log_console.c只解释本地命令和管理输出，查询通过一位请求交给默认任务。monitor_task.c只额外发布少量已有器件状态，getter不取总线锁、不访问器件。
- 报告使用1536字节静态缓冲、每10ms最多96字节非阻塞入队，出口忙最多保留2秒；切换来源取消剩余报告。仍是诊断出口，拥塞/接线问题可丢日志。
- 不增加RTOS任务，不改变PC/RK/B2线协议，也不把B1日志口设置同步到V2。

## 验证

- B1 MCU默认完整ARM构建及0x08000000向量/容量检查通过，BIN为253652字节。
- 实际board_diagnostics.c在QEMU执行：MCU/RK两种编译默认、早期错误提示、ISR/中断屏蔽拒绝、永久关闭早期输出、只报告一次、有效/无效测量、分块限速、拥塞重试/2秒丢弃、切换取消、尚无快照均通过。
- 实际日志模块和三路UART路由：两种默认、status分片/合并/确认优先/来源隔离及原日志回归通过。
- 实际监测任务：OFF不初始化、快速采样/窗口/缓存原回归与只读诊断不增加IO通过。
- B1现有板级、PC、电源、独立启动检查通过。代码已编译，尚未烧录到B1；时钟、串口电气、器件应答、精度和现场时序仍需实板验证。

复跑脚本在同级V2仓库：tests/verify_b1_diagnostics_c.py、tests/verify_uart_idd_c.py、tests/verify_monitor_boot.py，传入 --app-root D:/pro/b1 --compiler <arm-none-eabi-gcc.exe>；UART脚本分别加 --log-default 0/1。本次未连接串口、网络或ST-Link，未推送。
