/*
 * PC-MCU 协议的传输层与 JSON 入口。
 *
 * 本模块只负责固定 18 字节头、TCP 字节流组帧和 JSON 语法安全校验，
 * 不定义 CommandID 含义，也不参与 MCU-RK 的 0x5A/0xA5 TLV 协议。
 */
#ifndef PC_PROTOCOL_H
#define PC_PROTOCOL_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PC_PROTOCOL_HEADER_SIZE   18U
#define PC_PROTOCOL_VERSION       1U
#define PC_PROTOCOL_FLAG_JSON     8U

/*
 * PC工程师确认的MCU完整PDU上限，包含18字节头部。Length字段本身仍为u32，
 * 但超过4KiB时必须在继续接收包体前断开连接，避免占满FreeRTOS堆和任务栈。
 */
#define PC_PROTOCOL_MAX_PDU       4096U
#define PC_PROTOCOL_MAX_JSON_SIZE (PC_PROTOCOL_MAX_PDU - PC_PROTOCOL_HEADER_SIZE)

typedef enum {
    PC_PROTOCOL_ERROR_NONE = 0,
    PC_PROTOCOL_ERROR_STATE,
    PC_PROTOCOL_ERROR_LENGTH,
    PC_PROTOCOL_ERROR_VERSION,
    PC_PROTOCOL_ERROR_FLAG,
} pc_protocol_error_t;

typedef enum {
    PC_PROTOCOL_FEED_ERROR = -1,
    PC_PROTOCOL_FEED_MORE = 0,
    PC_PROTOCOL_FEED_FRAME_READY = 1,
} pc_protocol_feed_result_t;

typedef struct {
    uint32_t length;
    uint16_t version;
    uint16_t flag;
    uint16_t service_id;
    uint16_t command_id;
    uint32_t sequence; /* PC侧Seq；MCU响应必须原样回显，禁止混用RK侧Seq。 */
    uint16_t reserved;
    const uint8_t *json;
    uint32_t json_length;
} pc_protocol_frame_t;

typedef struct {
    uint8_t buffer[PC_PROTOCOL_MAX_PDU];
    uint32_t received;
    uint32_t expected_length;
    pc_protocol_error_t error;
    uint8_t frame_ready;
} pc_protocol_stream_t;

typedef enum {
    PC_JSON_OK = 0,
    PC_JSON_ERROR_ARGUMENT,
    PC_JSON_ERROR_UTF8,
    PC_JSON_ERROR_SYNTAX,
    PC_JSON_ERROR_TRAILING_DATA,
    PC_JSON_ERROR_ROOT_TYPE,
    PC_JSON_ERROR_DUPLICATE_KEY,
} pc_json_result_t;

/* 初始化或清空单个 TCP 连接的接收状态；断线和协议错误后都必须调用。 */
void pc_protocol_stream_reset(pc_protocol_stream_t *stream);

/*
 * 每次送入一个 TCP 字节。返回 FRAME_READY 后，调用者应先取帧和处理 JSON，
 * 再 reset 并继续送入同一 recv() 缓冲中的后续字节，从而自然支持粘包。
 */
pc_protocol_feed_result_t pc_protocol_stream_push_byte(
    pc_protocol_stream_t *stream, uint8_t byte);

/* frame 内的 json 指针由 stream 持有，仅在下一次 reset/push 前有效。 */
int pc_protocol_stream_get_frame(const pc_protocol_stream_t *stream,
                                 pc_protocol_frame_t *frame);

/*
 * 构造对PC请求的响应头：沿用请求ServiceID并原样回显请求Seq，CommandID由
 * 具体响应类型提供。MCU-RK Seq不得传入本函数。
 */
int pc_protocol_build_response(const pc_protocol_frame_t *request,
                               uint16_t response_command_id,
                               const uint8_t *json,
                               uint32_t json_length,
                               uint8_t *output,
                               size_t output_capacity,
                               uint32_t *output_length);

/* 返回最近一次致命组帧错误，供 TCP 层记录诊断后关闭连接。 */
pc_protocol_error_t pc_protocol_stream_error(const pc_protocol_stream_t *stream);

/*
 * cJSON 的内存钩子是进程级全局配置，必须在创建任何任务前调用一次。
 * STM32 固件传入 pvPortMalloc/vPortFree，主机单元测试可不调用并使用 libc。
 */
void pc_json_configure_allocator(void *(*allocate)(size_t),
                                 void (*deallocate)(void *));

/*
 * 精确按 json_length 解析，不依赖末尾 '\0'。除 JSON 语法外，还要求：
 * UTF-8 合法、根节点为对象、结尾仅有 JSON 空白、任意层对象不存在重复键。
 * 成功后由调用者使用 cJSON_Delete() 释放 root。
 */
pc_json_result_t pc_json_parse_object(const uint8_t *json,
                                      size_t json_length,
                                      cJSON **root,
                                      size_t *error_offset);

#ifdef __cplusplus
}
#endif

#endif /* PC_PROTOCOL_H */
