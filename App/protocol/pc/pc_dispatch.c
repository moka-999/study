#include "pc_dispatch.h"

#include "pc_command.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"

#include <string.h>

static pc_dispatch_result_t build_response(
    const pc_protocol_frame_t *request,
    uint16_t response_command_id,
    const uint8_t *json,
    uint32_t json_length,
    pc_dispatch_response_t *response)
{
    return pc_protocol_build_response(request, response_command_id,
                                      json, json_length,
                                      response->data, sizeof(response->data),
                                      &response->length) == 0
               ? PC_DISPATCH_RESPONSE_READY
               : PC_DISPATCH_ERROR;
}

pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    pc_dispatch_response_t *response)
{
    static const uint8_t empty_json[] = {'{', '}'};
    cJSON *root = NULL;
    pc_dispatch_result_t result = PC_DISPATCH_NO_RESPONSE;

    if (!request || !response) return PC_DISPATCH_ERROR;
    response->length = 0U;

    if (pc_json_parse_object(request->json, request->json_length,
                             &root, NULL) != PC_JSON_OK) {
        return PC_DISPATCH_ERROR;
    }

    switch (request->command_id) {
    case PC_COMMAND_CLIENT_HEARTBEAT:
        /* 心跳包体固定为{}，不允许借心跳夹带其他状态或业务参数。 */
        if (root->child != NULL) {
            result = PC_DISPATCH_ERROR;
            break;
        }
        result = build_response(request, PC_COMMAND_SERVER_HEARTBEAT,
                                empty_json, sizeof(empty_json), response);
        break;

    case PC_COMMAND_QUERY_TESTER_INFO:
        /* 查询包体固定为{}；响应字段必须从W25Q256的有效FRU记录读取。 */
        if (root->child != NULL) {
            result = PC_DISPATCH_ERROR;
            break;
        }
        {
            fru_info_t fru;
            cJSON *info = NULL;
            char json[192] = {0};

            if (pc_tester_info_load(&fru) != 0) {
                result = PC_DISPATCH_ERROR;
                break;
            }
            info = cJSON_CreateObject();
            if (!info ||
                !cJSON_AddStringToObject(info, "testerName",
                                         fru.tester_name) ||
                !cJSON_AddStringToObject(info, "testerVersion",
                                         fru.hardware_revision) ||
                !cJSON_AddStringToObject(info, "testBoard",
                                         fru.test_board) ||
                !cJSON_PrintPreallocated(info, json, sizeof(json), 0)) {
                cJSON_Delete(info);
                result = PC_DISPATCH_ERROR;
                break;
            }
            result = build_response(request, PC_COMMAND_TESTER_INFO_RESP,
                                    (const uint8_t *)json,
                                    (uint32_t)strlen(json), response);
            cJSON_Delete(info);
        }
        break;

    case PC_COMMAND_SERVER_HEARTBEAT:
    case PC_COMMAND_TESTER_INFO_RESP:
        /* 这两个CommandID只允许MCU发送，PC反向发送属于方向错误。 */
        result = PC_DISPATCH_ERROR;
        break;

    case PC_COMMAND_START_TEST: {
        /* 当前只校验已冻结字段；投递RK和正式响应由后续业务适配层实现。 */
        pc_test_start_view_t start;
        if (pc_test_start_parse(root, &start) != PC_TEST_JSON_OK) {
            result = PC_DISPATCH_ERROR;
        }
        break;
    }

    default:
        /* 未冻结命令只接受合法JSON对象，不编造ACK或隐式转发给RK。 */
        break;
    }

    cJSON_Delete(root);
    return result;
}
