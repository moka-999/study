/*
 * PC-MCU TCP字节流解析状态机：状态 -> 字节处理函数查表。
 *
 * TCP只保证字节顺序，不保证一次recv()对应一个协议帧。本模块逐字节收集
 * 固定18字节头和JSON正文，并在头字段刚到齐时尽早检查Length/Version/Flag/
 * ServiceID。
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
    PC_SM_ERROR_SERVICE_ID, /* ServiceID不是初版固定值0。 */
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

/**
 * @brief 初始化PC字节流解析状态，丢弃此前收帧进度。
 * @param[out] sm 调用方持有的状态机；NULL时不操作。
 * @note 等价于pc_sm_reset，不清空载荷缓冲。
 */
void pc_sm_init(pc_sm_t *sm);

/**
 * @brief 清除收帧计数和错误，准备接收下一帧。
 * @param[in,out] sm 状态机；NULL时不操作。
 * @note 不擦除buffer；有效范围由计数隔离。此前借用的帧视图应停止使用。
 */
void pc_sm_reset(pc_sm_t *sm);

/**
 * @brief 消费一个PC TCP字节，按头部长度收齐PDU。
 * @param[in,out] sm 已初始化且由同一接收任务串行访问的状态机。
 * @param[in] byte 下一个接收字节。
 * @return MORE继续接收；FRAME_READY可取帧；ERROR参数、状态或帧头错误。
 * @note FRAME_READY后先处理帧再reset；ERROR由TCP任务关闭连接。两个状态下
 *       继续喂入会返回状态错误。仅校验帧边界和固定字段，不验证JSON正文。
 */
pc_sm_feed_result_t pc_sm_feed(pc_sm_t *sm, uint8_t byte);

/**
 * @brief 从已收齐PDU中解码帧头并借用JSON正文。
 * @param[in] sm 状态须为FRAME_READY且计数一致。
 * @param[out] frame 非NULL帧视图；失败保持原值。
 * @return 0成功；-1参数无效或帧尚未就绪。
 * @note json指向sm内部buffer，不保证NUL结尾，不可释放；必须在reset和后续
 *       feed之前用完。原始数据由状态机持有，函数不复制正文。
 */
int pc_sm_get_frame(const pc_sm_t *sm, pc_protocol_frame_t *frame);

/**
 * @brief 获取已收齐PDU的原始字节视图。
 * @param[in] sm 状态机，可为NULL。
 * @return FRAME_READY时返回内部buffer，否则NULL。
 * @note 有效长度为sm->received；借用期同pc_sm_get_frame。
 */
const uint8_t *pc_sm_frame_data(const pc_sm_t *sm);

/**
 * @brief 判断是否已收到部分字节且仍在收头或正文。
 * @param[in] sm 状态机，可为NULL。
 * @return 1存在半帧；0无半帧，包括NULL、就绪和错误状态。
 */
int pc_sm_is_receiving(const pc_sm_t *sm);

/**
 * @brief 查询最近一次解析错误。
 * @param[in] sm 状态机，可为NULL。
 * @return 保存的错误码；NULL返回PC_SM_ERROR_STATE；reset后为NONE。
 */
pc_sm_error_t pc_sm_error(const pc_sm_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* PC_STATE_MACHINE_H */
