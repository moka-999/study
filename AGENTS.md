# 项目说明书（AI 接手必读）

> 本文件是给 AI 编码助手的「接手说明书」，换任何 AI 打开仓库先读这里。
> 每次里程碑更新「六、当前进度」一节。详细调试记录见 `docs/`。

---

## 一、项目是什么

STM32H743 **阿波罗 V2** 板，作为 **TCP server + TLV 协议中继**，把 PC 命令经串口透传给 RK3588（内存测试/电源监控从板）。

**拓扑**：
```
PC ──TCP:8888──► MCU(STM32H743) ──USART1──► RK3588
```

**软件栈**：FreeRTOS 10.6.2 + LWIP 2.2.1 + ETH (YT8512C, RMII) + TLV 协议。

---

## 二、TLV 协议（0x5A/0xA5 帧）

```
[0x5A] [Len:2B BE] [HCRC:2B] [Seq:8B] [TLV...] [FCRC:2B] [0xA5]
```
- CRC = Modbus CRC-16（多项式 0x8005，初值 0xFFFF）
- TLV 单元：`[Type:2B][TLV_Len:2B][Value]`
- 命令容器 `Type=0x5000`（Value = `[CmdID:2B][Params]`）；心跳 `Type=0x0000`
- 帧内 TLV 数量：**至少 1 个，最多 16 个**（`TLV_MAX_PER_FRAME`）
- 状态机 6 态：IDLE→HEADER→DATA→CRC→FOOTER，TCP 和 USART1 **共用**（`App/protocol/state_machine.c`）

> ⚠️ **type 语义未定**：现状是枚举常量（0x5000/0x0000），plan 文档里的「高4位类型+低12位Tag」结构化是另一套互斥设计，已搁置，等驱动写完拿真实数据再定。详见「七、待定事项」。

---

## 三、目录结构

| 目录 | 内容 |
|------|------|
| `App/protocol/` | TLV 协议栈：frame.c / state_machine.c / dispatch.c / crc.c |
| `App/tasks/` | FreeRTOS 任务：tcp_task / rk_task / hb_task / monitor_task 等 |
| `BSP/` | 板级驱动：AD7124 / Flash / IIC / LED / PCF8574 / WDT |
| `HAL_Drv/` | 硬件抽象：uart_router.c（串口路由）等 |
| `Core/` `Drivers/` `Middlewares/` | CubeMX 生成（HAL/CMSIS/FreeRTOS/LWIP），**勿手改** |
| `tests/` | Python 测试：`full_chain_stress.py`（12场景）、`stability_test.py`（6项） |
| `cmake/stm32cubemx/` | CubeMX 生成，**勿手改**（自定义源放顶层 CMakeLists.txt） |

---

## 四、工具链（最重要，换 AI 必读）

### 编译
```bash
cd build_debug && ninja
```

### 烧录（SWD）
```bash
STM32_Programmer_CLI.exe -c port=SWD -w test.elf
STM32_Programmer_CLI.exe -c port=SWD -rst   # 复位运行
```

### 工具链路径
```
C:/Users/Administrator/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/
```

### 串口 / 网络
- 串口：COM3 = CH340 → USART1（阿波罗阶段接 PC 模拟 RK3588）
- MCU IP：`192.168.1.201:8888`

### 崩溃诊断（HardFault 排查）
- 诊断地址：`crash_log` = 0x20009600（16×uint32），`tcp_diag` = 0x20009640（用 `nm test.elf | grep crash_log` 验证为准）
- 反汇编：`arm-none-eabi-objdump -d --start-address=<pc-0x100> --stop-address=<pc+0x100> test.elf`
- **判断异常栈**：用 `EXC_RETURN(LR) bit2`（1=PSP线程 / 0=MSP handler），**不要**用 `__get_CONTROL()&2`（进异常时硬件已清 SPSEL，判断必错）

### 运行测试
```bash
python tests/stability_test.py
python tests/full_chain_stress.py
```

### 工具依赖分层（换 AI 必读）

| 层 | 工具 | 换 AI 后 |
|----|------|---------|
| CLI（机器上，永不丢） | ninja / STM32_Programmer_CLI / arm-none-eabi-objdump / python / git / curl | ✅ 任何 AI 用 Bash 直接调 |
| 仓库脚本（git 内，永不丢） | tests/*.py、tools/mcp_serve.bat、.mcp.json | ✅ clone 即有 |
| MCP 服务器（绑定 Claude Code，会丢） | embedded-docs（查寄存器手册）、PlatformIO、translator | ⚠️ 降级到 CLI |

**MCP 降级方案**（换 AI 后若 MCP 不可用）：
- `embedded-docs`（查 STM32 寄存器/手册）→ 直接读 PDF 手册，或 `mineru-open-api flash-extract xxx.pdf` 转 md 读
- `PlatformIO` → **本项目未用**，编译烧录走 ninja + STM32_Programmer_CLI，无需它
- `mineru`（PDF 转 md skill）→ 底层是 npm 包，CLI 直接 `mineru-open-api flash-extract <file>` 等价

> 核心：开发主链路（编译→烧录→测试）纯 CLI 兜底，MCP 只是查手册辅助，断了可降级。

---

## 五、关键架构约束（改代码前必读）

1. **MCU 是单客户端 TCP server**：`listen(listenfd, 1)` + 单 accept 循环，一次只服务 1 个连接。测试脚本开新连接前必须 `close_tcp()`。
2. **背压链闭环**：`pcToRk_Queue`(8槽) + rk pending(4槽) = 最多 12 条在途。队列满→回 BUSY(0x04)，pending 满→命令滞留队列。**uart_send 可能失败（TX环满），每处调用都要想「失败怎么办」**。
3. **RK 响应匹配不到 pending 一律静默丢弃，绝不回 ACK**（否则 RK 把 ACK 当命令再回响应 → 死循环，曾观测单 seq 重传 1207 次）。
4. **重传**：500ms 超时 + 指数退避 500/1000/2000ms，封顶 3 次（`RK_MAX_RETRIES`）。
5. **dispatch 在 tcp_task 上下文同步执行**，handler 里**禁止 vTaskDelay 阻塞**（reboot 用软件定时器，非阻塞）。
6. **D-Cache 已关** → 无 cache 一致性顾虑，**勿再加 `SCB_InvalidateDCache_by_Addr`**（否则 UsageFault 升级 HardFault）。
7. **DMA 够不到 DTCMRAM**（DMA 在 D2 域），DMA 缓冲放 AXI SRAM (RAM, 0x24000000)。
8. **FreeRTOS 堆(128KB)手动挪到 AXI SRAM**（`heap_4.c` 的 `.heap` section + linker），不是 CubeMX 默认位置。

---

## 六、当前进度

**阶段：阿波罗 V2 固件调试完成，全链路稳定。**

- ✅ 全链路压测 12 场景 **33/33 PASS**，数据零损坏，准确率 100%，吞吐 27~35 帧/s
- ✅ 稳定性测试 11/11 PASS
- ✅ 已修复 3 个固件 bug（重传热循环 / 转发丢命令 / ACK回环）+ 多个测试脚本 bug
- ✅ git 已初始化，推送到 Gitea `LILIHONG/stm32h743`（私有）

**下一步（待办）**：
1. 写 BSP 驱动：AD7124(SPI4) / PAC1934T(I2C4) / TPS62868(I2C1/2/4) / pwr_ctrl(PC6/PC7/PG11 上电时序)
2. 拿到驱动真实数据后 → **修订 TLV type 语义**（枚举 vs 结构化，见下）
3. B1 板到货后：USART1 从 CH340 切到 RS0102→RK3588 真机联调
4. 高波特率（460800/921600）DMA+IDLE（等真机 RS0102，CH340 有 USB 间隙误触发拆帧风险）

---

## 七、待定事项（决策搁置中）

| 事项 | 状态 | 何时定 |
|------|------|--------|
| TLV type 语义（枚举 0x5000 vs 结构化高4位+低12位） | 搁置 | 驱动写完拿真实数据后 |
| 0x5000→0x5002 的语义（命令请求 vs 结果响应？） | 待确认 | 同上 |
| 打包侧一帧一 TLV vs 解包侧一帧多 TLV 不对称 | 待改 | 同上（多TLV打包三段式 builder） |
| `tlv_dispatch` 多单元响应互相覆盖 | 待改 | 同上 |

---

## 八、已知坑（改代码前必读）

### CubeMX 重新生成会覆盖的手动改动
1. 自定义源/头 → 放**顶层 CMakeLists.txt**（勿放 cmake/stm32cubemx/）
2. `heap_4.c` 的 `.heap` section → 重加；长期用 `configAPPLICATION_ALLOCATED_HEAP=1`
3. `stm32h7xx_it.c` 的 `HardFault_Handler_C` → USER CODE 区恢复
4. USART TX drain → 已用 HAL `TxISR` 钩子根治，it.c 无自定义代码

### 测试脚本坑
- 清串口缓冲必须先 settle（等 pending+重传排空）再 `reset_input_buffer()`，否则抓到旧帧
- 全链路测试 `_drain_serial_commands()` 动态排空，别用固定 sleep

### 内存布局
- 任务栈：rk 32KB / tcp 16KB / hb 4KB；堆 128KB 实际占用约 85-100KB，余 ~30KB
- 确认堆余量用 `xPortGetFreeHeapSize()`

---

## 九、提交规范（git）

- **远程仓库**：Gitea `http://192.168.1.122:3000/LILIHONG/stm32h743`（私有），本地 remote=origin，分支 main
- **每次 commit message 必须含两部分**：「做了什么」+「项目整体进度」
- 推送：`git add -A && git commit -m "..." && git push`
- 认证：用户名 `LILIHONG`，密码填 Access Token（已配置 `credential.helper store` 可免重复输入）

> 注意：组织仓库 `mcu/stm32h743` 中 LILIHONG 只有读权限，push 会拒。用 `LILIHONG/stm32h743`。
