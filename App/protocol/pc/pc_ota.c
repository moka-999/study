/* OTA线上字段/分块解码在本模块；Flash、维护门、提交及复位由ota_app统一负责。 */
#include "pc_ota.h"
#include "pc_command.h"
#include "ota_app.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

/** 复用固定头组帧；正文对象归调用者，本函数不释放。 */
static pc_dispatch_result_t respond(uint16_t command, uint32_t sequence,
                                     cJSON *body, pc_dispatch_response_t *response)
{
    char json[PC_DISPATCH_RESPONSE_CAPACITY - PC_PROTOCOL_HEADER_SIZE];
    if (!body || !cJSON_PrintPreallocated(body, json, sizeof(json), 0)) return PC_DISPATCH_ERROR;
    return pc_protocol_build_response(command, sequence, (const uint8_t *)json, (uint32_t)strlen(json),
               response->data, sizeof(response->data), &response->length) == 0
           ? PC_DISPATCH_RESPONSE_READY : PC_DISPATCH_ERROR;
}

#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT

#define OTA_BASE64_MAX_CHARS (4U * ((OTA_DOWNLOAD_BLOCK_BYTES + 2U) / 3U))
/* 紧凑JSON按最长u32偏移预算；实际线上长度仍由公共帧状态机检查，空白也计入。 */
_Static_assert(PC_PROTOCOL_HEADER_SIZE + OTA_BASE64_MAX_CHARS +
    sizeof("{\"action\":\"write\",\"packageId\":\"FFFFFFFF\",\"offset\":4294967295,\"data\":\"\"}") - 1U
    <= PC_PROTOCOL_MAX_PDU, "OTA write整帧超出容量");

typedef struct {
    uint32_t package_id, package_bytes, offset, bytes;
    uint8_t data[OTA_DOWNLOAD_BLOCK_BYTES]; /**< 单块缓冲；完整解码通过后才访问下载区。 */
    uint8_t has_id, has_offset;     /**< 只回显已通过类型/范围检查的关联字段。 */
} ota_message_t;

typedef struct {
    const char *action;
    const char *fields[4];
    uint8_t field_count;
    ota_download_result_t (*execute)(const ota_message_t *message);
} ota_action_t;

static ota_download_result_t begin(const ota_message_t *m) { return ota_download_begin(m->package_id, m->package_bytes); }
static ota_download_result_t write_block(const ota_message_t *m) { return ota_download_write(m->package_id, m->offset, m->data, m->bytes); }
static ota_download_result_t finish(const ota_message_t *m) { return ota_download_finish(m->package_id); }
static ota_download_result_t cancel(const ota_message_t *m) { return ota_download_cancel(m->package_id); }

/* 每个动作只接纳表中字段；query的packageId是唯一可选字段。 */
static const ota_action_t actions[] = {
    {"begin",  {"action", "packageId", "packageSize"}, 3U, begin},
    {"write",  {"action", "packageId", "offset", "data"}, 4U, write_block},
    {"finish", {"action", "packageId"}, 2U, finish},
    {"query",  {"action", "packageId"}, 2U, NULL},
    {"cancel", {"action", "packageId"}, 2U, cancel},
};

static const char *const errors[] = {
    [OTA_DOWNLOAD_OK] = "", [OTA_DOWNLOAD_ARGUMENT] = "invalidArgument",
    [OTA_DOWNLOAD_STATE] = "invalidState", [OTA_DOWNLOAD_BUSY] = "busy",
    [OTA_DOWNLOAD_NOT_READY] = "notReady", [OTA_DOWNLOAD_OFFSET] = "offsetMismatch",
    [OTA_DOWNLOAD_DATA] = "dataMismatch", [OTA_DOWNLOAD_PACKAGE] = "packageInvalid",
    [OTA_DOWNLOAD_TARGET] = "targetMismatch", [OTA_DOWNLOAD_VERSION] = "versionRejected",
    [OTA_DOWNLOAD_CRC] = "crcMismatch", [OTA_DOWNLOAD_FLASH] = "flashError",
    [OTA_DOWNLOAD_COMMIT_ERROR] = "commitUnknown",
};

static const char *const states[] = {
    [OTA_DOWNLOAD_IDLE] = "idle", [OTA_DOWNLOAD_RECEIVING] = "receiving",
    [OTA_DOWNLOAD_READY] = "ready", [OTA_DOWNLOAD_COMMIT_UNKNOWN] = "commitUnknown",
    [OTA_DOWNLOAD_CANCELLED] = "cancelled", [OTA_DOWNLOAD_FAILED] = "failed",
};

/** cJSON数字只接纳0..UINT32_MAX整数；先比较再转换，NaN/溢出也不会进入强转。 */
static int read_u32(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0.0 && item->valuedouble <= UINT32_MAX)) return 0;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) return 0;
    *out = value;
    return 1;
}

static int read_id(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) != 8U) return 0;
    uint32_t value = 0U;
    for (uint32_t i = 0U; i < 8U; ++i) {
        char c = item->valuestring[i];
        if (c >= '0' && c <= '9') value = (value << 4U) | (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') value = (value << 4U) | (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    *out = value;
    return 1;
}

static int base64_digit(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/** 标准Base64，拒绝空白、URL变体、中间填充及非零填充位；最多输出3000字节。 */
static int decode_block(const cJSON *item, ota_message_t *m)
{
    if (!cJSON_IsString(item) || !item->valuestring) return 0;
    const char *s = item->valuestring;
    size_t length = strlen(s);
    if (length == 0U || length > OTA_BASE64_MAX_CHARS || (length & 3U) != 0U) return 0;
    m->bytes = 0U;
    for (size_t i = 0U; i < length; i += 4U) {
        int a = base64_digit(s[i]), b = base64_digit(s[i + 1U]);
        int c = s[i + 2U] == '=' ? 0 : base64_digit(s[i + 2U]);
        int d = s[i + 3U] == '=' ? 0 : base64_digit(s[i + 3U]);
        uint32_t pad = (s[i + 2U] == '=') + (s[i + 3U] == '=');
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (pad && i + 4U != length) || (s[i + 2U] == '=' && s[i + 3U] != '=') ||
            (pad == 2U && (b & 15) != 0) || (pad == 1U && (c & 3) != 0) ||
            3U - pad > sizeof(m->data) - m->bytes) return 0;
        uint32_t value = ((uint32_t)a << 18U) | ((uint32_t)b << 12U) | ((uint32_t)c << 6U) | (uint32_t)d;
        m->data[m->bytes++] = (uint8_t)(value >> 16U);
        if (pad < 2U) m->data[m->bytes++] = (uint8_t)(value >> 8U);
        if (pad == 0U) m->data[m->bytes++] = (uint8_t)value;
    }
    return 1;
}

/** 上层已拒绝重复键及内嵌NUL；这里严格校验动作字段，不默默忽略拼写错误。 */
static int parse_message(const cJSON *root, const ota_action_t *action, ota_message_t *m)
{
    m->has_id = read_id(cJSON_GetObjectItemCaseSensitive(root, "packageId"), &m->package_id);
    m->has_offset = read_u32(cJSON_GetObjectItemCaseSensitive(root, "offset"), &m->offset);
    for (const cJSON *item = root->child; item; item = item->next) {
        size_t i;
        for (i = 0U; i < action->field_count; ++i) if (strcmp(item->string, action->fields[i]) == 0) break;
        if (i == action->field_count) return 0;
    }
    for (size_t i = 0U; i < action->field_count; ++i) {
        const char *name = action->fields[i];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
        if (strcmp(name, "packageId") == 0) {
            if (!m->has_id && (action->execute != NULL || item != NULL)) return 0;
        } else if (!item) return 0;
    }
    if (action->execute == begin && (!read_u32(cJSON_GetObjectItemCaseSensitive(root, "packageSize"), &m->package_bytes) ||
        m->package_bytes < 116U || m->package_bytes > 2097152U)) return 0;
    if (action->execute == write_block && (!m->has_offset || !decode_block(cJSON_GetObjectItemCaseSensitive(root, "data"), m))) return 0;
    return 1;
}

static int add_id(cJSON *body, const char *key, uint32_t id)
{
    char text[9];
    (void)snprintf(text, sizeof(text), "%08lX", (unsigned long)id);
    return cJSON_AddStringToObject(body, key, text) != NULL;
}

/** 查询在每次请求中重新加载持久化信息；只有运行目标包且已健康确认才返回0903。 */
static int query(cJSON *body, const ota_message_t *m, const ota_download_progress_t *p, uint16_t *command)
{
    ota_app_status_t status;
    if (ota_app_load_status(&status) != 0) return 0;
    char version[18];
    (void)snprintf(version, sizeof(version), "%u.%u.%u", status.version[0], status.version[1], status.version[2]);
    int active = p->state == OTA_DOWNLOAD_RECEIVING || p->state == OTA_DOWNLOAD_READY || p->state == OTA_DOWNLOAD_COMMIT_UNKNOWN;
    int complete = m->has_id && m->package_id == status.active_package_id && status.confirmed && !active;
    const char *state = complete ? "confirmed" : p->state == OTA_DOWNLOAD_IDLE ?
                        (status.confirmed ? "confirmed" : "trial") : states[p->state];
    if (!cJSON_AddStringToObject(body, "status", "completed") ||
        !cJSON_AddStringToObject(body, "state", state) ||
        !cJSON_AddStringToObject(body, "firmwareVersion", version) ||
        !cJSON_AddBoolToObject(body, "confirmed", status.confirmed)) return -1;
    if (complete) *command = PC_COMMAND_MCU_OTA_COMPLETE;
    else if (!add_id(body, "activePackageId", status.active_package_id)) return -1;
    if (p->state == OTA_DOWNLOAD_RECEIVING) {
        cJSON *download = cJSON_AddObjectToObject(body, "download");
        if (!download || !add_id(download, "packageId", p->package_id) ||
            !cJSON_AddNumberToObject(download, "receivedBytes", p->received_bytes) ||
            !cJSON_AddNumberToObject(download, "totalBytes", p->total_bytes)) return -1;
    }
    return 1;
}

#endif

pc_dispatch_result_t pc_ota_handle(const pc_protocol_frame_t *request, uint32_t sequence,
                                    const cJSON *root, pc_dispatch_response_t *response)
{
    cJSON *body = cJSON_CreateObject();
    uint16_t command = PC_COMMAND_MCU_OTA_RESP;
    const char *error = NULL;
    pc_dispatch_result_t result = PC_DISPATCH_ERROR;
    if (!body) return result;
#if defined(ENABLE_OTA_BOOT) && ENABLE_OTA_BOOT
    ota_message_t message = {0};
    const ota_action_t *action = NULL;
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(name) || !name->valuestring) error = "invalidArgument";
    else {
        for (size_t i = 0U; i < sizeof(actions) / sizeof(actions[0]); ++i) {
            if (strcmp(name->valuestring, actions[i].action) == 0) { action = &actions[i]; break; }
        }
        if (!action) error = "notSupported";
    }
    ota_download_poll();
    if (action) {
        if (!parse_message(root, action, &message)) error = "invalidArgument";
        if (!cJSON_AddStringToObject(body, "action", action->action) ||
            (message.has_id && !add_id(body, "packageId", message.package_id)) ||
            (action->execute == write_block && message.has_offset &&
             !cJSON_AddNumberToObject(body, "offset", message.offset))) goto done;
        ota_download_progress_t progress;
        if (!error && action->execute) {
            ota_download_result_t code = action->execute(&message);
            if (code != OTA_DOWNLOAD_OK) error = errors[code];
        }
        ota_download_get_progress(&progress);
        if (!error && action->execute == NULL) {
            int loaded = query(body, &message, &progress, &command);
            if (loaded < 0) goto done;
            if (!loaded) error = "flashError";
        } else if (!error) {
            if (!cJSON_AddStringToObject(body, "status", action->execute == finish ? "accepted" : "completed") ||
                !cJSON_AddStringToObject(body, "state", states[progress.state])) goto done;
            if (action->execute == begin && !cJSON_AddNumberToObject(body, "blockSize", OTA_DOWNLOAD_BLOCK_BYTES)) goto done;
        }
        if (((!error && (action->execute == begin || action->execute == write_block)) ||
             (error && progress.state == OTA_DOWNLOAD_RECEIVING && message.has_id && message.package_id == progress.package_id)) &&
            !cJSON_AddNumberToObject(body, "nextOffset", progress.received_bytes)) goto done;
        if (error && progress.state == OTA_DOWNLOAD_COMMIT_UNKNOWN &&
            !cJSON_AddStringToObject(body, "state", "commitUnknown")) goto done;
    }
#else
    (void)root;
    /* 未启用完整B合同的普通/隔离镜像明确拒绝OTA，不能假装已建立安全回退链。 */
    error = "notSupported";
#endif
    if (error) {
        command = PC_COMMAND_COMMAND_RESP;
        if (!cJSON_AddNumberToObject(body, "requestCommandId", request->command_id) ||
            !cJSON_AddStringToObject(body, "status", "failed") ||
            !cJSON_AddStringToObject(body, "errorCode", error)) goto done;
    }
    result = respond(command, sequence, body, response);
done:
    cJSON_Delete(body);
    return result;
}
