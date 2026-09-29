/* RK逐字节状态机；时间基于FreeRTOS tick，间隔达到50ms丢弃半帧。 */
#ifndef RK_STATE_MACHINE_H
#define RK_STATE_MACHINE_H
#include <stdint.h>
#include "frame.h"
/* 半帧相邻字节最大等待时间，单位ms；不用于算法执行超时或重发。 */
#define RK_BYTE_TIMEOUT_MS 50U

/* 状态值只在MCU内部使用，与s_state_handlers的指定下标一一对应。 */
typedef enum {
    TLV_SM_IDLE,   /* 等待5A；遇到其他字节继续寻找，不累积噪声。 */
    TLV_SM_HEADER, /* 收集Length(2)+HCRC(2)，完整缓冲长度达到5。 */
    TLV_SM_DATA,   /* 收集Seq(8)+TLV区；TLV起点为帧内偏移13。 */
    TLV_SM_CRC,    /* 收集FCRC(2)，将收到的CRC纳入重算，余数应为0。 */
    TLV_SM_FOOTER, /* 验证A5，发布完整帧边界；TLV内部仍由get_frame校验。 */
    TLV_SM_COUNT   /* 状态数量5，用于分发表边界检查，不是可运行状态。 */
} tlv_sm_state_t;

/* 每条逻辑RK串口独占一个实例；不能由多个任务同时feed/reset。 */
typedef struct {
    tlv_sm_state_t state; /* 下一字节应交给哪个状态处理函数。 */
    uint8_t buf[TLV_FRAME_MAX]; /* 从5A开始的整帧副本，最大1040字节。 */
    uint16_t pos;        /* 已写入buf的字节数，也是下一次写入下标。 */
    uint16_t expect;     /* 当前阶段结束时的绝对字节数。 */
    uint16_t ready_len;  /* 0无完整帧；下一次feed会使上次结果失效。 */
    uint32_t last_tick;  /* 上次feed的RTOS tick，不是毫秒或报文Seq。 */
} tlv_sm_t;
/* 初始化或放弃半帧；NULL时不操作。 */
void tlv_sm_init(tlv_sm_t *sm);
void tlv_sm_reset(tlv_sm_t *sm);
/* 消费字节：0继续，1边界/CRC通过，-1错误已恢复。任务上下文串行调用。
 * 返回1后、喂下个字节前必须get_frame；内部TLV损坏仍可能使get_frame失败。
 */
int tlv_sm_feed(tlv_sm_t *sm, uint8_t byte);
/* 仅提取最近完整帧；成功1..16，未就绪/解析失败-1并清空frame。
 * 输出拥有载荷副本，不依赖sm后续生命周期。
 */
int tlv_sm_get_frame(tlv_sm_t *sm, tlv_frame_t *frame);
/* 检查字节间间隔；达到timeout_ms复位返回1，否则0。不是测试完成超时。 */
int tlv_sm_check_timeout(tlv_sm_t *sm, uint32_t timeout_ms);
#endif
