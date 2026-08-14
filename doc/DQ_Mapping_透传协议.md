# DQ Mapping 透传协议

> MCU 角色: 转发路由, 不解析数据
> 传输方式: KT-ADL 分段索引

---

## 数据流

```
PC ──TCP──► MCU ──UART──► RK3588    (请求)
PC ◄──TCP── MCU ◄──UART──  RK3588    (应答)
```

---

## 命令码

| 命令 | 值 | 方向 |
|---|---|---|
| Get DQ Mapping | `0xB4` | 双向 |
| Get MRC Training Data | `0xB2` | 单向 |
| Restore MRC Training Data | `0xB3` | 单向 |

---

## 交互流程

### Step 1: PC 查询总大小

```
PC → MCU → RK:
  68 01 00 68 B4 00 6D 16
  │        │  │  │  │  │
  sync     │  cmd │  │  end
           len=1  data[0]=0 (Index0)
                  checksum
```

```
RK → MCU → PC:
  68 04 00 68 B4 00 10 00 00 C5 16
  │        │  │  │  └─totalSize──┘ │  │
  sync     │  cmd                 XOR end
           len=4   totalSize = 0x00001000 = 4096 bytes
```

### Step 2~N: PC 分段请求

```
PC → MCU → RK (Index=1):
  68 03 00 68 B4 01 00 00 6E 16
                 │  └─index=1──┘
                 cmd  data[0]=1, data[1-2]=offset? or just index

RK → MCU → PC (返回256字节chunk):
  68 00 01 68 B4 [256 bytes data...] XX 16
     └─len=256──┘                    └XOR
```

### Step N+1: 最后一包

```
RK → PC:
  68 XX 00 68 B4 [剩余数据 <256] XX 16
```

---

## MCU 实现

```c
// proto.c 命令表加一行:
{CMD_DQ_MAP, _h_fwd},  // 原样透传, MCU 不解包

// _h_fwd 的行为:
// 1. 收到 B4 帧 → proto_pack 打包 → uart_send(0) → RK
// 2. RK 回复 → uart_router 自动转发 TCP → PC
// 3. MCU 不缓存、不分段、不解析 data[]
```

---

## 帧大小上限

| 参数 | 值 |
|---|---|
| PROTO_MAX_DATA | 256 bytes |
| 单帧最大 | 263 bytes (256+7) |
| 分段大小 | 256 bytes/chunk |
| MCU 缓冲区 | 无需额外分配 |

--
## 索引编码约定 (待 B1 板确认)

```
data[0] = index (0=查询总大小, 1~N=数据块序号)
data[1-2] = 可选: offset (小端), 或直接按 index 顺序
```

> 最终格式以 RK3588 BIOS 实现为准, MCU 不解析 data 内容。
