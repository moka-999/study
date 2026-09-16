#ifndef PC_OTA_H
#define PC_OTA_H

#include "pc_dispatch.h"

/** 处理0x0901的严格JSON对象；root借用，响应仍使用MCU独立发送序号，不接触socket。 */
pc_dispatch_result_t pc_ota_handle(const pc_protocol_frame_t *request, uint32_t sequence,
                                    const cJSON *root, pc_dispatch_response_t *response);

#endif
