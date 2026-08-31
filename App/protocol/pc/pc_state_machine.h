/*
 * PC-MCU TCP字节流解析状态机：状态 -> 字节处理函数查表。
 *
 * TCP只保证字节顺序，不保证一次recv()对应一个协议帧。本模块逐字节收集
 * 固定18字节头和JSON正文，并在头字段刚到齐时尽早检查Length/Version/Flag。
 * 它只负责传输层组帧，不解析JSON字段，也不处理CommandID业务语义。
 * 状态表在pc_state_machine.c；业务命令表在pc_dispatch.c，两者互相独立。
 *
 * 状态转移：
 *   HEADER -> BODY -> FRAME_READY
 *      |        |          |
 *      +--------+--------> ERROR
 *
 * FRAME_READY后调用pc_sm_get_frame()取得借用视图；处理完成后调用reset，
 * 再继续喂入同一次recv中剩余的字节，即可处理半包、粘包和连续多帧。
 */
#ifndef PC_STATE_MACHINE_H
#define PC_STATE_MACHINE_H

#include "pc_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_SM_STATE_HEADER = 0,   /* 正在收集和校验18字节固定头。 */
    PC_SM_STATE_BODY,         /* 固定头有效，正在收集JSON正文。 */
    PC_SM_STATE_FRAME_READY,  /* 完整PDU已收齐，等待上层取帧。 */
    PC_SM_STATE_ERROR,        /* 当前连接已失步，上层应关闭socket。 */
    PC_SM_STATE_COUNT,        /* 状态表边界，不是可运行状态；新增状态放在它之前。 */
} pc_sm_state_t;

typedef enum {
    PC_SM_ERROR_NONE = 0, /* 尚未发现组帧错误。 */
    PC_SM_ERROR_STATE,    /* 调用顺序错误或缓冲区状态不可继续。 */
    PC_SM_ERROR_LENGTH,   /* Length小于18或超过完整PDU上限。 */
    PC_SM_ERROR_VERSION,  /* Version不是当前固定值1。 */
    PC_SM_ERROR_FLAG,     /* Flag不是当前固定JSON标志8。 */
} pc_sm_error_t;

typedef enum {
    PC_SM_FEED_ERROR = -1,       /* 发生致命组帧错误，应关闭连接。 */
    PC_SM_FEED_MORE = 0,         /* 当前PDU尚未收完整。 */
    PC_SM_FEED_FRAME_READY = 1,  /* 恰好收齐一个完整PDU。 */
} pc_sm_feed_result_t;

typedef struct {
    pc_sm_state_t state;                  /* 当前显式解析状态。 */
    uint8_t buffer[PC_PROTOCOL_MAX_PDU];  /* 当前PDU，固定4KiB且不动态分配。 */
    uint32_t received;                    /* 已写入buffer的字节数。 */
    uint32_t expected_length;             /* 从头部Length解出的完整PDU长度。 */
    pc_sm_error_t error;                  /* 最近一次致命错误，reset前保留。 */
} pc_sm_t;

/* 新连接开始时初始化状态机。 */
void pc_sm_init(pc_sm_t *sm);

/* 完整帧处理完后回到HEADER；不清空buffer，旧字节由received=0隔离。 */
void pc_sm_reset(pc_sm_t *sm);

/* 喂入一个TCP字节：先检查状态表下标和缓冲边界，再调用当前状态的函数。
 * 返回继续接收、完整帧或致命错误；FRAME_READY/ERROR继续喂入会拒绝写缓冲。 */
pc_sm_feed_result_t pc_sm_feed(pc_sm_t *sm, uint8_t byte);

/*
 * 将FRAME_READY中的固定头解码为frame视图。frame.json借用sm->buffer，
 * 只能使用到下一次reset/feed之前，调用者不得释放该指针。
 */
int pc_sm_get_frame(const pc_sm_t *sm, pc_protocol_frame_t *frame);

/* 测试镜像需要原样回显PDU；仅在FRAME_READY状态返回内部帧首地址。 */
const uint8_t *pc_sm_frame_data(const pc_sm_t *sm);

/* 返回1表示已收到部分字节但尚未成帧，供TCP任务执行半帧超时判断。 */
int pc_sm_is_receiving(const pc_sm_t *sm);

/* 返回最近一次致命解析错误，供测试与诊断使用。 */
pc_sm_error_t pc_sm_error(const pc_sm_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* PC_STATE_MACHINE_H */
