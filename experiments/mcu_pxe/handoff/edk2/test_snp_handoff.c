/* 使用真实 UEFI 头、SNP 驱动和 V2 编解码；UART/Boot Services 由内存对端模拟。 */
#include "SerialNetSnpDxe.c"

#define TEST_STRING_(x) #x
#define TEST_STRING(x) TEST_STRING_(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " TEST_STRING(__LINE__) " " #x "\n"); } while (0)
#define TEST_ETH_HEADER 14U
#define TEST_IP_HEADER 20U
#define TEST_UDP_HEADER 8U
#define TEST_BOOTP_SIZE 244U
#define TEST_UDP_OFFSET (TEST_ETH_HEADER + TEST_IP_HEADER)
#define TEST_BOOTP_OFFSET (TEST_UDP_OFFSET + TEST_UDP_HEADER)

static void message(const char *text)
{
    register UINT32 r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = text;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
}

static _Noreturn void finish(UINT32 code)
{
    UINT32 block[] = {0x20026U, code};
    register UINT32 r0 __asm__("r0") = 0x20U;
    register UINT32 *r1 __asm__("r1") = block;
    __asm__ volatile("bkpt 0xAB" : "+r"(r0), "+r"(r1) : : "memory");
    for (;;) {}
}

static _Noreturn void fail(const char *text) { message(text); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static const UINT8 test_mac[6] = {6U, 0x2AU, 0xDFU, 0x95U, 0x2DU, 4U};
static const UINT8 test_ip[4] = {192U, 168U, 1U, 201U};
static const UINT8 test_mask[4] = {255U, 255U, 255U, 0U};
static const UINT8 pc_ip[4] = {192U, 168U, 1U, 200U};
static UINT64 clock_us;
static sn_parser_t peer_parser;
static sn_server_t peer_server;
static sn_frame_t peer_request, peer_response;
static UINT8 incoming[SN_WIRE_MAX], packet[SN_ETH_MAX], transmitted[SN_ETH_MAX];
static UINTN incoming_size, incoming_pos, packet_size, sends, polls, hellos, releases;
static UINTN wire_requests, release_wire_requests, disconnects;
static UINTN rejected_requests;
static UINT32 first_release_sequence, last_release_sequence;
static UINT8 discard_reply, silence, packet_ready, hello_short, hello_legacy;
static UINT32 hello_not_ready, release_not_ready;
static UINT32 uart_regs[64], iomux_reg, clock_reg, gate_reg, reset_reg, input_reg;
static BOOLEAN detach_fail, detach_incomplete;
static BOOLEAN block_uart_tx;
static UINT32 source_reg, fraction_reg, parent_hz, peer_baud;
static UINT64 peer_rate_until;
static BOOLEAN old_mcu, legacy_rate_mcu, bad_fraction, bad_rate_reply, drop_fast_reply;

VOID *EFIAPI CopyMem(VOID *dst, CONST VOID *src, UINTN size)
{
    for (UINTN i = 0; i < size; ++i) ((UINT8 *)dst)[i] = ((CONST UINT8 *)src)[i];
    return dst;
}
VOID *EFIAPI SetMem(VOID *dst, UINTN size, UINT8 value)
{
    for (UINTN i = 0; i < size; ++i) ((UINT8 *)dst)[i] = value;
    return dst;
}
VOID *EFIAPI ZeroMem(VOID *dst, UINTN size) { return SetMem(dst, size, 0); }
INTN EFIAPI CompareMem(CONST VOID *a, CONST VOID *b, UINTN size)
{
    for (UINTN i = 0; i < size; ++i) {
        int delta = ((CONST UINT8 *)a)[i] - ((CONST UINT8 *)b)[i];
        if (delta) return delta;
    }
    return 0;
}
UINT64 EFIAPI GetPerformanceCounter(VOID) { clock_us += 500U; return clock_us; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks) { return ticks * 1000U; }
UINTN EFIAPI MicroSecondDelay(UINTN us) { clock_us += us; return us; }
UINT32 HAL_CRU_ClkGetFreq(eCLOCK_Name name) { CHECK(name == PLL_GPLL); return parent_hz; }
EFI_STATUS EFIAPI EfiAcquireLockOrFail(EFI_LOCK *lock)
{
    if (lock->Lock == EfiLockAcquired) return EFI_ACCESS_DENIED;
    lock->Lock = EfiLockAcquired;
    return EFI_SUCCESS;
}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *lock) { CHECK(EfiAcquireLockOrFail(lock) == EFI_SUCCESS); }
VOID EFIAPI EfiReleaseLock(EFI_LOCK *lock)
{
    CHECK(lock->Lock == EfiLockAcquired);
    lock->Lock = EfiLockReleased;
}

static void peer_execute(void *unused, const sn_frame_t *request, sn_frame_t *reply)
{
    (VOID)unused;
    reply->status = SN_OK;
    if (request->op == SN_HELLO) {
        ++hellos;
        if (request->length == SN_RATE_REQUEST_SIZE) {
            UINT32 requested = (UINT32)request->data[0] | (UINT32)request->data[1] << 8 |
                               (UINT32)request->data[2] << 16 | (UINT32)request->data[3] << 24;
            if (old_mcu) { reply->status = SN_INVALID; return; }
            CHECK(peer_baud == SN_BOOT_BAUD);
            CHECK(requested == SN_DATA_BAUD || requested == SN_LEGACY_DATA_BAUD);
            if (legacy_rate_mcu && requested != SN_LEGACY_DATA_BAUD) {
                reply->status = SN_INVALID;
                return;
            }
            reply->length = request->length;
            CopyMem(reply->data, request->data, request->length);
            if (bad_rate_reply) reply->data[0] ^= 1U;
            return;
        }
        if (hello_not_ready) {
            --hello_not_ready;
            reply->status = SN_NOT_READY;
            return;
        }
        reply->length = hello_short ? 9U : SN_HELLO_INFO_SIZE;
        ZeroMem(reply->data, reply->length);
        CopyMem(reply->data, test_mac, sizeof(test_mac));
        reply->data[SN_HELLO_LINK_OFFSET] = 1U;
        reply->data[SN_HELLO_MTU_OFFSET] = (UINT8)SN_MTU;
        reply->data[SN_HELLO_MTU_OFFSET + 1U] = (UINT8)(SN_MTU >> 8);
        if (!hello_short) {
            CopyMem(reply->data + SN_HELLO_IP_OFFSET, test_ip, sizeof(test_ip));
            CopyMem(reply->data + SN_HELLO_MASK_OFFSET, test_mask, sizeof(test_mask));
        }
    } else if (request->op == SN_SEND) {
        ++sends;
        CopyMem(transmitted, request->data, request->length);
    } else if (request->op == SN_POLL) {
        ++polls;
        if (packet_ready) {
            reply->length = (UINT16)packet_size;
            CopyMem(reply->data, packet, packet_size);
            packet_ready = 0U;
        } else reply->status = SN_EMPTY;
    } else if (request->op == SN_RELEASE) {
        ++releases;
        if (!first_release_sequence) first_release_sequence = request->sequence;
        last_release_sequence = request->sequence;
        if (release_not_ready) {
            --release_not_ready;
            reply->status = SN_NOT_READY;
        }
    }
}

/* 生成 CRC 正确的 V1 应答，验证拒绝来自版本门禁而非坏 CRC。 */
static UINTN legacy_reply(const sn_frame_t *reply)
{
    UINT8 raw[SN_RAW_MAX];
    UINTN length = 14U + reply->length, out = 1U, marker = 0U;
    UINT8 code = 1U;
    raw[0] = 1U; raw[1] = reply->op; raw[2] = reply->status; raw[3] = 0U;
    for (UINTN i = 0; i < 4U; ++i) {
        raw[4U + i] = (UINT8)(reply->session >> (i * 8U));
        raw[8U + i] = (UINT8)(reply->sequence >> (i * 8U));
    }
    raw[12] = (UINT8)reply->length; raw[13] = (UINT8)(reply->length >> 8);
    CopyMem(raw + 14U, reply->data, reply->length);
    UINT32 crc = sn_crc32(raw, (UINT16)length);
    for (UINTN i = 0; i < 4U; ++i) raw[length + i] = (UINT8)(crc >> (i * 8U));
    length += 4U;
    for (UINTN i = 0; i < length; ++i) {
        if (!raw[i]) { incoming[marker] = code; marker = out++; code = 1U; }
        else {
            incoming[out++] = raw[i];
            if (++code == 0xFFU) { incoming[marker] = code; marker = out++; code = 1U; }
        }
    }
    incoming[marker] = code; incoming[out++] = 0U;
    return out;
}

UINT32 EFIAPI MmioRead32(UINTN address)
{
    if (address == UART_IOMUX) return iomux_reg;
    if (address == UART_CLKSEL) return clock_reg;
    if (address == UART_CLKGATE) return gate_reg;
    if (address == UART_RESET) return reset_reg;
    if (address == UART_INPUT) return input_reg;
    if (address == UART_SOURCE) return source_reg;
    if (address == UART_FRACTION) return fraction_reg;
    if (address == UART_BASE + UART_LSR_OFFSET) return block_uart_tx ? 0U : UART_TX_EMPTY;
    if (address == UART_BASE + UART_USR_OFFSET)
        return (block_uart_tx ? 0U : BIT1) | (incoming_pos < incoming_size ? BIT3 : 0U);
    CHECK(address >= UART_BASE && address < UART_BASE + sizeof(uart_regs));
    if (address == UART_BASE && (uart_regs[3] & BIT7) == 0U) {
        CHECK(incoming_pos < incoming_size);
        return incoming[incoming_pos++];
    }
    return uart_regs[(address - UART_BASE) / 4U];
}

UINT32 EFIAPI MmioWrite32(UINTN address, UINT32 value)
{
    UINT32 *control = address == UART_IOMUX ? &iomux_reg :
                      address == UART_SOURCE ? &source_reg :
                      address == UART_CLKSEL ? &clock_reg :
                      address == UART_CLKGATE ? &gate_reg :
                      address == UART_RESET ? &reset_reg :
                      address == UART_INPUT ? &input_reg : NULL;
    if (control) {
        UINT32 mask = value >> 16;
        *control = (*control & ~mask) | (value & mask);
        return value;
    }
    if (address == UART_FRACTION) { fraction_reg = bad_fraction ? 0U : value; return value; }
    CHECK(address >= UART_BASE && address < UART_BASE + sizeof(uart_regs));
    if (address != UART_BASE || (uart_regs[3] & BIT7) != 0U) {
        uart_regs[(address - UART_BASE) / 4U] = value;
        return value;
    }
    if (peer_rate_until && clock_us >= peer_rate_until) {
        peer_baud = SN_BOOT_BAUD; peer_rate_until = 0U;
        sn_parser_reset(&peer_parser);
    }
    UINT32 wire_baud = (clock_reg & UART_MUX_MASK) == UART_MUX_FRACTION ?
                      (UINT32)((UINT64)parent_hz * (fraction_reg >> 16) /
                      (fraction_reg & MAX_UINT16) / UART_OVERSAMPLE) : SN_BOOT_BAUD;
    if (wire_baud != peer_baud) return value;
    if (sn_feed(&peer_parser, (UINT8)value, &peer_request) == 1) {
        ++wire_requests;
        if (peer_request.op == SN_RELEASE) ++release_wire_requests;
        /* 与 MCU 相同：非法序号仍回 SN_INVALID，不能靠测试断言隐藏实际恢复失败。 */
        if (sn_server_handle(&peer_server, &peer_request, &peer_response, peer_execute, NULL) < 0)
            ++rejected_requests;
        if (peer_request.op == SN_HELLO && peer_response.status == SN_OK) {
            if (peer_request.length == SN_RATE_REQUEST_SIZE) {
                peer_baud = (UINT32)peer_request.data[0] | (UINT32)peer_request.data[1] << 8 |
                            (UINT32)peer_request.data[2] << 16 | (UINT32)peer_request.data[3] << 24;
                peer_rate_until = clock_us + SN_RATE_TRIAL_MS * 1000U;
            } else if (peer_baud != SN_BOOT_BAUD) {
                peer_rate_until = 0U;
                if (drop_fast_reply) { discard_reply = 1U; drop_fast_reply = FALSE; }
            }
        }
        if (discard_reply || silence) {
            discard_reply = 0U;
            incoming_size = incoming_pos = 0U;
        } else {
            incoming_size = hello_legacy && peer_request.op == SN_HELLO ?
                            legacy_reply(&peer_response) :
                            sn_pack(&peer_response, incoming, sizeof(incoming));
            incoming_pos = 0U;
        }
    }
    return value;
}

static EFI_STATUS EFIAPI disconnect_controller(EFI_HANDLE controller,
                                               EFI_HANDLE driver, EFI_HANDLE child)
{
    ++disconnects;
    CHECK(controller == mHandle && !driver && !child);
    CHECK(mLock.Lock != EfiLockAcquired);
    CHECK(mInfo.State == NET_HANDOFF_RETURNING);
    CHECK(EFI_ERROR(Start(&mSnp)) && EFI_ERROR(Reset(&mSnp, FALSE)));
    if (detach_fail) return EFI_ACCESS_DENIED;
    if (detach_incomplete) return EFI_SUCCESS;
    if (mMode.State == EfiSimpleNetworkInitialized) CHECK(Shutdown(&mSnp) == EFI_SUCCESS);
    if (mMode.State == EfiSimpleNetworkStarted) CHECK(Stop(&mSnp) == EFI_SUCCESS);
    return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES boot_services = {.DisconnectController = disconnect_controller};
EFI_BOOT_SERVICES *gBS = &boot_services;

static void reset_fixture(void)
{
    clock_us = 0U;
    ZeroMem(&peer_parser, sizeof(peer_parser)); ZeroMem(&peer_server, sizeof(peer_server));
    ZeroMem(&mMode, sizeof(mMode)); ZeroMem(&mInfo, sizeof(mInfo));
    ZeroMem(&mParser, sizeof(mParser)); ZeroMem(&mRequest, sizeof(mRequest));
    ZeroMem(&mResponse, sizeof(mResponse)); ZeroMem(uart_regs, sizeof(uart_regs));
    incoming_size = incoming_pos = packet_size = sends = polls = hellos = releases = 0U;
    wire_requests = release_wire_requests = disconnects = 0U;
    rejected_requests = 0U;
    first_release_sequence = last_release_sequence = 0U;
    discard_reply = silence = packet_ready = hello_short = hello_legacy = 0U;
    hello_not_ready = release_not_ready = 0U;
    detach_fail = detach_incomplete = FALSE;
    block_uart_tx = FALSE;
    source_reg = 0xFFFFU; fraction_reg = 0U;
    parent_hz = 1500000000U; peer_baud = SN_BOOT_BAUD; peer_rate_until = 0U;
    old_mcu = legacy_rate_mcu = bad_fraction = bad_rate_reply = drop_fast_reply = FALSE;
    mSession = 55U; mSequence = 0U; mFailed = FALSE; mLastPoll = 0U;
    mReleasePending = FALSE; mDetached = FALSE;
    mPacketSize = 0U; mRecycle = NULL; mLock.Lock = EfiLockReleased;
    mHandle = (EFI_HANDLE)(UINTN)0x1234U;
    CHECK(UartInit() == EFI_SUCCESS);
}

static void write_be16(UINT8 *where, UINT16 value)
{
    where[0] = (UINT8)(value >> 8); where[1] = (UINT8)value;
}

static void ethernet(UINT16 type)
{
    ZeroMem(packet, sizeof(packet));
    SetMem(packet, 6U, 0xFFU); CopyMem(packet + 6U, test_mac, sizeof(test_mac));
    write_be16(packet + 12U, type);
}

static void ipv4_packet(const UINT8 source[4], UINT8 protocol, UINT16 payload)
{
    ethernet(0x0800U);
    packet_size = TEST_ETH_HEADER + TEST_IP_HEADER + payload;
    packet[14] = 0x45U;
    write_be16(packet + 16U, (UINT16)(TEST_IP_HEADER + payload));
    packet[22] = 64U; packet[23] = protocol;
    CopyMem(packet + 26U, source, 4U); CopyMem(packet + 30U, pc_ip, 4U);
}

static void dhcp_packet(BOOLEAN reply, const UINT8 offered[4])
{
    static const UINT8 zero[4] = {0U, 0U, 0U, 0U};
    static const UINT8 cookie[4] = {99U, 130U, 83U, 99U};
    ipv4_packet(reply ? pc_ip : zero, 17U, TEST_UDP_HEADER + TEST_BOOTP_SIZE);
    write_be16(packet + TEST_UDP_OFFSET, reply ? 67U : 68U);
    write_be16(packet + TEST_UDP_OFFSET + 2U, reply ? 68U : 67U);
    write_be16(packet + TEST_UDP_OFFSET + 4U, TEST_UDP_HEADER + TEST_BOOTP_SIZE);
    packet[TEST_BOOTP_OFFSET] = reply ? 2U : 1U;
    packet[TEST_BOOTP_OFFSET + 1U] = 1U; packet[TEST_BOOTP_OFFSET + 2U] = 6U;
    CopyMem(packet + TEST_BOOTP_OFFSET + 28U, test_mac, sizeof(test_mac));
    CopyMem(packet + TEST_BOOTP_OFFSET + 236U, cookie, sizeof(cookie));
    if (offered) CopyMem(packet + TEST_BOOTP_OFFSET + 16U, offered, 4U);
    packet[TEST_BOOTP_OFFSET + 240U] = 53U;
    packet[TEST_BOOTP_OFFSET + 241U] = 1U;
    packet[TEST_BOOTP_OFFSET + 242U] = reply ? 5U : 1U;
    packet[TEST_BOOTP_OFFSET + 243U] = 255U;
}

static void arp_packet(BOOLEAN probe)
{
    ethernet(0x0806U); packet_size = 42U;
    write_be16(packet + 14U, 1U); write_be16(packet + 16U, 0x0800U);
    packet[18] = 6U; packet[19] = 4U; write_be16(packet + 20U, 1U);
    CopyMem(packet + 22U, test_mac, sizeof(test_mac));
    if (!probe) CopyMem(packet + 28U, test_ip, sizeof(test_ip));
    CopyMem(packet + 38U, test_ip, sizeof(test_ip));
}

static void send_packet(BOOLEAN allowed)
{
    UINTN before = sends;
    VOID *recycled = NULL;
    EFI_STATUS status = Transmit(&mSnp, 0U, packet_size, packet, NULL, NULL, NULL);
    CHECK(EFI_ERROR(status) != allowed);
    CHECK(sends == before + (allowed ? 1U : 0U));
    if (allowed) {
        CHECK(CompareMem(transmitted, packet, packet_size) == 0);
        CHECK(GetStatus(&mSnp, NULL, &recycled) == EFI_SUCCESS && recycled == packet);
    }
}

static void receive_packet(BOOLEAN allowed)
{
    UINT8 buffer[SN_ETH_MAX];
    UINTN size = sizeof(buffer);
    packet_ready = 1U; clock_us += 10000U;
    EFI_STATUS status = Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL);
    CHECK(EFI_ERROR(status) != allowed);
    if (allowed) CHECK(size == packet_size && CompareMem(buffer, packet, size) == 0);
}

static void check_grant(void)
{
    reset_fixture(); hello_short = 1U;
    CHECK(EFI_ERROR(AwaitGrant()));
    CHECK(mInfo.State != NET_HANDOFF_ACTIVE);
    reset_fixture(); hello_legacy = 1U;
    CHECK(EFI_ERROR(AwaitGrant()));
    CHECK(mInfo.State != NET_HANDOFF_ACTIVE && clock_us < 75000000U);
    reset_fixture(); hello_not_ready = MAX_UINT32;
    CHECK(EFI_ERROR(AwaitGrant()));
    CHECK(hellos > 1U && mInfo.State != NET_HANDOFF_ACTIVE && clock_us < 75000000U);
    reset_fixture(); hello_not_ready = 3U;
    CHECK(AwaitGrant() == EFI_SUCCESS);
    CHECK(hellos == 4U && wire_requests == 4U && !mFailed);
    CHECK(mInfo.State == NET_HANDOFF_ACTIVE && mInfo.Session == mSession);
    CHECK(CompareMem(mInfo.Ip, test_ip, sizeof(test_ip)) == 0);
    CHECK(CompareMem(mInfo.Mask, test_mask, sizeof(test_mask)) == 0);
    CopyMem(&mMode.CurrentAddress, test_mac, sizeof(test_mac));
    CopyMem(&mMode.PermanentAddress, test_mac, sizeof(test_mac));
    mMode.ReceiveFilterSetting = FILTERS;
    CHECK(Start(&mSnp) == EFI_SUCCESS && Initialize(&mSnp, 0U, 0U) == EFI_SUCCESS);
}

static void check_address_guard(void)
{
    ipv4_packet(test_ip, 6U, 20U); discard_reply = 1U; send_packet(TRUE);
    CHECK(sends == 1U);
    packet[29] ^= 1U; send_packet(FALSE);
    ipv4_packet(pc_ip, 6U, 20U); receive_packet(TRUE);
    arp_packet(FALSE); send_packet(TRUE);
    packet[31] ^= 1U; send_packet(FALSE);
    arp_packet(TRUE); send_packet(TRUE);
    packet[41] ^= 1U; send_packet(FALSE);
    dhcp_packet(FALSE, NULL); send_packet(TRUE);
    packet[TEST_UDP_OFFSET + 3U] = 69U; send_packet(FALSE);
    dhcp_packet(TRUE, test_ip); receive_packet(TRUE);
    packet[TEST_BOOTP_OFFSET + 19U] ^= 1U; receive_packet(FALSE);
    dhcp_packet(TRUE, NULL); packet[TEST_BOOTP_OFFSET + 242U] = 2U; receive_packet(TRUE);
    ipv4_packet(test_ip, 6U, 20U); packet[20] = 0x20U; send_packet(FALSE);
    ethernet(0x86DDU); packet_size = 54U; send_packet(FALSE);
}

static void check_full_mtu_receive(void)
{
    UINT8 buffer[SN_ETH_MAX];
    UINTN size = 1U, before = polls;
    ipv4_packet(pc_ip, 17U, SN_MTU - TEST_IP_HEADER);
    CopyMem(packet, test_mac, sizeof(test_mac));
    CopyMem(packet + 30U, test_ip, sizeof(test_ip));
    write_be16(packet + TEST_UDP_OFFSET, 50000U);
    write_be16(packet + TEST_UDP_OFFSET + 2U, 1618U);
    write_be16(packet + TEST_UDP_OFFSET + 4U, SN_MTU - TEST_IP_HEADER);
    write_be16(packet + TEST_UDP_OFFSET + TEST_UDP_HEADER, 3U);
    write_be16(packet + TEST_UDP_OFFSET + TEST_UDP_HEADER + 2U, 1U);
    /* 1468字节TFTP载荷对应1514字节以太帧；应答丢失后重放同一包，不能重复出队。 */
    SetMem(packet + TEST_UDP_OFFSET + TEST_UDP_HEADER + 4U,
           SN_MTU - TEST_IP_HEADER - TEST_UDP_HEADER - 4U, 0x5AU);
    packet_ready = discard_reply = 1U;
    clock_us += RECEIVE_POLL_US;
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_BUFFER_TOO_SMALL);
    CHECK(size == SN_ETH_MAX && polls == before + 1U);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_SUCCESS);
    CHECK(size == SN_ETH_MAX && CompareMem(buffer, packet, size) == 0 && polls == before + 1U);
    message("PASS: full-MTU TFTP DATA, replay and receive buffer retry\n");
}

static void check_baud(void)
{
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS);
    UINT32 source_other = source_reg & ~UART_SOURCE_MASK;
    UINT32 clock_other = clock_reg & ~UART_MUX_MASK;
    CHECK(NegotiateBaud() == EFI_SUCCESS && mBaud == SN_DATA_BAUD && peer_baud == SN_DATA_BAUD);
    CHECK(fraction_reg == ((16U << 16) | 375U) && uart_regs[0] == 1U);
    CHECK((source_reg & ~UART_SOURCE_MASK) == source_other && (clock_reg & ~UART_MUX_MASK) == clock_other);
    CHECK(!peer_rate_until && !mFailed);

    /* 只支持2M的旧MCU明确拒绝4M后，使用新序号协商2M；轮询仍走快速间隔。 */
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); legacy_rate_mcu = TRUE;
    CHECK(NegotiateBaud() == EFI_SUCCESS && mBaud == SN_LEGACY_DATA_BAUD);
    CHECK(peer_baud == SN_LEGACY_DATA_BAUD && wire_requests == 4U && !peer_rate_until);
    CHECK(fraction_reg == ((8U << 16) | 375U));
    mLastPoll = clock_us;
    clock_us += RECEIVE_FAST_POLL_US;
    CHECK(Poll() == EFI_NOT_READY && polls == 1U);

    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); old_mcu = TRUE;
    CHECK(NegotiateBaud() == EFI_SUCCESS && mBaud == SN_BOOT_BAUD);
    CHECK(Rpc(SN_RESET, NULL, 0U) == EFI_SUCCESS && rejected_requests == 0U);

    /* 低速ACK丢失后MCU回低速，同序号重试重新切换；高速ACK丢失仍留高速重试。 */
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); discard_reply = 1U;
    CHECK(NegotiateBaud() == EFI_SUCCESS && wire_requests == 4U && rejected_requests == 0U);
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); drop_fast_reply = TRUE;
    CHECK(NegotiateBaud() == EFI_SUCCESS && wire_requests == 4U && !peer_rate_until);

    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); parent_hz = 0U;
    CHECK(NegotiateBaud() == EFI_SUCCESS && mBaud == SN_BOOT_BAUD && wire_requests == 1U);
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); bad_fraction = TRUE;
    CHECK(EFI_ERROR(NegotiateBaud()) && wire_requests == 2U);
    reset_fixture(); CHECK(AwaitGrant() == EFI_SUCCESS); bad_rate_reply = TRUE;
    CHECK(NegotiateBaud() == EFI_PROTOCOL_ERROR && mBaud == SN_BOOT_BAUD);
    reset_fixture(); block_uart_tx = TRUE;
    CHECK(UartDataBaud(SN_DATA_BAUD, DataClockFraction(SN_DATA_BAUD)) == EFI_TIMEOUT && mBaud == SN_BOOT_BAUD);
    message("PASS: UART 4M/2M negotiation, old MCU, lost ACK, clock readback and TX drain\n");
}

static void check_return(void)
{
    NET_HANDOFF_INFO info;
    CHECK(GetHandoffInfo(&mControl, &info) == EFI_SUCCESS);
    CHECK(info.Session == mSession && info.State == NET_HANDOFF_ACTIVE);
    detach_fail = TRUE;
    CHECK(EFI_ERROR(ReturnNetwork(&mControl)));
    CHECK(mInfo.State == NET_HANDOFF_RETURNING && releases == 0U);
    ipv4_packet(test_ip, 6U, 20U); send_packet(FALSE);
    CHECK(EFI_ERROR(Start(&mSnp)) && EFI_ERROR(Initialize(&mSnp, 0U, 0U)));
    detach_fail = FALSE; detach_incomplete = TRUE;
    CHECK(EFI_ERROR(ReturnNetwork(&mControl)) && releases == 0U);
    detach_incomplete = FALSE; release_not_ready = 1U;
    CHECK(ReturnNetwork(&mControl) == EFI_NOT_READY);
    CHECK(releases == 1U && mInfo.State == NET_HANDOFF_RETURNING);
    CHECK(mMode.State == EfiSimpleNetworkStopped && !mPacketSize && !mRecycle);
    discard_reply = 1U;
    CHECK(ReturnNetwork(&mControl) == EFI_SUCCESS);
    CHECK(releases == 2U && release_wire_requests == 3U);
    CHECK(last_release_sequence == first_release_sequence + 1U);
    CHECK(mInfo.State == NET_HANDOFF_RELEASED);
    UINTN before = wire_requests;
    CHECK(ReturnNetwork(&mControl) == EFI_SUCCESS && wire_requests == before);
    CHECK(EFI_ERROR(Start(&mSnp)) && EFI_ERROR(Initialize(&mSnp, 0U, 0U)));
    CHECK(EFI_ERROR(Reset(&mSnp, FALSE)));
    send_packet(FALSE); receive_packet(FALSE);
    CHECK(wire_requests == before);
}

/* 所有 RELEASE 应答丢失后，下一次命令仍须重发原事务，不能跳过未知结果。 */
static void check_return_timeout(void)
{
    reset_fixture();
    CHECK(AwaitGrant() == EFI_SUCCESS);
    CopyMem(&mMode.CurrentAddress, test_mac, sizeof(test_mac));
    mMode.ReceiveFilterSetting = FILTERS;
    CHECK(Start(&mSnp) == EFI_SUCCESS && Initialize(&mSnp, 0U, 0U) == EFI_SUCCESS);
    silence = 1U;
    CHECK(ReturnNetwork(&mControl) == EFI_TIMEOUT);
    CHECK(releases == 1U && release_wire_requests == 3U);
    CHECK(mInfo.State == NET_HANDOFF_RETURNING && mReleasePending && mFailed);
    CHECK(mMode.State == EfiSimpleNetworkStopped && mDetached);
    UINT32 sequence = mSequence;
    UINTN before = wire_requests;
    CHECK(EFI_ERROR(Start(&mSnp)) && EFI_ERROR(Reset(&mSnp, FALSE)));
    CHECK(wire_requests == before);
    silence = 0U;
    CHECK(ReturnNetwork(&mControl) == EFI_SUCCESS);
    CHECK(mSequence == sequence && releases == 1U && release_wire_requests == 4U);
    CHECK(mInfo.State == NET_HANDOFF_RELEASED && !mReleasePending);
    CHECK(disconnects == 1U);
}

/* 数据请求根本未到 MCU 时，显式归还仍须成功；不得重发可能产生业务副作用的旧 SEND。 */
static void check_return_after_unsent_request(void)
{
    reset_fixture();
    CHECK(AwaitGrant() == EFI_SUCCESS);
    CopyMem(&mMode.CurrentAddress, test_mac, sizeof(test_mac));
    mMode.ReceiveFilterSetting = FILTERS;
    CHECK(Start(&mSnp) == EFI_SUCCESS && Initialize(&mSnp, 0U, 0U) == EFI_SUCCESS);
    ipv4_packet(test_ip, 6U, 20U);
    UINTN before = wire_requests;
    block_uart_tx = TRUE;
    CHECK(Transmit(&mSnp, 0U, packet_size, packet, NULL, NULL, NULL) == EFI_TIMEOUT);
    CHECK(wire_requests == before && sends == 0U && mFailed);
    block_uart_tx = FALSE;
    CHECK(ReturnNetwork(&mControl) == EFI_SUCCESS);
    CHECK(sends == 0U && releases == 1U && rejected_requests == 0U);
    CHECK(mInfo.State == NET_HANDOFF_RELEASED && disconnects == 1U);
}

void test_main(void)
{
    CHECK(SN_VERSION == 2U && SN_RELEASE == 6U && SN_HELLO_INFO_SIZE == 21U);
    check_grant();
    check_address_guard();
    check_full_mtu_receive();
    check_return();
    check_return_timeout();
    check_return_after_unsent_request();
    check_baud();
    message("PASS: same-IP SNP handoff grant, address guard, teardown and release\n");
    finish(0U);
}
