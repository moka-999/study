#ifndef B2_SERVICE_H
#define B2_SERVICE_H

#include "b2_protocol.h"

typedef struct {
    b2_status_t status;
    uint8_t tester_id; /* 已生效的编号；0=尚未收到配置。 */
    uint8_t site_id;   /* 与当前生效IP配对；首次配置前读取本板槽位。 */
} b2_service_reply_t;

/** 调度前创建两个单槽队列；1成功，0内存不足。只调用一次。 */
int b2_service_prepare(void);
/** 仅B2任务调用；交由TCP任务顺序执行并等待结果，绝不在本任务直接改lwIP/PC状态。 */
void b2_service_exchange(const b2_request_t *request, b2_service_reply_t *reply);
/** 仅TCP任务周期调用。connected=1表示已接受PC连接；改IP请求将回busy。
 * 热模式更新不改变网络，允许测试期间执行；不控制加热、不写FRU。
 */
void b2_service_poll(int connected);
/** 仅TCP业务上下文调用。返回已知镜像位图；未知值不推断为常温。 */
uint8_t b2_service_thermal_modes(uint8_t modes[B2_MIRROR_COUNT]);
/** 返回正式配置使用的siteId；0表示尚未采用B2配置，调用方保留原本地测试标签。 */
uint8_t b2_service_site_id(void);
/* 仅TCP上下文核对010C的目标。已分配时须两项均匹配，单板直连时仍检查物理编号范围。 */
int b2_service_matches_site(uint16_t tester_id, uint16_t site_id);

#endif
