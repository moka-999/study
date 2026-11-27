/* RK内部请求/结果队列；不传递PC原始帧，RK发送Seq独立于PC发送Seq。 */
#ifndef RK_LINK_H
#define RK_LINK_H
#include "FreeRTOS.h"
#include "queue.h"
#include "frame.h"
#include "idd_stats.h"
#include <stdint.h>

/* 算法类别供PC入口核对，禁止把IDD名称经FUNC命令下发；不是线上的TLV字段。 */
typedef enum {
    RK_COMMAND_CATEGORY_FUNCTIONAL = 0, /* 功能测试，命令号1..20。 */
    RK_COMMAND_CATEGORY_PARAMETRIC     /* 参数测试，IDD命令号21..36。 */
} rk_command_category_t;

/* RK编号命令目录项，不是PC业务权限表。
 * command_id是0x5000 Value里的小端uint16；name是RK参考资料中的规范名称。
 * MCU把命令送到RK执行，不为每个算法实现一份本地执行函数。
 */
typedef struct {
    uint16_t command_id; /* 算法编号1..36；17=MATS，21..36按IDD原文编号。 */
    const char *name;    /* 指向静态只读字符串，例如"gtest-xor"；不允许修改/释放。 */
    rk_command_category_t category; /* 名称区分大小写；PC须同时核对算法类别。 */
} rk_command_descriptor_t;

/* 按编号或精确名称查只读命令表；未找到/名称为NULL返回NULL，无硬件副作用。
 * 返回项在整个运行期有效。找到只表示协议编号受支持，不代表电源/DDR已经就绪。
 */
const rk_command_descriptor_t *rk_command_find_by_id(uint16_t command_id);
const rk_command_descriptor_t *rk_command_find_by_name(const char *name);

/* 业务层投递的请求副本；发送帧由RK任务统一组装，调用者不操作UART。 */
typedef struct {
    uint16_t command_id;       /* FUNC 1..20只发编号，IDD 21..36附进入/退出动作；时序为0x25/0x26/0x28..0x34。 */
    uint32_t cookie;           /* 调用方关联令牌，不是任何一侧的线上Seq。 */
    uint32_t timeout_ms;       /* 结果等待毫秒数；0不限时，非0超时也不自动重发测试。 */
    uint32_t parameter_value;  /* 仅0x25/0x26/0x28..0x34使用；无应答设置，算法必须填0。 */
} rk_request_t;
typedef enum {
    RK_RESPONSE_COMPLETE,  /* FUNC完整最终包通过校验；IDD本地窗口完成且退出已发完，不代表电流合格。 */
    RK_RESPONSE_TIMEOUT,   /* 最终结果超时；RK可能仍在执行，后续请求暂停发送。 */
    RK_RESPONSE_TX_FAILED, /* 本端整帧未能进入UART发送队列；不自动重发。 */
    RK_RESPONSE_INVALID,   /* 已出队请求不合法，例如命令不支持或时限超范围。 */
    RK_RESPONSE_POWER_OFF, /* B1供电状态离开ON；未完成/未发送请求不能算测试通过。 */
    RK_RESPONSE_RECOVERED, /* 已报TIMEOUT的RK执行状态恢复已知；仅解除等待，不再报测试结果。 */
    RK_RESPONSE_MEASUREMENT_FAILED, /* IDD窗口读取失败/缺样/溢出；退出已发完，不产生0109 PASS。 */
    RK_RESPONSE_SENT      /* 时序整帧已进入UART队列；不表示RK收到、设置成功或硬件生效。 */
} rk_response_status_t;

/* 本地业务事件，不直接作为RK帧或PC JSON发送；仅COMPLETE的result有效。
 * 有限等待超时后先交付TIMEOUT；匹配迟到结果或B1断电后再交付一次RECOVERED。
 * 两个事件使用同一个cookie/sequence，队列背压不得改变交付顺序或覆盖事件。
 */
typedef struct {
    uint32_t cookie;             /* 原请求关联令牌，用于找到业务算法index。 */
    uint64_t sequence;           /* 原下行命令Seq，仅内部诊断；不是RK最近上报值或PC Seq。 */
    rk_response_status_t status; /* 通信/执行交付状态；不是DDR算法PASS/FAIL。 */
    rk_final_result_t result;    /* 仅COMPLETE有效；含0..2组失败明细，其他状态不是DDR测试FAIL。 */
    idd_stats_t measurement;     /* 仅IDD的COMPLETE有效；冻结的5s窗口统计，电流由逐点记录计算。 */
} rk_response_t;
/* 启动阶段建立的8槽队列；业务请用下面的接口，不直接清空或覆盖队列。 */
extern QueueHandle_t rkRequestQueue;
extern QueueHandle_t rkResponseQueue;
/* 任务上下文提交副本；0已入队，-1参数非法/队列未就绪/板卡未上电，-2队列满。
 * timeout_ms=0表示无限等待；非0转换后须在1..INT32_MAX tick内。
 * 成功不代表RK已收到或开始执行；无限等待不允许靠断开PC来取消/重发。
 */
int rk_link_request(const rk_request_t *request);
/* 非阻塞领取一个结果；1成功，0无结果/参数无效。仅一个业务消费者调用。 */
int rk_link_receive(rk_response_t *response);
/* 仅内部诊断，不是线上错误码；同一帧只记录实际拒绝路径的第一个原因。
 * NO_PENDING也包括完成后的重复结果，表示未交付业务，不等于RK协议错误。
 */
typedef enum {
    RK_RX_REJECT_NONE = 0,       /* 启动后尚无拒绝；counts[0]始终为0。 */
    RK_RX_REJECT_NO_PENDING,     /* 非普通心跳到达时没有在途算法。 */
    RK_RX_REJECT_SEQUENCE,       /* Seq早于本次命令或超出最近成功发送上界。 */
    RK_RX_REJECT_ALGORITHM,      /* 结构合法，但算法号不是当前在途算法。 */
    RK_RX_REJECT_PROGRESS,       /* 含3016但不是有效的5001+3016双TLV进度。 */
    RK_RX_REJECT_RESULT,         /* 最终包不满足3/5/7 TLV完整结果合同。 */
    RK_RX_REJECT_TLV,            /* 帧边界/CRC通过，但TLV层无法完整解包。 */
    RK_RX_REJECT_COUNT
} rk_rx_reject_reason_t;

/* MCU复位后从0累计，计数饱和于UINT32_MAX；新测试不会清除历史诊断。
 * 只统计完整帧的拒绝，不统计串口文字、CRC坏帧及字节间超时；这些不进业务解析。
 * 普通心跳不计拒绝。最近记录只在拒绝时更新，合法帧不会擦除诊断证据。
 * 已在TLV解包阶段发现的字段宽度/取值错误归TLV，其余按业务拒绝路径分类。
 */
typedef struct {
    uint32_t rejected_total;
    uint32_t counts[RK_RX_REJECT_COUNT];
    rk_rx_reject_reason_t last_reason;
    uint32_t last_tick;          /* RTOS tick，不是毫秒；用于与本机日志对时。 */
    uint64_t received_sequence;  /* frame_valid=1时有效。 */
    uint64_t command_sequence;   /* waiting=1时为本次算法Seq窗口下界。 */
    uint64_t last_tx_sequence;   /* waiting=1时为最近成功入UART队列的窗口上界。 */
    uint16_t expected_command;   /* waiting=0时为0，不能当成有效算法编号。 */
    uint16_t received_command;   /* 仅算法不匹配时有效，其余为0，未强行解读坏载荷。 */
    uint8_t tlv_count;           /* frame_valid=1时有效。 */
    uint8_t frame_valid;         /* 1表示已通过TLV解包，不代表符合最终业务格式。 */
    uint8_t waiting;             /* 拒绝当时是否有算法在途，包括超时后的未知状态。 */
} rk_rx_diagnostics_t;

/* 任务上下文只读快照：out非NULL返回0，NULL返回-1；短临界区保护64位字段。
 * 不清计数、不打印、不发PC帧、不改变RK状态；不用于ISR或控制接收门禁。
 */
int rk_link_get_rx_diagnostics(rk_rx_diagnostics_t *out);
/* 按本次测试的令牌和算法号读取最近进度。0..100为RK上报值，-1为尚无匹配进度。 */
int rk_link_progress_percent(uint32_t cookie, uint16_t command_id);
/* 新命令/心跳独立u64序号，启动从0递增，临界区保护；失败允许序号间隙。 */
uint64_t rk_link_next_sequence(void);
/* RK任务在SPL帧整帧成功入队后调用，推进算法和时序查询的应答Seq上界。 */
void rk_link_note_queued(uint64_t sequence);

/* TCP在本轮启动前绑定频率；token非0，值按PC原数传递，负数由RK解释为默认。
 * RK任务只读快照及记录发送结果；64位值使用短临界区保护。没有绑定时回复-1。
 * status返回0未发送、1整帧已入UART、-1发送失败/令牌失效；不是频率生效应答。
 */
void rk_link_frequency_begin(uint32_t token, int64_t value);
/* 每次实际拉起电源前安装；手动启动可指定MHz，其他情况填-1。正式启动令牌优先。 */
void rk_link_manual_frequency(int64_t value);
int64_t rk_link_frequency_get(uint32_t *token);
void rk_link_frequency_note_tx(uint32_t token, int success);
int rk_link_frequency_status(uint32_t token);
void rk_link_frequency_end(uint32_t token);

/** TCP电源编排上电前调用；token非0、每轮更新。清除旧就绪，0成功/-1参数错误。
 * 不发串口数据。确认必须发生在此后成功发出的MCU心跳Seq窗口内。
 */
int rk_link_ready_begin(uint32_t token);
/** 同一启动令牌查询：0等待、1普通心跳已确认、-1令牌不匹配；线程安全、非阻塞。
 * 这是项目约定的初始化完成信号，不是MCU独立验证DDR训练成功。
 */
int rk_link_ready_poll(uint32_t token);
/** 仅RK任务在普通心跳整帧成功进入UART队列后调用；失败发送不能扩大窗口。 */
void rk_link_ready_note_tx(uint64_t sequence);
/** 仅RK任务在完整帧通过长度/HCRC/FCRC/帧尾检查后调用；只认零TLV普通心跳，
 * 兼容单个0x0000空Value，拒绝进度/结果/未知TLV，不把串口文字视为就绪。
 */
void rk_link_ready_note_rx(const tlv_frame_t *frame);
#endif
