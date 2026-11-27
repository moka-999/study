#ifndef B2_TASK_H
#define B2_TASK_H

#include <stdint.h>

typedef struct {
    uint32_t valid_requests;
    uint32_t invalid_frames;
    uint32_t duplicate_requests;
    uint32_t last_valid_ms;
    uint32_t session; /* 每次从离线恢复时递增，供温度模式缓存区分两次上线。 */
    uint8_t online; /* 3秒没有结构完整、命令可识别的帧则离线；不推断温控/测试结果。 */
} b2_link_diagnostics_t;

/** 唯一B2链路任务：收CS事务、解析、等待业务应答、发布响应；不另设心跳任务。 */
void StartB2Task(void *argument);
/** 在任务上下文复制链路状态，online 按当前时刻计算。 */
void b2_link_diagnostics(b2_link_diagnostics_t *out);

#endif
