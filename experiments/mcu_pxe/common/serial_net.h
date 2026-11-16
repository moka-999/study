#ifndef SERIAL_NET_H
#define SERIAL_NET_H

#ifdef SERIAL_NET_EDK2
#include <Uefi.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
#else
#include <stdint.h>
#endif

/* 实验链路：COBS零分隔、CRC32；仅承载无VLAN的以太网帧，不兼容RK算法TLV。 */
#define SN_MTU 1500U
#define SN_ETH_MAX (SN_MTU + 14U)
#define SN_RAW_MAX (14U + SN_ETH_MAX + 4U)
#define SN_WIRE_MAX (SN_RAW_MAX + SN_RAW_MAX / 254U + 2U)
#define SN_VERSION 1U
#define SN_HELLO 1U
#define SN_SEND 2U
#define SN_POLL 3U
#define SN_RESET 4U
#define SN_STOP 5U
#define SN_REPLY 0x80U
#define SN_OK 0U
#define SN_EMPTY 1U
#define SN_INVALID 2U
#define SN_NOT_READY 3U
#define SN_IO_ERROR 4U

typedef struct {
    uint8_t op;
    uint8_t status;
    uint32_t session;
    uint32_t sequence;
    uint16_t length;
    uint8_t data[SN_ETH_MAX];
} sn_frame_t;

typedef struct {
    uint16_t used;
    uint8_t overflow;
    uint8_t encoded[SN_WIRE_MAX];
} sn_parser_t;

/* 返回编码字节数，含末尾0；参数越界返回0。输入输出缓冲不得重叠。 */
uint16_t sn_pack(const sn_frame_t *frame, uint8_t *wire, uint16_t capacity);
/* 每次输入一个字节：1=完整合法帧，0=待续，-1=完整坏帧/溢出。 */
int sn_feed(sn_parser_t *parser, uint8_t byte, sn_frame_t *frame);
void sn_parser_reset(sn_parser_t *parser);
uint32_t sn_crc32(const uint8_t *data, uint16_t length);

/* 相同事务只执行一次；请求及应答缓存由调用方静态分配，不放任务栈。 */
typedef struct {
    uint8_t valid;
    sn_frame_t request;
    sn_frame_t response;
} sn_server_t;
typedef void (*sn_execute_t)(void *context, const sn_frame_t *request, sn_frame_t *response);
/* 返回1=新执行，0=重放缓存，-1=非法序号/内容；不做自动网络重发。 */
int sn_server_handle(sn_server_t *server, const sn_frame_t *request,
                     sn_frame_t *response, sn_execute_t execute, void *context);

#endif
