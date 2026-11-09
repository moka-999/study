/* IDD任务适配：复用纯会话状态机，衔接UART物理完成与监测新采样。 */
#ifndef RK_IDD_H
#define RK_IDD_H
#include "rk_link.h"

/* RK任务启动调用；静态互斥锁初始化并注册单一监测观察者。 */
void rk_idd_init(void);
/* 0进入帧已入队；-1尚未发送任何进入字节。仅RK任务、单事务调用。 */
int rk_idd_start(uint16_t command, uint64_t sequence);
/* 非阻塞推进发送/交付；1生成一次结果，0仍在途。TIMEOUT须确认掉电后恢复。 */
int rk_idd_process(rk_response_t *response);
/* 确认板卡已退出ON后清理，不能用来伪造发送完成或手动放行下一测试。 */
void rk_idd_power_off(void);
/* 显式业务时限到达：尽力退出；不能以取消代替UART完成事件。 */
void rk_idd_cancel(void);
#endif
