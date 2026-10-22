#ifndef MASTER_WIRE_H
#define MASTER_WIRE_H

#include "b2_protocol.h"

typedef struct {
    uint8_t tester_id;             /* 0=尚未设置，不发送testerId。 */
    uint8_t thermal_mask;          /* 位0..2选择本包携带的镜像。 */
    uint8_t thermal_mode[3];       /* 每项0常温、1高温；只通知显示模式。 */
} master_state_t;

typedef struct {
    uint8_t status;                /* 0成功、1参数错、2忙、3不支持、4执行失败。 */
    uint8_t tester_id;
    uint8_t site_id;
} master_reply_t;

/* 生成0004请求；返回实际字节数，输入越界/容量不足返回0，不操作外设。 */
size_t master_build(uint64_t sequence, const master_state_t *state,
                    uint8_t *out, size_t capacity);
/* 校验13字节头；返回整帧长度，非法头或全FF返回0。 */
size_t master_frame_size(const uint8_t header[B2_HEADER_SIZE]);
/* 完整校验应答、Seq和命令号；1成功解码（包括业务失败），0不可认领。 */
int master_decode(const uint8_t *frame, size_t length, uint64_t sequence,
                  master_reply_t *reply);

#endif
