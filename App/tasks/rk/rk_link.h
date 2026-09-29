/* RK内部请求/结果队列；不传递PC原始帧，RK发送Seq独立于PC发送Seq。 */
#ifndef RK_LINK_H
#define RK_LINK_H
#include "FreeRTOS.h"
#include "queue.h"
#include "frame.h"
#include <stdint.h>

/* RK支持的无参数算法目录项，不是PC业务权限表。
 * command_id是0x5000 Value里的小端uint16；name是RK参考资料中的规范名称。
 * MCU把命令送到RK执行，不为每个算法实现一份本地执行函数。
 */
typedef struct {
    uint16_t command_id; /* 算法编号1..20；17=MATS，21..36的IDD不在本表。 */
    const char *name;    /* 指向静态只读字符串，例如"gtest-xor"；不允许修改/释放。 */
} rk_command_descriptor_t;

/* 按编号或精确名称查只读命令表；未找到/名称为NULL返回NULL，无硬件副作用。
 * 返回项在整个运行期有效。找到只表示协议编号受支持，不代表电源/DDR已经就绪。
 */
const rk_command_descriptor_t *rk_command_find_by_id(uint16_t command_id);
const rk_command_descriptor_t *rk_command_find_by_name(const char *name);

/* 业务层投递的请求副本；发送帧由RK任务统一组装，调用者不操作UART。 */
typedef struct {
    uint16_t command_id;       /* 已实现的无参数功能测试1..20，含17=MATS。 */
    uint32_t cookie;           /* 调用方关联令牌，不是任何一侧的线上Seq。 */
    uint32_t timeout_ms;       /* 结果等待毫秒数；0不限时，非0超时也不自动重发测试。 */
} rk_request_t;
typedef enum {
    RK_RESPONSE_COMPLETE,  /* 收到完整且Seq+命令匹配的结果；PASS/FAIL再看首错地址。 */
    RK_RESPONSE_TIMEOUT,   /* 最终结果超时；RK可能仍在执行，后续请求暂停发送。 */
    RK_RESPONSE_TX_FAILED, /* 本端整帧未能进入UART发送队列；不自动重发。 */
    RK_RESPONSE_INVALID,   /* 已出队请求不合法，例如命令不支持或时限超范围。 */
    RK_RESPONSE_POWER_OFF, /* B1供电状态离开ON；未完成/未发送请求不能算测试通过。 */
    RK_RESPONSE_RECOVERED  /* 已报TIMEOUT的RK执行状态恢复已知；仅解除等待，不再报测试结果。 */
} rk_response_status_t;

/* 本地业务事件，不直接作为RK帧或PC JSON发送；仅COMPLETE的result有效。
 * 有限等待超时后先交付TIMEOUT；匹配迟到结果或B1断电后再交付一次RECOVERED。
 * 两个事件使用同一个cookie/sequence，队列背压不得改变交付顺序或覆盖事件。
 */
typedef struct {
    uint32_t cookie;             /* 原请求关联令牌，用于找到业务算法index。 */
    uint64_t sequence;           /* 实际RK链路Seq；未开始发送的请求可为0。 */
    rk_response_status_t status; /* 通信/执行交付状态；不是DDR算法PASS/FAIL。 */
    rk_test_result_t result;   /* 仅COMPLETE有效；其他状态不是DDR测试FAIL。 */
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
/* 新命令/心跳独立u64序号，启动从0递增，临界区保护；失败允许序号间隙。 */
uint64_t rk_link_next_sequence(void);
#endif
