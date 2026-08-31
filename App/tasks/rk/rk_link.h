/*
 * RK任务共享资源，只沿用已有内部队列载荷，不移植旧PC-TLV分发器。
 * PC JSON尚未投递算法或消费结果。pc_seq为旧关联值，不是新PC发送流水号；
 * 业务请求身份和多阶段结果的合同仍待双方冻结。
 */
#ifndef RK_LINK_H
#define RK_LINK_H

#include "FreeRTOS.h"
#include "queue.h"
#include "frame.h"
#include <stdint.h>

typedef struct {
    uint16_t cmd;
    uint8_t data[TLV_CMD_DATA_MAX];
    uint16_t data_len;
    uint64_t pc_seq;
} pc2rk_msg_t;

extern QueueHandle_t pcToRk_Queue;
extern QueueHandle_t rkToPc_Queue;

/* 任务上下文分配独立RK u64序号；重传沿用原序号，不写Flash。 */
uint64_t rk_link_next_sequence(void);

#endif
