STM32 与 RK3588 UART通信协议规范

1. 文档概述

1.1 目的

本文档旨在定义 STM32 微控制器（主节点）与 RK3588 应用处理器（从节点）之间 UART 串口通信的底层协议规范。通过标准化的帧结构、Modbus CRC-16 校验机制与 TLV 数据封装格式，确保双端在复杂工业环境下的通信可靠性、可扩展性及数据完整性。

1.2 适用范围

本规范适用于所有基于 STM32 与 RK3588 硬件平台的嵌入式系统开发，涵盖底层驱动适配、协议栈解析、业务逻辑交互及异常处理等全链路开发工作。

1.3 版本历史

版本

日期

修改说明

v1.0.0

初始版本

确立基于 0x5A/0xA5 帧头帧尾、TLV 数据区及双重 CRC16 校验的基础协议框架。

v1.1.0

TLV Type 扩展

TLV Type 字段由 1 字节扩展为 2 字节，以支持更丰富的类型编号空间。

v1.2.0

主从翻转与CRC升级

MCU(STM32)改为主节点(Master)，RK3588改为从节点(Slave)；CRC算法由CRC16-CCITT替换为Modbus CRC-16；新增CRC查表法实现。

v1.3.0

Command容器与Tag索引命令化

新增通用命令容器机制，定义Type=0x0064作为命令载体，Tag索引1~63作为Command ID嵌入TLV Value字段。

v1.3.1

新增sequence字段

新增sequence字段，以增加通信双方的同步性

2. 物理层与链路层说明

为确保 STM32 与 RK3588 之间的高速稳定通信，物理层与链路层参数建议配置如下：

• 通信介质：TTL 电平直连或 RS485 差分总线（视硬件走线距离而定）。

• 波特率：115200 bps 或 460800 bps或921600（到时根据实际应用测试效果来定）。

• 数据位：8 bits。

• 停止位：1 bit。

• 校验位：None（无硬件校验，依赖协议层 Modbus CRC-16 校验）。

• 流控：None（无硬件流控，依赖应用层心跳与超时重传机制）。

3. 帧格式总览

完整的通信帧采用"帧头 + 长度 + 头部校验 + TLV数据区 + 帧校验 + 帧尾"的结构布局。具体字节分布如下表所示：

字段名称

偏移量(Bytes)

长度(Bytes)

说明

Header

0

1

固定字节 0x5A，标识帧起始

Length

1

2

大端序，表示 TLV 数据区的总字节数

Header CRC16

3

2

对 Header + Length 进行 Modbus CRC-16 校验

Sequence

5

8

无符号8字节整数

TLV Data

13

N

零个或多个 TLV 单元，N = Length 字段值

Frame CRC16

13+N

2

对 Length + Header CRC + Sequence + TLV Data 进行 Modbus CRC-16 校验

Footer

15+N

1

固定字节 0xA5，标识帧结束

TLV 单元结构：

子字段

偏移

长度(Bytes)

说明

Type

0

2

大端序，TLV 类型编号（2字节）

Length

2

2

表示 Value 字段的字节数

Value

4

Length

实际数据内容

帧结构图示：

+--------+--------+--------+--------+--------+--------+--------+

| Header |LengthH |LengthL |CRC_H1  |CRC_H2  |  ...   | Footer |

|  0x5A  |        |        |        |        | TLV    |  0xA5  |

+--------+--------+--------+--------+--------+--------+--------+

Offset: 0      1        3        5        5+N      7+N

4. 字段详细说明

4.1 Header (帧头)

• 字节序：单字节

• 取值范围：固定为 0x5A

• 说明：接收端通过持续扫描该字节实现帧同步。

4.2 Length (长度字段)

• 字节序：大端序（Big-Endian），高字节在前

• 取值范围：0x0000 ~ 0xFFFF

• 说明：仅表示 TLV 数据区的总字节数，不包含 Header、Header CRC16、Frame CRC16 及 Footer。

4.3 Header CRC16

• 校验范围：覆盖 Header (0x5A) + Length[0] + Length[1]，共计 3 个字节

• 说明：用于快速验证帧头与长度字段在传输中是否发生位翻转，避免后续解析越界。采用 Modbus CRC-16 算法计算。

4.4 Sequence

说明：rk3588往上发当前处理的命令的sequence，mcu往下发新命令时，上一次命令的sequence增1。

4.5 TLV 数据区

• 结构：Type (2 Bytes) + Length (2 Byte) + Value (Length Bytes)

• Type 编号空间：0x0000 ~ 0xFFFF，预留 65536 种类型

• 说明：支持在一帧内打包多个 TLV 单元，所有 TLV 单元的 Value 长度之和必须等于帧头部的 Length 字段值。

Type 的前 4 bit 为数据类型，支持的数据类型如下：

0：字符串类型

1：8bit bool型

2：8bit signed char型

3：8bit unsigned char型

4：16bit signed short int型

5：16bit unsigned short int型

6：32bit signed int型

7：32bit unsigned int型

8：64bit signed long型

9：64bit unsigned long型

Type 的剩下 12 bit 为 tag 索引：

0：command

16：param_fix_bit

17：param_fix_level

18：param_count

19：param_physical_address

20：param_fault_physical_address

21：param_loop

使用说明：当主节点（STM32）需要下发命令时，构造 TLV Type=0x0064 的 TLV 单元，Length 设为 1（仅 Command ID）或更大（含参数），Value 的第一个字节为 Command ID（1~63），后续字节为可选参数。从节点（RK3588）收到后，根据 Command ID 执行对应操作并返回结果帧。

0x01 (1)

内存粘滞地址测试 (gtest-stuck-address)

无

0x02 (2)

随机数测试 (gtest-rand)

无

0x03 (3)

异或测试 (gtest-xor)

无

0x04 (4)

减法测试 (gtest-sub)

无

0x05 (5)

乘法测试 (gtest-mul)

无

0x06 (6)

除法测试 (gtest-div)

无

0x07 (7)

或测试 (gtest-or)

无

0x08 (8)

与测试 (gtest-and)

无

0x09 (9)

递增序列测试 (gtest-seqinc)

无

0x0A (10)

全1位测试 (gtest-solidbits)

无

0x0B (11)

棋盘格测试 (gtest-checkerboard)

无

0x0C (12)

块序列测试 (gtest-blockseq)

无

0x0D (13)

0位行走测试 (gtest-walkbits0)

无

0x0E (14)

1位行走测试 (gtest-walkbits1)

无

0x0F (15)

位扩散测试 (gtest-bitspread)

无

0x10 (16)

位翻转测试 (gtest-bitflip)

无

0x11 (17)

保留 (reserved)

—

0x12 (18)

MarchX测试 (marchx)

无

0x13 (19)

MarchC-测试 (marchc-)

无

0x14 (20)

MarchC+测试 (marchc+)

无

0x15 (21)

IDD 0测试 (idd_0)

无

0x16 (22)

IDD 2P测试 (idd_2P)

无

0x17 (23)

IDD 2PS测试 (idd_2PS)

无

0x18 (24)

IDD 2N测试 (idd_2N)

无

0x19 (25)

IDD 2NS测试 (idd_2NS)

无

0x1A (26)

IDD 3P测试 (idd_3P)

无

0x1B (27)

IDD 3PS测试 (idd_3PS)

无

0x1C (28)

IDD 3N测试 (idd_3N)

无

0x1D (29)

IDD 3NS测试 (idd_3NS)

无

0x1E (30)

IDD 4R测试 (idd_4R)

无

0x1F (31)

IDD 4W测试 (idd_4W)

无

0x20 (32)

IDD 5测试 (idd_5)

无

0x21 (33)

IDD 5AB测试 (idd_5AB)

无

0x22 (34)

IDD 5PB测试 (idd_5PB)

无

0x23 (35)

IDD 6测试 (idd_6)

无

0x24 (36)

IDD 6ET测试 (idd_6ET)

无

0x29 (41)

gtest-stuck-address-result

测试结果数据

0x2A (42)

gtest-rand-result

测试结果数据

0x2B (43)

gtest-xor-result

测试结果数据

0x2C (44)

gtest-sub-result

测试结果数据

0x2D (45)

gtest-mul-result

测试结果数据

0x2E (46)

gtest-div-result

测试结果数据

0x2F (47)

gtest-or-result

测试结果数据

0x30 (48)

gtest-and-result

测试结果数据

0x31 (49)

gtest-seqinc-result

测试结果数据

0x32 (50)

gtest-solidbits-result

测试结果数据

0x33 (51)

gtest-checkerboard-result

测试结果数据

0x34 (52)

gtest-blockseq-result

测试结果数据

0x35 (53)

gtest-walkbits0-result

测试结果数据

0x36 (54)

gtest-walkbits1-result

测试结果数据

0x37 (55)

gtest-bitspread-result

测试结果数据

0x38 (56)

gtest-bitflip-result

测试结果数据

0x39 (57)

保留 (reserved)

—

0x3A (58)

marchx-result

测试结果数据

0x3B (59)

marchc--result

测试结果数据

0x3C (60)

marchc+-result

测试结果数据

0x3D (61)

idd_status_report

状态报告数据

0x3E (62)

reboot

0x3F (63)

frame_error

错误帧数据

当 TLV Type 为 0x5002 时，Value 的第一、第二个字节解析为 Command ID（取值范围 0x01~0x3F，即 1~63），对应原 Tag 索引 1~63 的功能定义。Value 的后续字节（Byte 1~N）为可选参数，具体格式视 Command ID 而定。

通用命令 ID（Command ID）映射表（Value Byte 0）：

Type (Hex)

名称

说明

Value 定义

0x5000

Command

通用命令容器（v1.3.0新增）

Byte 0 1: Command ID (1~63)

0x0000

keepalive

心跳包

空

0x0001~0x003F

Reserved

原测试项索引保留

视具体业务定义

0x00A0~0x00A5

Reserved

参数配置类索引保留

视具体业务定义

Type 编号空间扩展说明：

v1.3.0 版本新增通用命令容器（General Command Container）机制，允许通过一个统一的 TLV Type 承载 Tag 索引 1~63 作为 Command ID，实现命令的统一下发与调度。

4.6 Frame CRC16

• 校验范围：覆盖 Length[0] + Length[1] + 所有 TLV 数据

• 说明：确保核心业务数据的完整性。采用 Modbus CRC-16 算法计算。Header CRC16 与 Frame CRC16 自身不参与校验计算。

4.7 Footer (帧尾)

• 字节序：单字节

• 取值范围：固定为 0xA5

• 说明：标识一帧数据的正常结束。若接收到非 0xA5 字节，则判定为帧截断或异常。

5. CRC16 算法说明

本协议统一采用 Modbus CRC-16 算法，以保证双端计算结果的一致性。该算法在工业通信领域广泛应用（如 Modbus RTU 协议），具有计算简单、检错能力强的特点。

5.1 算法参数

参数

值

多项式 (Polynomial)

0x8005（反射形式 0xA001）

初始值 (Initial Value)

0xFFFF

输入反转 (Input Reflect)

True

输出反转 (Output Reflect)

True

结果异或值 (XorOut)

0x0000

5.2 C 语言参考实现

5.2.1 逐位计算法

#include <stdint.h>

uint16_t crc16_modbus(const uint8_t *data, uint16_t length) {

uint16_t crc = 0xFFFF;

for (uint16_t i = 0; i < length; i++) {

crc ^= data[i];  // 与当前数据字节异或

for (uint8_t j = 0; j < 8; j++) {

if (crc & 0x0001) {

crc = (crc >> 1) ^ 0xA001;  // 多项式 0xA001

} else {

crc >>= 1;

}

}

}

return crc;

}

5.2.2 查表法（推荐用于资源充足的 MCU）

查表法的核心思想是预先计算 256 个字节（0x00~0xFF）对应的 CRC 值，存储在一个长度为 256 的查找表中。计算时每次处理一个字节，通过查表代替逐位运算，将 8 次循环减少为 1 次查表，大幅提升计算效率。

const uint16_t crc_table[256] = {    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040,};

步骤一：预计算 CRC 查表

#include <stdint.h>

// 预计算 CRC 查表函数

void crc16_modbus_table_init(uint16_t *table) {

for (uint16_t i = 0; i < 256; i++) {

uint16_t crc = i;

for (uint8_t j = 0; j < 8; j++) {

if (crc & 0x0001) {

crc = (crc >> 1) ^ 0xA001;

} else {

crc >>= 1;

}

}

table[i] = crc;

}

}

步骤二：查表法 CRC 计算

// 查表法 CRC 计算（每次处理 1 个字节）

uint16_t crc16_modbus_lut(const uint8_t *data, uint16_t length,

const uint16_t *table) {

uint16_t crc = 0xFFFF;

for (uint16_t i = 0; i < length; i++) {

// 关键公式：crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF]

crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];

}

return crc;

}

步骤三：完整使用示例

// 完整使用示例

uint16_t crc_table[256];

void init(void) {

crc16_modbus_table_init(crc_table);

}

void process(void) {

uint8_t frame[] = {0x5A, 0x00, 0x09, 0x00, 0x08, 0x06, 0x20, 0x26, 0x08, 0x06, 0x10, 0x35};

uint16_t crc = crc16_modbus_lut(frame, sizeof(frame), crc_table);

// crc = 0x7C90

}

5.2.3 Modbus CRC-16 查表（256 项）

以下为预计算的完整 CRC-16/MODBUS 查表（多项式 0xA001）：

5.3 校验范围说明

• Header CRC16：对 0x5A + Length[0] + Length[1] 三个字节调用 crc16_modbus()

• Frame CRC16：对 Length[0] + Length[1] + 所有 TLV 数据字节调用 crc16_modbus()

6. 通信流程说明

6.1 主从关系

本协议采用主从（Master-Slave）架构，STM32 作为主节点（Master），RK3588 作为从节点（Slave）。主节点负责发起通信、下发命令和配置；从节点负责响应命令、上报状态与数据。

角色

设备

职责

Master（主节点）

STM32

下发命令、配置、时间同步；处理响应

Slave（从节点）

RK3588

响应命令、上报状态与数据、接收时间同步

6.2 命令/响应模式

采用严格的"请求-应答"机制：

1. Master（STM32）发送 Command / Config Req 帧

2. Slave（RK3588）收到后解析并执行，回复 Config Resp 或带状态码的响应帧

3. 若 Slave 无法执行，回复状态码为失败的 Config Resp

6.3 超时重传机制

参数

值

响应超时

500 ms

最大重传次数

3 次

重传间隔

500 ms（指数退避：500ms, 1000ms, 2000ms）

最终处理

3 次均超时，上报链路故障

6.4 心跳机制

参数

值

心跳周期

1000 ms（双方各自行发送）

心跳帧

Type=0x0001，Value 为空

超时判定

连续 3 个周期（3000ms）未收到对方心跳，判定链路断开

断开处理

触发底层重连或复位流程

7. 错误处理与异常恢复

7.1 CRC 校验失败处理

校验类型

处理策略

Header CRC16 失败

直接丢弃该帧，不解析 Length，继续寻找下一个 0x5A 帧头

Frame CRC16 失败

丢弃整个 TLV 数据区，记录一次 CRC 错误计数。若错误计数超过阈值（如 10 次/秒），触发链路告警

7.2 帧头丢失与重新同步

接收端维护一个状态机，包含以下状态：

状态

说明

IDLE

等待帧头 0x5A

SYNC_HEADER

收到 0x5A，等待 Length 字段

SYNC_LENGTH

收到 Length，验证 Header CRC16

SYNC_DATA

接收 TLV 数据区

SYNC_CRC

接收 Frame CRC16

SYNC_FOOTER

接收 Footer 0xA5，帧解析完成

若在非帧头位置检测到 0x5A，需结合后续字节的 CRC 校验进行二次确认，防止数据区内的 0x5A 导致伪同步。

7.3 Length 超限处理

若解析出的 Length 字段大于系统预设的最大缓冲区大小（如 1024 Bytes），立即丢弃该帧，并上报"帧长度异常"错误，防止内存溢出。

7.4 TLV 解析异常处理

在遍历 TLV 数据区时，若剩余字节数不足以解析当前 TLV 的 Length 或 Value，立即终止解析，标记该帧为"TLV 格式损坏"。

7.5 异常处理状态机流程图

+--------+

|  IDLE  | ← 等待 0x5A

+---+----+

| 收到 0x5A

v

+-------+-------+

| SYNC_HEADER  | ← 接收 Length (2 Bytes)

+-------+------+

|

v

+-------+-------+

| 验证 Header   |

| CRC16         |

+-------+------+

/        \

OK /          \ FAIL → 丢弃，回 IDLE

/            \

v              v

+-------+------+  +----+----+

| SYNC_LENGTH  |  |  IDLE   |

+-------+------+  +---------+

|

v

+-------+-------+

| SYNC_DATA     | ← 接收 TLV 数据区

+-------+------+

|

v

+-------+-------+

| SYNC_CRC      | ← 接收 Frame CRC16 (2 Bytes)

+-------+------+

|

v

+-------+-------+

| SYNC_FOOTER   | ← 接收 Footer 0xA5

+-------+------+

|

OK /    \ FAIL → 丢弃，回 IDLE

/      \

v        v

+---+--+   +--+---+

| 帧解析 |  | IDLE |

| 完成  |  +------+

+------+

8. 附录

8.1 CRC 验证工具与测试用例

在线验证工具：推荐使用 crccalc.com，选择 CRC-16/MODBUS 算法进行快速验算。

单元测试用例：开发阶段需覆盖以下测试：

测试编号

测试场景

预期结果

TC-01

正常帧解析与 CRC 校验

帧解析成功，CRC 校验通过

TC-02

篡改 Length 字段

Header CRC 拦截，帧丢弃

TC-03

篡改 TLV Value

Frame CRC 拦截，帧丢弃

TC-04

注入随机 0x5A 字节

状态机抗伪同步，正确重新同步

TC-05

Length 超限（> 1024）

帧丢弃，上报长度异常

TC-06

TLV Value 截断

帧标记为格式损坏，终止解析

TC-07

帧尾非 0xA5

帧丢弃，回 IDLE 状态

8.2 与其他协议对比说明

特性

本规范 (TLV + 双CRC)

标准 Modbus RTU

自定义固定帧长协议

数据封装

TLV 动态封装，扩展性极强

固定功能码+寄存器，扩展受限

结构固定，新增字段需改协议

校验机制

Modbus CRC-16 双校验

Modbus CRC-16 单校验

无校验或单一 Checksum

适用场景

复杂业务交互、多类型数据并发

简单工业仪表数据采集

极低资源 MCU 的固定指令控制

抗干扰能力

高（双重校验+帧尾确认）

中

低

扩展性

高（新增 TLV 类型即可扩展）

低（需定义新功能码）

低（需修改帧结构）

8.3 帧最小/最大长度

项目

长度 (Bytes)

说明

最小帧

8

Header(1) + Length(2) + HeaderCRC(2) + FrameCRC(2) + Footer(1) + TLV(0)

最大帧

1032

Header(1) + Length(2) + HeaderCRC(2) + TLV(1024) + FrameCRC(2) + Footer(1)

8.4 帧解析伪代码

function parseFrame(data):

pos = 0

while pos < len(data):

// 1. 查找帧头

if data[pos] != 0x5A:

pos += 1

continue

// 2. 读取 Length (2 Bytes, Big-Endian)

if pos + 2 > len(data):

break

length = (data[pos+1] << 8) | data[pos+2]

// 3. 验证 Length 是否超限

if length > MAX_TLV_BUFFER:

pos += 1

continue

// 4. 验证 Header CRC16 (Modbus)

header_crc = crc16_modbus(data[pos:pos+3])

stored_crc = (data[pos+3] << 8) | data[pos+4]

if header_crc != stored_crc:

pos += 1

continue

// 5. 计算 Frame CRC16 (Modbus)

frame_data = data[pos+1 : pos+5+length]

frame_crc = crc16_modbus(frame_data)

stored_frame_crc = (data[pos+5+length] << 8) | data[pos+6+length]

if frame_crc != stored_frame_crc:

pos += 5 + length + 3  // 跳过整个帧

continue

// 6. 验证帧尾

if data[pos+7+length] != 0xA5:

pos += 1

continue

// 7. 解析 TLV 数据区（Type 占 2 字节）

tlv_start = pos + 5

tlv_end = tlv_start + length

offset = tlv_start

while offset < tlv_end:

tlv_type = (data[offset] << 8) | data[offset+1]  // Type 为 2 字节

tlv_len = data[offset+2]                          // Length 为 1 字节

tlv_value = data[offset+3 : offset+3+tlv_len]

handleTLV(tlv_type, tlv_value)

offset += 3 + tlv_len  // Type(2) + Length(1) + Value(N)

// 8. 移动到下一帧

pos = tlv_end + 3  // + FrameCRC(2) + Footer(1)