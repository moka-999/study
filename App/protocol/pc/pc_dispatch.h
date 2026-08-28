/*
 * PC-MCU本地命令分发层。
 *
 * 输入是状态机已经收齐的完整帧；本模块负责严格JSON解析、命令方向/字段校验，
 * 并在本地命令需要回复时生成完整PC响应PDU。它不调用socket，也不接触RK链路。
 */
#ifndef PC_DISPATCH_H
#define PC_DISPATCH_H

#include "pc_protocol.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ponytail: 当前本地响应最大102字节，256字节足够；新增更大响应时再扩容。 */
#define PC_DISPATCH_RESPONSE_CAPACITY 256U

typedef enum {
    PC_DISPATCH_ERROR = -1,          /* JSON或业务合同非法，TCP层应关闭连接。 */
    PC_DISPATCH_NO_RESPONSE = 0,     /* 合法帧，但当前没有本地同步响应。 */
    PC_DISPATCH_RESPONSE_READY = 1,  /* response中已有可直接发送的完整PDU。 */
} pc_dispatch_result_t;

typedef struct {
    uint8_t data[PC_DISPATCH_RESPONSE_CAPACITY]; /* 18字节头+JSON正文。 */
    uint32_t length;                           /* data中的有效字节数。 */
} pc_dispatch_response_t;

/* 解析并处理一条PC请求；响应始终回显请求ServiceID和PC Seq。 */
pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    pc_dispatch_response_t *response);

#ifdef __cplusplus
}
#endif

#endif /* PC_DISPATCH_H */
