#include "pc_dispatch.h"

#include "pc_command.h"
#include "pc_test_message.h"
#include "pc_tester_info.h"

#include <string.h>

/*
 * 每个命令处理函数只负责本命令的字段校验和本地响应。
 * request/root均为借用：root由分发入口统一释放，处理函数不得释放或保存它。
 * response由TCP任务提供；需要回复时填入完整PDU，不在这里调用socket或RK。
 */
typedef pc_dispatch_result_t (*pc_command_handler_t)(
    const pc_protocol_frame_t *request,
    const cJSON *root,
    pc_dispatch_response_t *response);

typedef struct {
    uint16_t command_id;           /* 查找键：PC协议的CommandID，不是RK命令号。 */
    pc_command_handler_t handler; /* 命中后调用的唯一处理函数。 */
} pc_command_entry_t;

/* 复用统一组帧入口，确保所有本地响应都回显请求的ServiceID和PC Seq。 */
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

/* PC主动心跳：只接受空对象{}，返回MCU心跳，不夹带其他状态。 */
static pc_dispatch_result_t handle_client_heartbeat(
    const pc_protocol_frame_t *request,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    static const uint8_t empty_json[] = {'{', '}'};

    if (root->child != NULL) return PC_DISPATCH_ERROR;
    return build_response(request, PC_COMMAND_SERVER_HEARTBEAT,
                          empty_json, sizeof(empty_json), response);
}

/* 查询测试仪信息：每次从W25Q256读取有效FRU，再生成三个已冻结字段。 */
static pc_dispatch_result_t handle_query_tester_info(
    const pc_protocol_frame_t *request,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    fru_info_t fru;
    cJSON *info = NULL;
    char json[192] = {0};
    pc_dispatch_result_t result;

    /* 先校验查询必须为{}，非法请求不能触发Flash访问。 */
    if (root->child != NULL) return PC_DISPATCH_ERROR;
    if (pc_tester_info_load(&fru) != 0) return PC_DISPATCH_ERROR;

    /* info是本函数创建的响应对象，与借用的请求root分开管理。 */
    info = cJSON_CreateObject();
    if (!info ||
        !cJSON_AddStringToObject(info, "testerName", fru.tester_name) ||
        !cJSON_AddStringToObject(info, "testerVersion", fru.hardware_revision) ||
        !cJSON_AddStringToObject(info, "testBoard", fru.test_board) ||
        !cJSON_PrintPreallocated(info, json, sizeof(json), 0)) {
        cJSON_Delete(info);
        return PC_DISPATCH_ERROR;
    }

    result = build_response(request, PC_COMMAND_TESTER_INFO_RESP,
                            (const uint8_t *)json,
                            (uint32_t)strlen(json), response);
    cJSON_Delete(info);
    return result;
}

/* 只允许MCU发送的响应命令若从PC收到，统一报告方向错误。 */
static pc_dispatch_result_t reject_response_command(
    const pc_protocol_frame_t *request,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    (void)request;
    (void)root;
    (void)response;
    return PC_DISPATCH_ERROR;
}

/* START_TEST当前只校验已冻结字段；投递RK和正式响应仍待业务适配。 */
static pc_dispatch_result_t handle_start_test(
    const pc_protocol_frame_t *request,
    const cJSON *root,
    pc_dispatch_response_t *response)
{
    pc_test_start_view_t start;

    (void)request;
    (void)response;
    return pc_test_start_parse(root, &start) == PC_TEST_JSON_OK
               ? PC_DISPATCH_NO_RESPONSE
               : PC_DISPATCH_ERROR;
}

/*
 * PC命令表：新增命令时编写一个处理函数，并在此添加一项，CommandID不可重复。
 * 当前合同按CommandID查找，ServiceID仅沿用现有回显规则，不新增业务限制。
 * 命令集合在编译时确定，使用只读表，无需启动注册或额外RAM登记表。
 * 明确列出已冻结的反向命令，避免它们落入“未知命令暂不响应”的路径。
 */
static const pc_command_entry_t s_command_table[] = {
    {PC_COMMAND_SERVER_HEARTBEAT,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_CLIENT_HEARTBEAT,  handle_client_heartbeat},  /* PC -> MCU，回复{}。 */
    {PC_COMMAND_QUERY_TESTER_INFO, handle_query_tester_info}, /* PC -> MCU，读取FRU。 */
    {PC_COMMAND_TESTER_INFO_RESP,  reject_response_command}, /* MCU -> PC，拒收。 */
    {PC_COMMAND_START_TEST,        handle_start_test},        /* 仅校验测试算法。 */
};

pc_dispatch_result_t pc_dispatch_handle(
    const pc_protocol_frame_t *request,
    pc_dispatch_response_t *response)
{
    cJSON *root = NULL;
    pc_dispatch_result_t result = PC_DISPATCH_NO_RESPONSE;

    if (!request || !response) return PC_DISPATCH_ERROR;
    /* 即使本次不响应或报错，也不能让TCP任务误发上一次留下的PDU。 */
    response->length = 0U;

    /* 所有命令（包括未知命令）都先经过一次严格JSON对象校验。 */
    if (pc_json_parse_object(request->json, request->json_length,
                             &root, NULL) != PC_JSON_OK) {
        return PC_DISPATCH_ERROR;
    }

    /* 小表顺序查找，命中只执行一次；未命中保持不响应，也不隐式转发RK。 */
    for (size_t i = 0U; i < sizeof(s_command_table) / sizeof(s_command_table[0]); i++) {
        if (s_command_table[i].command_id == request->command_id) {
            result = s_command_table[i].handler(request, root, response);
            break;
        }
    }

    /* 统一收尾：处理函数无论成功、拒绝还是不响应，请求JSON都只释放一次。 */
    cJSON_Delete(root);
    return result;
}
