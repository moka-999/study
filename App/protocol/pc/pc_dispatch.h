/*
 * PC-MCU本地命令分发层。
 *
 * 输入是状态机已经收齐的完整帧；先严格解析JSON，再按CommandID查只读命令表，
 * 调用对应处理函数校验方向/字段并生成完整PC响应PDU。
 * 它不调用socket，也不接触RK链路；新增命令只需添加处理函数和表项。
 */
#ifndef PC_DISPATCH_H
#define PC_DISPATCH_H

#include "pc_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 三轨监测完整JSON小于320字节；384字节包含18字节头并保留格式余量。 */
#define PC_DISPATCH_RESPONSE_CAPACITY 384U

typedef enum {
    PC_DISPATCH_ERROR = -1,          /* 帧JSON语法或内部组包失败，应关闭连接。 */
    PC_DISPATCH_NO_RESPONSE = 0,     /* 合法帧，但当前没有本地同步响应。 */
    PC_DISPATCH_RESPONSE_READY = 1,  /* response中已有可直接发送的完整PDU。 */
} pc_dispatch_result_t;

typedef struct {
    uint8_t data[PC_DISPATCH_RESPONSE_CAPACITY]; /* 18字节头+JSON正文。 */
    uint32_t length;                           /* data中的有效字节数。 */
} pc_dispatch_response_t;

/*
 * 解析并查表处理一条PC请求；响应ServiceID固定0，并使用调用方提供的
 * 当前MCU发送Seq。只有返回RESPONSE_READY并成功发送后，TCP任务才推进计数。
 * 未登记的命令仍须通过严格JSON校验，通过后返回0x0006/notSupported，
 * 不静默丢弃也不隐式转发。
 * 解析得到的cJSON对象由本函数统一释放，原始帧/正文仍归调用方管理。
 * 调用方只需依据返回值发送response、继续接收或关闭连接。
 */
pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    uint32_t mcu_tx_sequence,
    pc_dispatch_response_t *response);

/** TCP有界等待后轮询OTA空闲超时；未启用OTA时为空操作。 */
void pc_dispatch_poll(void);
/** TCP连接结束时取消未提交下载，已提交请求保留到复位交B处理。 */
void pc_dispatch_disconnected(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_DISPATCH_H */
