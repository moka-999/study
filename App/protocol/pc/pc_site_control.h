#ifndef PC_SITE_CONTROL_H
#define PC_SITE_CONTROL_H

#include "pc_test_message.h"

/* 以下接口均由同一个TCP任务调用；站点配置在RAM中跨连接保留。 */
int pc_site_enabled(void);

/* 提交完整且已解析的010C。返回静态错误名或NULL；deferred=1时待下电后还有终态应答。 */
const char *pc_site_configure(const pc_site_config_t *config, int *deferred);

/* 停止服务确认真实OFF且队列和串口已清理，或给出静态错误名后，结算等候的010C。 */
void pc_site_stop_finished(const char *error);
/* 原连接按受理顺序领取终态。返回1时error为NULL表示成功，否则为静态错误名。 */
int pc_site_take_reply(const char **error);
/* 断线只丢弃应答归属；已受理的最后一份开关配置仍等待物理下电后生效。 */
void pc_site_disconnected(void);

#endif
