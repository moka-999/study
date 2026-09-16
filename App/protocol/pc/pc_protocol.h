/*
 * PC-MCU 固定头定义、响应组帧与JSON入口。
 *
 * TCP字节流解析状态已独立到pc_state_machine.h；本模块不定义CommandID
 * 业务含义，也不参与MCU-RK的0x5A/0xA5 TLV协议。
 * MCU发送头使用连接内独立u32流水号，ServiceID固定0；接收PC Seq只用于
 * 诊断，不回显、不参与RK序号或业务关联。
 */
#ifndef PC_PROTOCOL_H
#define PC_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* 帧状态机只需要不透明JSON指针，不能因此传递引入整个第三方解析器头。 */
typedef struct cJSON cJSON;

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

/*
 * 已解析的PC帧视图，不拥有任何动态内存。
 * 除json/json_length外的字段均由18字节头按网络大端序转换为MCU本机整数。
 */
typedef struct {
    uint32_t length;       /* 完整PDU字节数：18字节头+JSON正文。 */
    uint16_t version;      /* 当前必须为PC_PROTOCOL_VERSION。 */
    uint16_t flag;         /* 当前必须为PC_PROTOCOL_FLAG_JSON。 */
    uint16_t service_id;   /* 初版固定为0；非0帧由状态机拒绝。 */
    uint16_t command_id;   /* 当前PDU的请求或响应命令号。 */
    uint32_t sequence;     /* PC自身发送流水号；接收不改变MCU发送计数。 */
    uint16_t reserved;     /* 预留字段；当前发送响应时固定写0。 */
    const uint8_t *json;   /* 借用PC状态机内部缓冲中的正文，不需要free。 */
    uint32_t json_length;  /* 正文字节数，不包含额外的'\0'结束符。 */
} pc_protocol_frame_t;

typedef enum {
    PC_JSON_OK = 0,               /* 严格JSON对象校验通过。 */
    PC_JSON_ERROR_ARGUMENT,       /* 空指针、空正文或正文长度越界。 */
    PC_JSON_ERROR_UTF8,           /* 正文不是合法UTF-8字节序列。 */
    PC_JSON_ERROR_SYNTAX,         /* cJSON报告JSON语法错误。 */
    PC_JSON_ERROR_TRAILING_DATA,  /* 根对象后仍存在非空白字符。 */
    PC_JSON_ERROR_ROOT_TYPE,      /* 根节点不是{...}对象。 */
    PC_JSON_ERROR_DUPLICATE_KEY,  /* 任意层对象包含同名字段。 */
} pc_json_result_t;

/*
 * 构造MCU发送帧：ServiceID固定0，Seq由当前TCP连接显式传入。该接口不依赖
 * 请求帧，因此后续异步结果上报也能复用；MCU-RK Seq不得传入本函数。
 */
int pc_protocol_build_response(uint16_t response_command_id,
                               uint32_t mcu_tx_sequence,
                               const uint8_t *json,
                               uint32_t json_length,
                               uint8_t *output,
                               size_t output_capacity,
                               uint32_t *output_length);

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
