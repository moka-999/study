#ifndef B2_PROTOCOL_H
#define B2_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* B2链路复用RK的线格式，不复用RK的命令/Type业务含义。所有容量以字节计。 */
#define B2_HEADER_SIZE       13U
#define B2_MAX_PAYLOAD       1024U
#define B2_MAX_FRAME         (B2_MAX_PAYLOAD + 16U)
#define B2_MAX_TLVS          16U
#define B2_RESPONSE_SIZE     37U
#define B2_MIRROR_COUNT      3U
#define B2_COMMAND_EXCHANGE  0x0004U

typedef enum {
    B2_STATUS_OK = 0,
    B2_STATUS_INVALID = 1,
    B2_STATUS_BUSY = 2,
    B2_STATUS_UNSUPPORTED = 3,
    B2_STATUS_FAILED = 4
} b2_status_t;

typedef struct {
    uint64_t sequence;             /* 请求流水号；应答逐位回显，不接入RK/PC流水号。 */
    uint16_t command;               // 请求命令；目前仅支持0004。
    b2_status_t status;             /* 请求里的字段是否合法、命令是否支持。 */
    uint8_t has_tester_id;          /* 0=不携带测试机号，保持当前配置；1=携带测试机号。 */
    uint8_t tester_id;              /* 本次携带的测试仪编号，1～21，没有该字段时保持当前配置。 */
    uint8_t thermal_mask;           /* 位0..2表示本次实际携带的镜像，未传的保持不变。 */
    uint8_t thermal_mode[B2_MIRROR_COUNT]; /* 0常温/1高温，只是模式，不是实测温度。 */
} b2_request_t;

/** 解码完整的单次CS报文。1=可回复（含业务错误），0=坏帧/无唯一命令，静默丢弃。
 * 未知TLV边界正确即可跳过；已知字段重复/长度错误整包拒绝，最多16个TLV。
 * 输入只借用至返回，输出不包含任何输入缓冲指针，不执行网络或灯光动作。
 */
int b2_request_decode(const uint8_t *frame, size_t length, b2_request_t *request);

/** 组应答：成功37字节；失败27字节且不带tester/site。容量不足返回0。
 * 成功tester_id允许0（尚未配置），site_id必须1..8。输出仅写实际报文长度。
 */
size_t b2_response_build(const b2_request_t *request, b2_status_t status,
    uint8_t tester_id, uint8_t site_id, uint8_t *out, size_t capacity);

#endif
