/* MCU–RK专用编解码，不用于PC的固定头＋JSON协议。
 * 帧：5A | Length(BE16) | HCRC(LE16) | Seq(BE64) | TLV... | FCRC(LE16) | A5。
 * HCRC覆盖前3字节；FCRC覆盖Seq及全部TLV。CRC均为Modbus，初值FFFF。
 * TLV的Type/Length为大端，Value内多字节整数为小端。
 */
#ifndef RK_FRAME_H
#define RK_FRAME_H

#include <stdint.h>

/* 帧边界字节。Value中也允许出现5A/A5，接收端必须按Length定位，不能遇值截帧。 */
#define TLV_SYNC_HEADER       0x5AU  /* 帧首字节：开始接收Length和HCRC。 */
#define TLV_SYNC_FOOTER       0xA5U  /* 帧末字节：前面的FCRC通过后还要检查它。 */

/* Type是TLV字段类型，不是算法编号，也不是PC命令号。
 * 高4位表示基础类型（5=uint16，9=uint64，3=uint8），低12位是字段tag。
 * 例如发送异或算法：Type=0x5000，ValueLength=2，Value=03 00（CmdID=3）。
 * 下列长度指Value，不包括Type和ValueLength各自占用的2字节。
 */
#define TLV_TYPE_HEARTBEAT    0x0000U /* 心跳：空Value，不代表RK/DDR就绪。 */
#define TLV_TYPE_CMD          0x5000U /* 下行命令容器：起始2字节为小端CmdID。 */
#define TLV_TYPE_RESULT_CMD   0x5001U /* 上行结果：小端uint16命令号，Value=2字节。 */
#define TLV_TYPE_RESULT_ADDR  0x9001U /* 上行结果：小端uint64首错地址，8字节；0=PASS。 */
#define TLV_TYPE_RESULT_DIE   0x3001U /* 上行结果：uint8颗粒编号，1字节；映射未冻结。 */
#define TLV_TYPE_RESULT_TIME  0x9002U /* 上行结果：小端uint64测试耗时，8字节，单位ms。 */

/* 容量单位均为字节。线上Length只统计TLV区，不能与整帧长度混用。 */
#define TLV_PAYLOAD_MIN       4U     /* 最短TLV区：Type(2)+ValueLength(2)，如空心跳。 */
#define TLV_PAYLOAD_MAX       1024U  /* 所有TLV的头和值相加上限；不是单个Value上限。 */
#define TLV_VALUE_MAX         (TLV_PAYLOAD_MAX - 4U) /* 1020：单TLV扣除4字节头。 */
#define TLV_CMD_DATA_MAX      (TLV_VALUE_MAX - 2U)   /* 1018：命令参数还要扣除CmdID。 */
#define TLV_FRAME_MAX         (TLV_PAYLOAD_MAX + 16U) /* 1040：外层头5+Seq8+CRC2+尾1。 */
#define TLV_MAX_PER_FRAME     16U    /* 单帧TLV个数上限；未知扩展也占一个名额。 */
#define TLV_CMD_FRAME_SIZE    22U    /* 无参数命令：外层16+TLV头4+CmdID2。 */
#define TLV_HEARTBEAT_FRAME_SIZE 20U /* 独立心跳：外层16+TLV头4，无Value。 */

/* 保留完整Value，不拆除命令号；data_off只在所属帧内有效。 */
typedef struct {
    uint16_t type;      /* 完整Type，例如0x9001；已经转为本机数值。 */
    uint16_t data_off;  /* Value在所属帧payload数组中的起点，不是在线上帧的偏移。 */
    uint16_t data_len;  /* Value字节数，不包括4字节TLV头。 */
} tlv_unit_t;

/* 内存模型，不是可直接发送的C结构体：payload仅存放连续拼接的Value副本。 */
typedef struct {
    tlv_unit_t units[TLV_MAX_PER_FRAME]; /* 每个Value的类型、起点和长度。 */
    uint8_t payload[TLV_PAYLOAD_MAX];    /* 拥有数据副本，不借用串口接收缓冲。 */
    uint8_t count;                      /* 有效units数，组帧时必须为1..16。 */
    uint16_t payload_used;              /* Value累计长度；线上Length另加每单元4字节。 */
    uint64_t sequence; /* RK响应回显请求序号；不能作为PC发送序号。 */
} tlv_frame_t;

/* 四个TLV合并后的测试结果；error_address为0表示PASS，否则是首个失败地址。
 * die为RK原始颗粒编号，尚不等同于PC的siteId或mirrorIndex。
 */
typedef struct {
    uint16_t command_id;    /* 0x5001：必须与在途请求的算法编号一致。 */
    uint64_t error_address; /* 0x9001：0表示PASS，非0表示首个失败物理地址。 */
    uint64_t elapsed_ms;    /* 0x9002：RK报告的算法耗时，不是MCU通信耗时。 */
    uint8_t die;            /* 0x3001：原样保留；不自行转换为PC槽位。 */
} rk_test_result_t;

/* 清空帧；NULL时不操作。追加TLV前必须初始化。 */
void frame_init(tlv_frame_t *frame);
/* 追加完整Value并复制到帧内。成功0；指针/数量/容量/类型长度错误-1，
 * 失败不改变frame。长度为0时value可为NULL，源数据不能与frame重叠。
 */
int tlv_pack(tlv_frame_t *frame, uint16_t type, const uint8_t *value, uint16_t value_len);
/* 将1..16个TLV组为完整帧，seq为独立RK序号。成功返回20..1040字节，
 * 非法参数/容量不足返回0且不写输出。in/out不得重叠，out_max为真实可写容量。
 */
int tlv_pack_frame(const tlv_frame_t *in, uint64_t seq, uint8_t *out, uint16_t out_max);
/* Value前两字节为小端cmd，之后原样复制0..1018字节参数；仅做编码，
 * 不批准命令可执行性。任务层只允许已实现的无参数1..20命令。
 * 返回完整帧长度，错误为0；data_len为0时data可为NULL。
 */
int tlv_pack_cmd(uint16_t cmd, const uint8_t *data, uint16_t data_len,
                 uint64_t seq, uint8_t *out, uint16_t out_max);
/* 空Value心跳固定20字节；错误返回0。 */
int tlv_pack_heartbeat(uint64_t seq, uint8_t *out, uint16_t out_max);
/* 输入从5A开始的完整帧及真实可读总字节数，不是Seq起点或TLV长度。
 * 检查CRC、帧尾及全部TLV；成功返回1..16，失败-1并清空out。
 * 未知类型完整保留，交业务层决定是否忽略。输入与out不得重叠。
 */
int tlv_unpack(const uint8_t *wire, uint16_t wire_len, tlv_frame_t *out);
/* 原子合并四个结果TLV，顺序不限，每种必须恰好一次。成功0，缺失/重复/
 * 长度错或混有命令返回-1并清空result。未知扩展TLV、空心跳可忽略。
 */
int rk_result_decode(const tlv_frame_t *frame, rk_test_result_t *result);
/* 借用完整Value；调用方保证单元属于该帧且偏移/长度有效。 */
static inline const uint8_t *tlv_unit_get_data(const tlv_frame_t *f, const tlv_unit_t *u)
{
    return f->payload + u->data_off;
}
/* buf指向Length本身的两个大端字节，调用方保证可读。 */
static inline uint16_t tlv_get_length(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | buf[1];
}

#endif
