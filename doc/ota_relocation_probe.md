# 单镜像及A1/A2启动验证

更新：2026-09-15。Boot工作树为 `D:\pro\bootloader`；App实验已迁回 `D:\pro\test`。

## 结论与范围

保留一个B和两个可执行App槽。一个A1基准镜像加地址修正表可生成A2安装内容；
独立链接A2仅作为逐字节比对答案，不要求PC传两份App。没有使用Bank Swap或第二个B。

旧V2基线 `e9b69f7` 的Debug镜像已在Apollo V2分别验证A1、转换后的A2启动。
最新 `app-v2` 的Debug/Release已完成转换的离线复验；本次没有再次烧录最新App。
固定槽实验B不等于自动选择槽位，更不等于完成下载、安装、断电恢复和回滚。

## 工程归属与复现

B工程只保留入口、链接脚本、构建工具及OTA资料；App和重定位校验工具归App-V2。

```powershell
# 在D:\pro\test内：校验当前App，而非复现旧基线
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 ota-probe

# 在D:\pro\bootloader内：构建B，命令本身不烧录
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -ProbeSlot A1
powershell -ExecutionPolicy Bypass -File tools/dev.ps1 build -ProbeSlot A2
```

继续使用GNU Arm 14.3.1、CMake和Ninja；App校验脚本只用Python标准库及binutils。
App实验产物在 `../test/build/ota-probe-*`，不会覆盖其正式 `build_debug`：

1. 同源配置构建A1=`0x08020000`及参考A2=`0x08100000`，两者均限896 KiB。
2. 实验App的VTOR引用 `g_pfnVectors`，使其地址进入修正范围。
3. 仅从A1 ELF提取需要修正的位置，对A1裸镜像进行转换。
4. 转换结果与参考A2逐字节一致，反向转换与A1一致后，才导出 `a2_from_a1.bin`。
5. 实板必须使用此转换产物，不能把直接链接的参考A2冒充转换成功。

实验的固定构建时间用于消除比对差异。App实验开关默认关闭，正常App仍是原A1布局、
1920 KiB上限；这不代表正式双槽App布局已经启用。独立B的 `-ProbeSlot A2`仅固定跳A2，
默认B仍沿用原A1入口。现有粗略向量检查不构成正式升级包安全校验。

## 离线结果：区分两个代码基线

| 基线 / 构建 | App长度 | 修正项数 | 偏移表长度 | A1 CRC32 / 转换A2 CRC32 |
|---|---:|---:|---:|---|
| 旧基线e9b69f7 / Debug | 211164 B | 1579 | 6316 B | 88251644 / BADF4A26 |
| 旧基线e9b69f7 / Release | 124468 B | 1059 | 4236 B | 7D1674FA / BD8DC9EF |
| 当前app-v2 / Debug | 220812 B | 1722 | 6888 B | CD301CC1 / 24143616 |
| 当前app-v2 / Release | 130428 B | 1215 | 4860 B | B8CB6E4B / 22FA917B |

四组均通过转换A2与独立链接A2的精确比较及反向A1比较。`.data`在RAM运行，但其装载初值
中的Flash指针也需要修正；RAM指针本身不变。详细偏移见各A1目录的 `relocation_report.json`。

脚本还检查Thumb位保留、RAM/任意地址拒改、表越界/重叠/乱序、未知类型和跨区域相对引用。
它只处理本机可信构建产物，尚不是接收不可信OTA包的生产解析器。

## Apollo V2实板启动证据

烧录器：ST-Link V2，SN `54FF6D067182505239431087`；CubeProgrammer 2.23.0；
检测电压3.26V，器件ID0x450、Rev V、内部Flash 2 MiB。RDP=0xAA、SWAP_BANK=0，
BOOT_ADD0=0x08000000；本次只读取选项字节，没有修改。

先读取内部Flash全2 MiB两次，长度与SHA-256一致，才进行以下实验。
App/Boot每次下载均经烧录器读回校验；A2使用A1转换产物，不是参考A2的bin。

| 项目 | A1实验 | A2实验 |
|---|---|---|
| App来源 | 旧基线Debug A1 | 同一A1经1579处地址修正 |
| B启动入口 | 固定A1 | 固定A2，硬件复位启动 |
| VTOR | 0x08020000 | 0x08100000 |
| 两次采样PC | 0x0803032E / 0x0803040C | 0x081103D6 / 0x0811040E |
| HAL毫秒计数 | 83137 → 134055 | 46485 → 115434 |
| RTOS毫秒计数 | 83131 → 134048 | 46478 → 115427 |
| 两次采样间推进 | 50918 ms | 68949 ms |
| RTOS任务数 | 12 | 12 |
| CFSR / HFSR | 均为0 | 均为0 |
| crash_log | 16个字均为0 | 16个字均为0 |
| TCP内部状态 | 已监听，等待accept | 已监听，等待accept |

两槽SystemCoreClock均为400 MHz，monitor采样时间戳持续更新、业务电源状态保持OFF。
诊断变量地址由实际ELF的nm符号表取得；没有使用历史地址写RAM。
每次读取核心寄存器后执行 `-run`，避免把调试器暂停当成固件卡死。

只尝试了A1的TCP心跳，连接超时；用户随后确认网线未接。本次不判网络故障、不标网络通过，
也没有发测试仪信息查询、电源/电压控制或W25Q256擦写指令。
这里的计数观察不是已经实现的“30秒健康确认”状态机，也不是长期压力/断电升级验收。

## 原程序恢复与可恢复归档

实验结束后写回实验前完整2 MiB备份，烧录器校验后再完整回读；恢复结果与原备份SHA-256相同：

```text
576B83A8BCABC1B85F1BE4BCCCD23760682F098AAB21444CFFF629F6A9506635
```

随后硬件复位，确认原程序VTOR=0x08020000，CFSR/HFSR=0并保持运行。
备份、恢复回读和当时的四组实验ELF/bin/报告位于：
`D:\pro\test\build\ota-hw-20260915-151821`。不属于待提交源文件。

清理前源码（含11个未提交/新增修改）另归档为该目录中的 `bootloader-before-cleanup.zip`，
逐一比对这11个文件与归档内容一致。归档SHA-256为：

```text
5BFC74470AF9A5A3BF4FA778D92B14EE19656CC8FA33CCCE3A419136F4658100
```

清理了Boot中的App、FreeRTOS/LwIP、无关外设、旧协议/原理图、旧测试、索引和构建缓存，
共4847个文件（包含构建产物）。没有删除工作树根目录或.git，也没有改动B1。
源码可从上述归档或Git历史恢复；旧实验镜像已另存，旧构建缓存可重新生成。

独立Boot的Debug、Release、A1/A2实验构建及镜像向量/大小检查通过；
A1/A2实验B与实测B逐字节一致，默认B与实验前板上B的前64字节一致。
自有汇编和构建脚本的注释已统一中文。App迁移复验遇到tcp_task.c末尾孤立的
`tufresume`，经用户明确确认只删除这一行；正常App构建和PC协议24项离线检查通过。
Boot差异检查通过；App差异检查仅提示tcp_task.c原有末尾空行，本次遵照“只删除这一行”
保留其余空白，没有扩大到该文件其他修改。
没有提交或推送。

## 修正规则与后续边界

当前只对 `R_ARM_ABS32`的Flash目标做平移；两端同移的 `R_ARM_THM_CALL`、
`R_ARM_THM_JUMP24`、`R_ARM_PREL31`保留相对距离，`R_ARM_NONE`无动作。
未定义弱引用必须由符号表确认WEAK+UND，不能只凭符号值为0放行。
依据：[ARM ELF ABI](https://github.com/ARM-software/abi-aa/blob/main/aaelf32/aaelf32.rst)、
[GNU ld保留重定位记录](https://sourceware.org/binutils/docs/ld/Options.html)。
后续换编译选项、库或布局必须重新执行逐字节验证，不保证未来所有程序自动适用。

版本记录、FRU仍沿用现有双扇区保护，不等于两个App副本；启动控制记录尚未实现。
下一步为受控包格式、B安装器、安装后CRC、启动控制掉电模型和3+3恢复。
信息查询继续以W25Q256持久记录为唯一运行时来源，不能提前删除App兼容初始化。
全系统设计见[OTA设计](../../test/doc/ota_design.md)。
