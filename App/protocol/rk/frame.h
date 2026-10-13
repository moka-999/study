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
 * 高4位表示基础类型（1=bool，3=uint8，5=uint16，9=uint64），低12位是字段tag。
 * 例如发送异或算法：Type=0x5000，ValueLength=2，Value=03 00（CmdID=3）。
 * 下列长度指Value，不包括Type和ValueLength各自占用的2字节。
 */
#define TLV_TYPE_HEARTBEAT    0x0000U /* 仅接收兼容：空Value心跳；本端发送改为零TLV帧。 */
#define TLV_TYPE_CMD          0x5000U /* 下行命令容器：起始2字节为小端CmdID。 */
#define TLV_TYPE_RESULT_CMD   0x5001U /* 上行进度/结果所属算法：小端uint16，Value=2字节。 */
#define TLV_TYPE_RESULT_ADDR  0x9001U /* 上行结果：小端uint64首错地址，8字节；不单独判PASS。 */
#define TLV_TYPE_RESULT_DIE   0x3001U /* 失败颗粒编号：uint8、仅0/1，1字节；对应PC mirrorIndex。 */
#define TLV_TYPE_RESULT_TIME  0x9002U /* 上行结果：小端uint64测试耗时，8字节，单位ms。 */
#define TLV_TYPE_PROGRESS     0x3016U /* 与0x5001同帧：uint8进度0..100，1字节，不是完成标志。 */
#define TLV_TYPE_RESULT_FAIL  0x1017U /* 整体结论：1字节bool，0=两颗PASS、1=至少一颗FAIL。 */

/* 容量单位均为字节。线上Length只统计TLV区，不能与整帧长度混用。 */
#define TLV_PAYLOAD_MIN       4U     /* 非空TLV区下限；零TLV心跳另允许Length=0，1..3非法。 */
#define TLV_PAYLOAD_MAX       1024U  /* 所有TLV的头和值相加上限；不是单个Value上限。 */
#define TLV_VALUE_MAX         (TLV_PAYLOAD_MAX - 4U) /* 1020：单TLV扣除4字节头。 */
#define TLV_CMD_DATA_MAX      (TLV_VALUE_MAX - 2U)   /* 1018：命令参数还要扣除CmdID。 */
#define TLV_FRAME_MAX         (TLV_PAYLOAD_MAX + 16U) /* 1040：外层头5+Seq8+CRC2+尾1。 */
#define TLV_FRAME_MIN         16U    /* 零TLV心跳仍必须包含Seq、双CRC和帧尾。 */
#define TLV_MAX_PER_FRAME     16U    /* 单帧TLV个数上限；未知扩展也占一个名额。 */
#define TLV_CMD_FRAME_SIZE    22U    /* 无参数命令：外层16+TLV头4+CmdID2。 */
#define TLV_HEARTBEAT_FRAME_SIZE TLV_FRAME_MIN /* 本端普通心跳不附加任何TLV。 */

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
    uint8_t count;                      /* 有效units数0..16；0时payload_used必须也为0。 */
    uint16_t payload_used;              /* Value累计长度；线上Length另加每单元4字节。 */
    uint64_t sequence; /* 下行由MCU递增；RK上行填最近已收MCU Seq，不是测试身份或PC Seq。 */
} tlv_frame_t;

/* 通用报告解码的字段视图；正式任务仅用command_id校验双TLV进度。
 * 最终结果必须用rk_final_result_t，不将该单颗视图直接上报PC。
 */
typedef struct {
    uint16_t command_id;    /* 0x5001：必须与在途请求的算法编号一致。 */
    uint64_t error_address; /* 0x9001：首错地址，0表示未提供地址，不能据此判PASS。 */
    uint64_t elapsed_ms;    /* 0x9002：RK报告的算法耗时，不是MCU通信耗时。 */
    uint8_t die;            /* 0x3001：0=第一颗、1=第二颗，原样输出为PC mirrorIndex。 */
    uint8_t failed;         /* 0x1017：0=PASS、1=FAIL；必须确认该字段实际收到。 */
} rk_test_result_t;

#define RK_RESULT_DIE_COUNT 2U /* 双颗粒结果中的DDR编号仅为0和1，不是B1/B2编号。 */

/* 失败颗粒的一组相邻9001+3001；按die匹配颗粒，不按数组位置猜编号。 */
typedef struct {
    uint64_t error_address; /* 0x9001的完整64位地址；0也保留，不据此判PASS。 */
    uint8_t die;            /* 紧随该地址的0x3001：0=第一颗、1=第二颗。 */
} rk_failure_detail_t;

/* 一个完整3/5/7 TLV最终包；只存RK给出的失败明细，不生成PC结果项。 */
typedef struct {
    uint16_t command_id;    /* 唯一0x5001，算法编号，是否匹配请求由任务层检查。 */
    uint64_t elapsed_ms;    /* 唯一0x9002，整体测试耗时，单位ms，0有效。 */
    uint8_t failed;         /* 唯一0x1017：0=全部PASS，1=至少一颗FAIL。 */
    uint8_t failure_count;  /* 0/1/2组失败明细；failed须与是否存在明细一致。 */
    rk_failure_detail_t failures[RK_RESULT_DIE_COUNT]; /* 保留线上配对顺序；未用项为0。 */
} rk_final_result_t;

/* 通用报告解码有效位，不是线上字段；正式任务只接受PROGRESS，最终包另行严格解码。 */
enum {
    RK_REPORT_DETAILS = 1U,  /* 同帧四个详情均合法、各出现一次。 */
    RK_REPORT_PROGRESS = 2U, /* 同帧含合法0x5001算法号与0x3016进度，各一次。 */
    RK_REPORT_FAIL = 4U      /* 收到一个合法的0x1017。 */
};
typedef struct {
    rk_test_result_t result; /* PROGRESS仅使command_id有效；DETAILS/FAIL分别使详情/failed有效。 */
    uint8_t fields;          /* RK_REPORT_*按位或；纯心跳或未知扩展为0。 */
    uint8_t progress_percent; /* 0..100；只表示进度，不作为完成或DDR-ready依据。 */
} rk_report_t;

/* 清空帧；NULL时不操作。追加TLV前必须初始化。 */
void frame_init(tlv_frame_t *frame);
/* 追加完整Value并复制到帧内。成功0；指针/数量/容量/类型长度错误-1，
 * 失败不改变frame。长度为0时value可为NULL，源数据不能与frame重叠。
 */
int tlv_pack(tlv_frame_t *frame, uint16_t type, const uint8_t *value, uint16_t value_len);
/* 将0..16个TLV组为完整帧，seq为独立RK序号。成功返回16..1040字节，
 * 非法参数/容量不足返回0且不写输出。in/out不得重叠，out_max为真实可写容量。
 */
int tlv_pack_frame(const tlv_frame_t *in, uint64_t seq, uint8_t *out, uint16_t out_max);
/* Value前两字节为小端cmd，之后原样复制0..1018字节参数；仅做编码，
 * 不批准命令可执行性。任务层只允许已实现的无参数1..20命令。
 * 返回完整帧长度，错误为0；data_len为0时data可为NULL。
 */
int tlv_pack_cmd(uint16_t cmd, const uint8_t *data, uint16_t data_len,
                 uint64_t seq, uint8_t *out, uint16_t out_max);
/* 零TLV心跳固定16字节，Length=0；错误返回0。 */
int tlv_pack_heartbeat(uint64_t seq, uint8_t *out, uint16_t out_max);
/* 输入从5A开始的完整帧及真实可读总字节数，不是Seq起点或TLV长度。
 * 检查CRC、帧尾及全部TLV；成功返回0..16（0=零TLV心跳），失败-1并清空out。
 * 未知类型完整保留，交业务层决定是否忽略。输入与out不得重叠。
 */
int tlv_unpack(const uint8_t *wire, uint16_t wire_len, tlv_frame_t *out);
/* 单帧上行报告解码：进度由0x5001算法号与0x3016组成，不设置DETAILS有效位。
 * 最终详情须四项同帧各一次，可附带FAIL/进度；FAIL也可另帧，单独进度无效。
 * 成功0；重复、缺算法号、残缺详情、宽度/取值错或混有下行命令返回-1并清空report。
 * 未知扩展和心跳不补齐结果；函数不做跨帧或Seq匹配，由RK任务汇总。
 */
int rk_report_decode(const tlv_frame_t *frame, rk_report_t *report);
/* 解码一个完整最终包：5001+1017+9002，以及0/1/2组相邻9001+3001。
 * 必需标量各一次；失败颗粒die为0/1且不能重复，字段宽度沿用TLV规则表。
 * 标量可换序，但地址后必须立即跟所属die；不接受残缺、扩展或矛盾的最终包。
 * 成功0；参数/格式错误-1并清空result。frame/result不重叠，无硬件或JSON副作用。
 * 调用前须由tlv_unpack或状态机检查CRC；此函数不判断Seq、在途算法或业务完成。
 */
int rk_final_result_decode(const tlv_frame_t *frame, rk_final_result_t *result);
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
