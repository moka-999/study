/* 使用真实EDK2头文件和SNP源码；模拟UART对端，检查丢应答后的网络副作用。 */
#include "../edk2/SerialNetSnpDxe.c"

#define S0(x) #x
#define S(x) S0(x)
#define CHECK(x) do { if (!(x)) fail("FAIL " S(__LINE__) " " #x "\n"); } while (0)
static void message(const char *s)
{
    register UINT32 r0 __asm__("r0") = 4U;
    register const char *r1 __asm__("r1") = s;
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
static _Noreturn void fail(const char *s) { message(s); finish(1U); }
void test_abort(void) { fail("FAIL CPU exception\n"); }

static UINT64 clock_us;
static sn_parser_t peer_parser;
static sn_server_t peer_server;
static sn_frame_t peer_request, peer_response;
static UINT8 incoming[SN_WIRE_MAX], packet[SN_ETH_MAX], transmitted[SN_ETH_MAX];
static UINTN incoming_size, incoming_pos, sends, polls;
static UINT8 discard_reply, silence, packet_ready;
static UINT32 uart_regs[64], iomux_reg, clock_reg, gate_reg, reset_reg, input_reg;
static UINT8 reject_divisor, reject_mux, block_uart_tx;

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
        int d = ((CONST UINT8 *)a)[i] - ((CONST UINT8 *)b)[i];
        if (d) return d;
    }
    return 0;
}
UINT64 EFIAPI GetPerformanceCounter(VOID) { clock_us += 50U; return clock_us; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 ticks) { return ticks * 1000U; }
UINTN EFIAPI MicroSecondDelay(UINTN us) { clock_us += us; return us; }
EFI_STATUS EFIAPI EfiAcquireLockOrFail(EFI_LOCK *lock)
{
    if (lock->Lock == EfiLockAcquired) return EFI_ACCESS_DENIED;
    lock->Lock = EfiLockAcquired;
    return EFI_SUCCESS;
}
VOID EFIAPI EfiAcquireLock(EFI_LOCK *lock) { CHECK(EfiAcquireLockOrFail(lock) == EFI_SUCCESS); }
VOID EFIAPI EfiReleaseLock(EFI_LOCK *lock) { CHECK(lock->Lock == EfiLockAcquired); lock->Lock = EfiLockReleased; }

static void peer_execute(void *unused, const sn_frame_t *q, sn_frame_t *r)
{
    (VOID)unused;
    r->status = SN_OK;
    if (q->op == SN_SEND) {
        ++sends; CopyMem(transmitted, q->data, q->length);
    } else if (q->op == SN_POLL) {
        ++polls;
        if (packet_ready) {
            r->length = sizeof(packet); CopyMem(r->data, packet, sizeof(packet));
            packet_ready = 0;
        } else r->status = SN_EMPTY;
    }
}
UINT32 EFIAPI MmioRead32(UINTN address)
{
    if (address == UART_IOMUX) return iomux_reg;
    if (address == UART_CLKSEL) return clock_reg;
    if (address == UART_CLKGATE) return gate_reg;
    if (address == UART_RESET) return reset_reg;
    if (address == UART_INPUT) return input_reg;
    if (address == UART_BASE + UART_USR)
        return (block_uart_tx ? 0 : BIT1) | (incoming_pos < incoming_size ? BIT3 : 0);
    CHECK(address >= UART_BASE && address < UART_BASE + sizeof(uart_regs));
    if (address == UART_BASE && (uart_regs[3] & BIT7) == 0) {
        CHECK(incoming_pos < incoming_size);
        return incoming[incoming_pos++];
    }
    return uart_regs[(address - UART_BASE) / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN address, UINT32 value)
{
    UINT32 *control = address == UART_IOMUX ? &iomux_reg :
                      address == UART_CLKSEL ? &clock_reg :
                      address == UART_CLKGATE ? &gate_reg :
                      address == UART_RESET ? &reset_reg :
                      address == UART_INPUT ? &input_reg : NULL;
    if (control) {
        if (address == UART_IOMUX && reject_mux) return value;
        /* 只模拟写掩码与回读，不代表真实RK时钟或引脚已经验证。 */
        UINT32 mask = value >> 16;
        *control = (*control & ~mask) | (value & mask);
        return value;
    }
    CHECK(address >= UART_BASE && address < UART_BASE + sizeof(uart_regs));
    if (address != UART_BASE || (uart_regs[3] & BIT7) != 0) {
        if (!reject_divisor || address != UART_BASE)
            uart_regs[(address - UART_BASE) / 4] = value;
        return value;
    }
    if (sn_feed(&peer_parser, (UINT8)value, &peer_request) == 1) {
        CHECK(sn_server_handle(&peer_server, &peer_request, &peer_response, peer_execute, NULL) >= 0);
        if (discard_reply || silence) { discard_reply = 0; incoming_size = incoming_pos = 0; }
        else {
            incoming_size = sn_pack(&peer_response, incoming, sizeof(incoming)); incoming_pos = 0;
        }
    }
    return value;
}

/* 重放MNP初始化与224.0.0.1入组时的参数，检查真正阻塞网络的接口。 */
static void check_network_filters(void)
{
    UINT8 buffer[SN_ETH_MAX];
    UINTN size;
    EFI_IP_ADDRESS ip = {.v4 = {{224, 0, 0, 1}}};
    EFI_MAC_ADDRESS multicast, saved;
    CHECK(FILTERS == 0x15U);
    CHECK(ReceiveFilters(&mSnp, 5U, 0x10U, TRUE, 0, NULL) == EFI_SUCCESS);
    CHECK(mMode.ReceiveFilterSetting == 5U);
    /* 旧能力只有5时，MNP会退到全混杂(0x08)；不能谎报支持。 */
    CHECK(ReceiveFilters(&mSnp, 0x0DU, 8U, FALSE, 0, NULL) == EFI_INVALID_PARAMETER);
    CHECK(mMode.ReceiveFilterSetting == 5U);
    CHECK(ReceiveFilters(&mSnp, 0x15U, 0, FALSE, 0, NULL) == EFI_SUCCESS);
    CHECK(mMode.ReceiveFilterSetting == 0x15U);
    CHECK(ReceiveFilters(&mSnp, 0, 8U, TRUE, 0, NULL) == EFI_INVALID_PARAMETER);
    CHECK(ReceiveFilters(&mSnp, 0, 0, FALSE, 1, NULL) == EFI_INVALID_PARAMETER);
    CHECK(mMode.ReceiveFilterSetting == 0x15U);
    CHECK(ReceiveFilters(&mSnp, 0, 0, TRUE, 100, NULL) == EFI_SUCCESS);
    CHECK(Mcast(&mSnp, FALSE, &ip, &multicast) == EFI_SUCCESS);
    CHECK(multicast.Addr[0] == 1 && multicast.Addr[1] == 0 && multicast.Addr[2] == 0x5E);
    CHECK(multicast.Addr[3] == 0 && multicast.Addr[4] == 0 && multicast.Addr[5] == 1);
    saved = multicast; ip.v4.Addr[0] = 192;
    CHECK(Mcast(&mSnp, FALSE, &ip, &multicast) == EFI_INVALID_PARAMETER);
    CHECK(CompareMem(&saved, &multicast, sizeof(saved)) == 0);
    CHECK(Mcast(&mSnp, TRUE, &ip, &multicast) == EFI_UNSUPPORTED);

    CopyMem(packet, &multicast, 6);
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_SUCCESS);
    CHECK(CompareMem(buffer, &multicast, 6) == 0);
    /* 同位启停以Disable为准，关闭组播不会关闭广播。 */
    CHECK(ReceiveFilters(&mSnp, 0x10U, 0x10U, FALSE, 0, NULL) == EFI_SUCCESS);
    CHECK(mMode.ReceiveFilterSetting == 5U);
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_NOT_READY);
    SetMem(packet, 6, 0xFF);
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_SUCCESS);
    CopyMem(packet, &mMode.CurrentAddress, 6);
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_SUCCESS);
    CHECK(ReceiveFilters(&mSnp, 0, 1U, FALSE, 0, NULL) == EFI_SUCCESS);
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_NOT_READY);
    CHECK(ReceiveFilters(&mSnp, FILTERS, 0, FALSE, 0, NULL) == EFI_SUCCESS);
    packet[5] ^= 2U;
    packet_ready = 1; clock_us += 10000U; size = sizeof(buffer);
    CHECK(Receive(&mSnp, NULL, &size, buffer, NULL, NULL, NULL) == EFI_NOT_READY);
    SetMem(packet, 6, 0xFF);
}

void test_main(void)
{
    UINTN size, header;
    UINT32 interrupt;
    UINT16 protocol = 0x0800;
    VOID *recycle;
    EFI_MAC_ADDRESS dst = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}}, src;
    UINT8 receive_buffer[SN_ETH_MAX];
    /* 以原理图UART1_M0固定映射约束驱动，防止测试随错误端口一起变更。 */
    CHECK(UART_BASE == 0xFEB40000U && UART_IOMUX == 0xFD5F804CU);
    CHECK(UART_CLKSEL == 0xFD7C03ACU && UART_CLKGATE == 0xFD7C0830U);
    CHECK(UART_RESET == 0xFD7C0A30U && UART_INPUT == 0xFD5FA194U);
    iomux_reg = 0x3333U; clock_reg = 0xA5F1U; gate_reg = 0xFFFFU;
    reset_reg = 0x0100U; input_reg = 0xA59FU;
    reject_mux = 1;
    CHECK(UartInit() == EFI_DEVICE_ERROR && peer_parser.used == 0);
    reject_mux = 0;
    /* 分频寄存器写入未生效时停止握手，不能仍声称已配置115200。 */
    reject_divisor = 1;
    uart_regs[0] = 0;
    CHECK(UartInit() == EFI_DEVICE_ERROR && peer_parser.used == 0);
    reject_divisor = 0;
    CHECK(UartInit() == EFI_SUCCESS && peer_parser.used == 0);
    /* 复用、时钟及输入开关不能覆盖其它引脚或外设的配置。 */
    CHECK(iomux_reg == 0xAA33U && clock_reg == 0xA5F2U && gate_reg == 0xDFFBU);
    CHECK(reset_reg == 0x0100U && input_reg == 0xA5DFU);
    mSession = 55; mMode.State = EfiSimpleNetworkStopped;
    mMode.CurrentAddress.Addr[0] = 6;
    mMode.CurrentAddress.Addr[5] = 1;
    mMode.ReceiveFilterSetting = FILTERS;
    CHECK(Transmit(&mSnp, 0, sizeof(packet), packet, NULL, NULL, NULL) == EFI_NOT_STARTED);
    EfiAcquireLock(&mLock); CHECK(Rpc(SN_HELLO, NULL, 0) == EFI_SUCCESS); EfiReleaseLock(&mLock);
    CHECK(Start(&mSnp) == EFI_SUCCESS);
    CHECK(Initialize(&mSnp, 0, 0) == EFI_SUCCESS);
    CHECK(Start(&mSnp) == EFI_ALREADY_STARTED);
    for (UINTN i = 14; i < sizeof(packet); ++i) packet[i] = (UINT8)(i * 3U);

    /* 请求已执行但应答丢失：UART重试不得重复向网线发包。 */
    discard_reply = 1;
    CHECK(Transmit(&mSnp, 14, sizeof(packet), packet, NULL, &dst, &protocol) == EFI_SUCCESS);
    CHECK(sends == 1 && CompareMem(transmitted, packet, sizeof(packet)) == 0);
    CHECK(CompareMem(packet + 6, &mMode.CurrentAddress, 6) == 0 && packet[12] == 8 && packet[13] == 0);
    CHECK(Transmit(&mSnp, 0, sizeof(packet), packet, NULL, NULL, NULL) == EFI_NOT_READY);
    CHECK(GetStatus(&mSnp, NULL, &recycle) == EFI_SUCCESS && recycle == packet);
    CHECK(GetStatus(&mSnp, NULL, &recycle) == EFI_SUCCESS && recycle == NULL);

    /* 首次领取的应答丢失仍返回同一帧；小缓冲重试不能吞掉该帧。 */
    packet_ready = 1; discard_reply = 1; clock_us += 10000U;
    size = 8;
    CHECK(Receive(&mSnp, NULL, &size, receive_buffer, NULL, NULL, NULL) == EFI_BUFFER_TOO_SMALL);
    CHECK(size == sizeof(packet) && polls == 1 && packet_ready == 0);
    CHECK(Receive(&mSnp, &header, &size, receive_buffer, &src, &dst, &protocol) == EFI_SUCCESS);
    CHECK(polls == 1 && header == 14 && protocol == 0x0800);
    CHECK(CompareMem(receive_buffer, packet, size) == 0);
    CHECK(CompareMem(&src, &mMode.CurrentAddress, 6) == 0);
    clock_us += 10000U;
    CHECK(GetStatus(&mSnp, &interrupt, NULL) == EFI_SUCCESS && interrupt == 0);
    CHECK(mMode.MediaPresent == TRUE);

    check_network_filters();
    CHECK(ReceiveFilters(&mSnp, 0, EFI_SIMPLE_NETWORK_RECEIVE_BROADCAST, FALSE, 0, NULL) == EFI_SUCCESS);
    packet_ready = 1; clock_us += 10000U; size = sizeof(receive_buffer);
    CHECK(Receive(&mSnp, NULL, &size, receive_buffer, NULL, NULL, NULL) == EFI_NOT_READY);
    CHECK(Shutdown(&mSnp) == EFI_SUCCESS && Stop(&mSnp) == EFI_SUCCESS);
    CHECK(Start(&mSnp) == EFI_SUCCESS && Initialize(&mSnp, 0, 0) == EFI_SUCCESS);
    EfiAcquireLock(&mLock);
    CHECK(Transmit(&mSnp, 0, sizeof(packet), packet, NULL, NULL, NULL) == EFI_NOT_READY);
    EfiReleaseLock(&mLock);

    /* 连续三次无应答后停止新事务，防止跳过执行结果未知的序号。 */
    silence = 1;
    CHECK(Transmit(&mSnp, 0, sizeof(packet), packet, NULL, NULL, NULL) == EFI_TIMEOUT);
    CHECK(sends == 2 && mFailed && !mMode.MediaPresent);
    CHECK(Transmit(&mSnp, 0, sizeof(packet), packet, NULL, NULL, NULL) == EFI_DEVICE_ERROR);
    CHECK(sends == 2);
    /* FIFO不可写时仍有界退出，不将未投递的请求交给对端。 */
    mFailed = FALSE; block_uart_tx = 1;
    CHECK(Rpc(SN_HELLO, NULL, 0) == EFI_TIMEOUT && mFailed && sends == 2);
    message("PASS: serial-net SNP TX/RX, lost replies, buffer retention, filters and timeout\n");
    finish(0);
}
