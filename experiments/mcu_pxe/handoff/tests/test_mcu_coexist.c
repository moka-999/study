/* 复用真实交接模块和外设替身，专门验证PC持续连接时的串口网卡生命周期。 */
#define test_main legacy_handoff_test_main
#include "test_mcu_handoff.c"
#undef test_main

static ip4_addr_t kept_ip, kept_mask, kept_gateway;

static void check_pc_network(void)
{
    CHECK(B1_SERIAL_NET_KEEP_TCP == 1);
    CHECK(handoff_mcu_listen_allowed());
    CHECK(s_netif->ip_addr.addr == kept_ip.addr);
    CHECK(s_netif->netmask.addr == kept_mask.addr);
    CHECK(s_netif->gw.addr == kept_gateway.addr);
    CHECK(s_netif->hwaddr[0] == 2U);
}

/* 单客户端策略接受PC后关闭监听；同一连接须独立完成授权、RELEASE和OFF收尾。 */
static void poll_connected(void)
{
    handoff_mcu_tcp_poll(1, 0);
    check_pc_network();
}

static void grant_connected(uint32_t session)
{
    CHECK(power_state == PWR_CTRL_STATE_OFF && !handoff_mcu_busy());
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    poll_connected();
    CHECK(s_state.phase == HANDOFF_ARMED);
    CHECK(handoff_mcu_rk_poll() == 1 && s_boot_phase == HANDOFF_BOOT_PROBE);
    power_state = PWR_CTRL_STATE_ON;
    poll_connected();
    sn_frame_t hello = {.op = SN_HELLO, .session = session, .sequence = 1U};
    receive_serial(&hello);
    CHECK(s_boot_phase == HANDOFF_BOOT_EDK && s_state.session == session);
    CHECK(s_response.status == SN_NOT_READY && !s_active);
    poll_connected();
    CHECK(s_state.phase == HANDOFF_WITHDRAWING && pending_callback);
    run_callback();
    check_pc_network();
    poll_connected();
    CHECK(s_state.phase == HANDOFF_OWNED && !s_active);
    CHECK(transact(SN_HELLO, session) == SN_OK && s_active);
    check_pc_network();
}

enum {
    TEST_ETH_HEADER = 14U,
    TEST_IP_HEADER = 20U,
    TEST_TCP_HEADER = 20U,
    TEST_UDP_HEADER = 8U,
    TEST_ARP_LENGTH = 42U,
    TEST_ETH_TYPE = 12U,
    TEST_IP_SOURCE = TEST_ETH_HEADER + 12U,
    TEST_IP_DESTINATION = TEST_ETH_HEADER + 16U,
    TEST_TRANSPORT = TEST_ETH_HEADER + TEST_IP_HEADER,
    TEST_ARP_SENDER_MAC = TEST_ETH_HEADER + 8U,
    TEST_ARP_SENDER_IP = TEST_ETH_HEADER + 14U,
    TEST_ARP_TARGET_MAC = TEST_ETH_HEADER + 18U,
    TEST_ARP_TARGET_IP = TEST_ETH_HEADER + 24U,
    TEST_IP_TCP = 6U,
    TEST_IP_UDP = 17U,
    TEST_PEER_PORT = 41000U,
    TEST_TFTP_DYNAMIC_PORT = 52001U
};
static const uint8_t peer_mac[] = {2U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU};
static const uint8_t peer_ip[] = {192U, 168U, 1U, 200U};

static void put_be16(uint8_t *data, unsigned value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

/* 只构造分流所需头部；IP/TCP校验和属于协议栈，此测试不替代以太网联调。 */
static uint16_t make_ipv4(uint8_t *frame, const uint8_t destination[6],
                          uint8_t protocol, uint16_t destination_port)
{
    uint16_t size = TEST_ETH_HEADER + TEST_IP_HEADER +
        (protocol == TEST_IP_TCP ? TEST_TCP_HEADER : TEST_UDP_HEADER);
    memset(frame, 0, size);
    memcpy(frame, destination, 6U);
    memcpy(frame + 6U, peer_mac, 6U);
    put_be16(frame + TEST_ETH_TYPE, 0x0800U);
    frame[TEST_ETH_HEADER] = 0x45U;
    put_be16(frame + TEST_ETH_HEADER + 2U, size - TEST_ETH_HEADER);
    frame[TEST_ETH_HEADER + 8U] = 64U;
    frame[TEST_ETH_HEADER + 9U] = protocol;
    memcpy(frame + TEST_IP_SOURCE, peer_ip, 4U);
    memcpy(frame + TEST_IP_DESTINATION, &kept_ip.addr, 4U);
    put_be16(frame + TEST_TRANSPORT, TEST_PEER_PORT);
    put_be16(frame + TEST_TRANSPORT + 2U, destination_port);
    if (protocol == TEST_IP_TCP) frame[TEST_TRANSPORT + 12U] = 0x50U;
    else put_be16(frame + TEST_TRANSPORT + 4U, TEST_UDP_HEADER);
    return size;
}

static void make_arp(uint8_t *frame, uint16_t operation)
{
    memset(frame, 0, TEST_ARP_LENGTH);
    memcpy(frame, s_netif->hwaddr, 6U);
    memcpy(frame + 6U, peer_mac, 6U);
    put_be16(frame + TEST_ETH_TYPE, 0x0806U);
    put_be16(frame + TEST_ETH_HEADER, 1U);
    put_be16(frame + TEST_ETH_HEADER + 2U, 0x0800U);
    frame[TEST_ETH_HEADER + 4U] = 6U;
    frame[TEST_ETH_HEADER + 5U] = 4U;
    put_be16(frame + TEST_ETH_HEADER + 6U, operation);
    memcpy(frame + TEST_ARP_SENDER_MAC, peer_mac, 6U);
    memcpy(frame + TEST_ARP_SENDER_IP, peer_ip, 4U);
    memcpy(frame + TEST_ARP_TARGET_MAC, s_netif->hwaddr, 6U);
    memcpy(frame + TEST_ARP_TARGET_IP, &kept_ip.addr, 4U);
}

static void test_frame_routes(void)
{
    uint8_t data[TEST_ETH_HEADER + TEST_IP_HEADER + TEST_TCP_HEADER], original[sizeof(data)];
    uint16_t length = make_ipv4(data, s_netif->hwaddr, TEST_IP_TCP, B1_PC_TCP_PORT);
    struct pbuf packet = {.payload = data, .len = length, .tot_len = length};
    int initial_rx = rx_count;
    memcpy(original, data, length);
    CHECK(handoff_mcu_input(&packet) == 0 && rx_count == initial_rx);
    CHECK(memcmp(data, original, length) == 0);
    /* 旧PC ARP指向06时只修以太网目的地址，TCP全部字节保持原样。 */
    memcpy(data, s_mac, 6U);
    CHECK(handoff_mcu_input(&packet) == 0 && rx_count == initial_rx);
    CHECK(memcmp(data, original, length) == 0);

    length = make_ipv4(data, s_netif->hwaddr, TEST_IP_UDP, TEST_TFTP_DYNAMIC_PORT);
    packet.len = packet.tot_len = length;
    CHECK(handoff_mcu_input(&packet) == 1 && rx_count == initial_rx + 1);
    CHECK(s_rx.length == length && memcmp(s_rx.data, s_mac, 6U) == 0);
    CHECK(memcmp(data, s_netif->hwaddr, 6U) == 0);
    CHECK(memcmp(data + 6U, s_rx.data + 6U, length - 6U) == 0);
    /* UDP即便端口号也是8888仍属RK；协议号是分流键的一部分。 */
    put_be16(data + TEST_TRANSPORT + 2U, B1_PC_TCP_PORT);
    CHECK(handoff_mcu_input(&packet) == 1 && rx_count == initial_rx + 2);

    make_arp(data, 1U);
    memset(data, 255, 6U);
    packet.len = packet.tot_len = TEST_ARP_LENGTH;
    CHECK(handoff_mcu_input(&packet) == 0 && rx_count == initial_rx + 2);
    make_arp(data, 2U);
    CHECK(handoff_mcu_input(&packet) == 0 && rx_count == initial_rx + 3);
    CHECK(memcmp(data + TEST_ARP_TARGET_MAC, s_netif->hwaddr, 6U) == 0);
    CHECK(memcmp(s_rx.data + TEST_ARP_TARGET_MAC, s_mac, 6U) == 0);
    CHECK(memcmp(s_rx.data, s_mac, 6U) == 0);

    /* 真实SN_SEND只改副本的源MAC；异步输出仍由tcpip拥有缓冲。 */
    sn_frame_t send = {.op = SN_SEND, .session = s_state.session,
                      .sequence = s_server.request.sequence + 1U};
    send.length = make_ipv4(send.data, peer_mac, TEST_IP_UDP, TEST_TFTP_DYNAMIC_PORT);
    memcpy(send.data + 6U, s_mac, 6U);
    memcpy(send.data + TEST_IP_SOURCE, &kept_ip.addr, 4U);
    memcpy(send.data + TEST_IP_DESTINATION, peer_ip, 4U);
    sn_frame_t reply;
    int initial_tx = tx_count;
    CHECK(sn_server_handle(&s_server, &send, &reply, execute, NULL) == 1);
    CHECK(reply.status == SN_IO_ERROR && pending_callback && s_tx_pending);
    CHECK(memcmp(s_tx.data + 6U, s_netif->hwaddr, 6U) == 0);
    CHECK(memcmp(send.data + 6U, s_mac, 6U) == 0);
    CHECK(memcmp(send.data + TEST_ETH_HEADER, s_tx.data + TEST_ETH_HEADER,
                 send.length - TEST_ETH_HEADER) == 0);
    run_callback();
    CHECK(!s_tx_pending && s_tx_result == ERR_OK && tx_count == initial_tx + 1);
    CHECK(memcmp(packet_bytes, s_tx.data, send.length) == 0);
    CHECK(sn_server_handle(&s_server, &send, &reply, execute, NULL) == 0);
    CHECK(!pending_callback && tx_count == initial_tx + 1);

    make_arp(send.data, 1U);
    send.length = TEST_ARP_LENGTH;
    memcpy(send.data + 6U, s_mac, 6U);
    memcpy(send.data + TEST_ARP_SENDER_MAC, s_mac, 6U);
    memcpy(send.data + TEST_ARP_SENDER_IP, &kept_ip.addr, 4U);
    int initial_callbacks = callback_count;
    CHECK(transmit(&send) == SN_OK && !pending_callback && !s_tx_pending);
    /* 源0探测自身地址同样被MCU本地吞掉，不送到lwIP自撞，也不在LAN重复宣告。 */
    memset(send.data + TEST_ARP_SENDER_IP, 0, 4U);
    CHECK(transmit(&send) == SN_OK && callback_count == initial_callbacks);
    memcpy(send.data + TEST_ARP_SENDER_IP, &kept_ip.addr, 4U);
    memcpy(send.data + TEST_ARP_TARGET_IP, peer_ip, 4U);
    CHECK(transmit(&send) == SN_IO_ERROR && pending_callback);
    CHECK(memcmp(s_tx.data + TEST_ARP_SENDER_MAC, s_netif->hwaddr, 6U) == 0);
    run_callback();
    CHECK(tx_count == initial_tx + 2);
    check_pc_network();
    message("PASS: MCU coexist TCP preservation, dynamic UDP, ARP single owner and SEND MAC translation\n");
}

static void auto_off_complete(uint32_t cookie)
{
    power_state = PWR_CTRL_STATE_OFF;
    poll_connected();
    CHECK(pending_callback);
    run_callback();
    poll_connected();
    CHECK(s_reset_stream && handoff_slt_status(cookie) != HANDOFF_SLT_OFF_READY);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(handoff_slt_status(cookie) == HANDOFF_SLT_OFF_READY);
    CHECK(actual_baud == SN_BOOT_BAUD && !s_server.valid && !s_parser.used);
}

static void auto_grant(sn_frame_t *hello)
{
    receive_serial(hello);
    CHECK(s_response.status == SN_NOT_READY && !s_active);
    poll_connected();
    CHECK(pending_callback);
    run_callback();
    poll_connected();
    ++hello->sequence;
    receive_serial(hello);
    CHECK(s_response.status == SN_OK && s_active);
}

static void test_automatic_slt(void)
{
    enum { AUTO_COOKIE = 101U, AUTO_NEXT_COOKIE = 102U, AUTO_SESSION = 1001U };
    CHECK(!handoff_mcu_busy());
    power_state = PWR_CTRL_STATE_ON;
    CHECK(!handoff_slt_begin(0U));
    CHECK(handoff_slt_begin(AUTO_COOKIE));
    test_busy = 1;
    CHECK(handoff_mcu_busy() && s_state.phase == HANDOFF_NORMAL && s_slt.waiting_off);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_slt_prepared(AUTO_COOKIE));
    poll_connected();
    CHECK(s_state.phase == HANDOFF_NORMAL && !s_state.seen_on && !pending_callback);
    CHECK(!handoff_slt_begin(AUTO_NEXT_COOKIE) && !handoff_slt_cancel(AUTO_NEXT_COOKIE));
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_MCU));
    poll_connected();
    CHECK(handoff_mcu_busy() && s_slt.cookie == AUTO_COOKIE);

    /* 旧ON之后的本轮OFF才布防，原RK路径继续处理频率和普通时序。 */
    power_state = PWR_CTRL_STATE_OFF;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_ARMED && !s_state.off_release && !s_slt.waiting_off);
    CHECK(handoff_mcu_rk_poll() == 0 && rk_link_ready_poll(AUTO_COOKIE) == 0);
    CHECK(rk_link_ready_poll(HANDOFF_SPL_READY_TOKEN) == -1);
    power_state = PWR_CTRL_STATE_ON;
    poll_connected();
    rk_link_frequency_begin(AUTO_COOKIE, 2133);
    uint8_t wire[SN_WIRE_MAX];
    int length = tlv_pack_cmd(RK_CMD_QUERY_FREQUENCY, NULL, 0U, 0U, wire, sizeof(wire));
    tlv_frame_t frequency;
    CHECK(tlv_unpack(wire, length, &frequency) == 1);
    CHECK(handoff_mcu_rk_poll() == 0);
    rk_task_handoff_receive(&frequency);
    CHECK(rk_link_frequency_status(AUTO_COOKIE) == 1);
    int jumps = jump_attempts;
    now += 1000U;
    rk_task_handoff_heartbeat();
    uint64_t heartbeat = heartbeat_sequence;
    length = tlv_pack_heartbeat(heartbeat, wire, sizeof(wire));
    for (int i = 0; i < length; ++i) CHECK(!handoff_mcu_rk_observe(wire[i]));
    handoff_mcu_rk_reset_probe();
    tlv_frame_t ready;
    CHECK(tlv_unpack(wire, length, &ready) == 0);
    rk_task_handoff_receive(&ready);
    CHECK(rk_link_ready_poll(AUTO_COOKIE) == 1 && jump_attempts == jumps);
    CHECK(handoff_mcu_rk_poll() == 0);
    CHECK(!handoff_slt_prepared(AUTO_NEXT_COOKIE));
    CHECK(handoff_slt_prepared(AUTO_COOKIE));
    rk_idle = 0;
    CHECK(handoff_mcu_rk_poll() == 1 && jump_attempts == jumps);
    rk_idle = 1; uart_idle = 0;
    CHECK(handoff_mcu_rk_poll() == 1 && jump_attempts == jumps);
    uart_idle = 1;
    CHECK(handoff_mcu_rk_poll() == 1 && jump_attempts == jumps + 1);
    CHECK(s_boot_phase == HANDOFF_BOOT_WAIT_TX && !handoff_slt_cancel(AUTO_COOKIE));
    sn_frame_t hello = {.op = SN_HELLO, .session = AUTO_SESSION, .sequence = 1U};
    auto_grant(&hello);
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_WAITING);

    /* EDk归还完成但最终应答尚未发送/TC，都不能作为正式SLT下电条件。 */
    sn_frame_t release = {.op = SN_RELEASE, .session = AUTO_SESSION,
        .sequence = s_server.request.sequence + 1U};
    receive_serial(&release);
    CHECK(s_response.status == SN_NOT_READY);
    poll_connected(); run_callback(); poll_connected();
    CHECK(s_state.phase == HANDOFF_EDK_IDLE);
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_WAITING);
    ++release.sequence; uart_fail = 1;
    receive_serial(&release);
    CHECK(s_response.status == SN_OK && !s_slt.release_queued);
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_WAITING);
    uart_fail = 0; uart_idle = 0;
    receive_serial(&release);
    CHECK(s_slt.release_queued && handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_WAITING);
    uart_idle = 1;
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_RELEASED);
    now += HANDOFF_START_TIMEOUT_MS * 2U;
    poll_connected();
    CHECK(handoff_mcu_busy() && handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_RELEASED);
    auto_off_complete(AUTO_COOKIE);
    test_busy = 0;
    rk_link_frequency_end(AUTO_COOKIE);

    /* 自动直进前级在prepared前送HELLO：先NOT_READY，不发003E，不提前授权。 */
    CHECK(handoff_slt_begin(AUTO_NEXT_COOKIE));
    test_busy = 1;
    CHECK(s_state.phase == HANDOFF_ARMED);
    CHECK(handoff_mcu_rk_poll() == 0);
    power_state = PWR_CTRL_STATE_ON;
    poll_connected();
    hello.session = AUTO_SESSION + 1U; hello.sequence = 1U;
    length = sn_pack(&hello, wire, sizeof(wire));
    CHECK(length > 0);
    for (int i = 0; i < length; ++i) (void)handoff_mcu_rk_observe(wire[i]);
    CHECK(handoff_slt_status(AUTO_NEXT_COOKIE) == HANDOFF_SLT_EARLY_EDK);
    CHECK(s_response.status == SN_NOT_READY && !s_active);
    poll_connected();
    CHECK(!pending_callback && !handoff_slt_cancel(AUTO_NEXT_COOKIE));
    CHECK(handoff_slt_prepared(AUTO_NEXT_COOKIE));
    poll_connected(); run_callback(); poll_connected();
    ++hello.sequence;
    receive_serial(&hello);
    CHECK(s_response.status == SN_OK && jump_attempts == jumps + 1);
    /* 没有RELEASE的异常掉电只给OFF_READY事实，上层决定报powerOff。 */
    auto_off_complete(AUTO_NEXT_COOKIE);
    test_busy = 0;

    CHECK(handoff_slt_begin(AUTO_COOKIE));
    CHECK(handoff_slt_cancel(AUTO_COOKIE));
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_FAILED);
    /* 上电完成同tick可prepared，reset尚未处理时只等RK先复原，不误拒绝。 */
    CHECK(handoff_slt_begin(AUTO_COOKIE));
    test_busy = 1; power_state = PWR_CTRL_STATE_ON;
    CHECK(s_reset_stream && handoff_slt_prepared(AUTO_COOKIE));
    poll_connected();
    CHECK(handoff_mcu_rk_poll() == 1);
    CHECK(!s_slt.started && jump_attempts == jumps + 1); /* 空时序仍等本轮心跳才发003E。 */
    now += HANDOFF_START_TIMEOUT_MS;
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_FAILED);
    CHECK(handoff_mcu_rk_poll() == 1 && s_boot_phase == HANDOFF_BOOT_FAILED);
    CHECK(jump_attempts == jumps + 1);
    CHECK(handoff_slt_cancel(AUTO_COOKIE)); /* 未跳转的超时可以收回预约，不替上层断电。 */
    CHECK(power_state == PWR_CTRL_STATE_ON && handoff_mcu_rk_poll() == 0);
    CHECK(!handoff_mcu_busy());
    test_busy = 0;
    power_state = PWR_CTRL_STATE_OFF;
    CHECK(handoff_slt_begin(AUTO_COOKIE));
    test_busy = 1; power_state = PWR_CTRL_STATE_ON;
    CHECK(handoff_slt_prepared(AUTO_COOKIE));
    poll_connected();
    CHECK(handoff_mcu_rk_poll() == 1 && !s_slt.started);
    now += 1000U;
    handoff_mcu_rk_poll();
    uart_fail = 1;
    receive_heartbeat(heartbeat_sequence);
    CHECK(handoff_slt_status(AUTO_COOKIE) == HANDOFF_SLT_FAILED && !s_slt.started);
    CHECK(jump_attempts == jumps + 2);
    uart_fail = 0;
    CHECK(handoff_slt_cancel(AUTO_COOKIE));
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy() && power_state == PWR_CTRL_STATE_ON);
    test_busy = 0;
    check_pc_network();
    message("PASS: automatic SLT reservation, original TLV preparation, same-cookie ready, early EDK, RELEASE TC and OFF recovery\n");
}

/* 0104不要求EDK RELEASE；验证尚未上电预约和已授权高速网卡均能完整收回。 */
static void test_explicit_stop(void)
{
    power_state = PWR_CTRL_STATE_OFF;
    CHECK(!handoff_mcu_busy());
    CHECK(handoff_slt_begin(901U));
    handoff_mcu_stop();
    poll_connected();
    CHECK(s_state.phase == HANDOFF_RELEASING && pending_callback);
    run_callback(); poll_connected();
    CHECK(s_state.phase == HANDOFF_NORMAL && s_reset_stream);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(handoff_slt_status(901U) == HANDOFF_SLT_OFF_READY);

    grant_connected(902U);
    s_uart_baud = actual_baud = SN_DATA_BAUD;
    s_parser.used = 3U;
    handoff_mcu_stop();
    CHECK(!s_active && handoff_mcu_busy());
    poll_connected();
    CHECK(handoff_mcu_rk_poll() == 1 && actual_baud == SN_DATA_BAUD);
    power_state = PWR_CTRL_STATE_OFF;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_RELEASING && pending_callback);
    run_callback(); poll_connected();
    CHECK(s_state.phase == HANDOFF_NORMAL && s_reset_stream);
    uart_idle = 0;
    CHECK(handoff_mcu_rk_poll() == 1 && handoff_mcu_busy());
    uart_idle = 1; baud_fail = 1;
    CHECK(handoff_mcu_rk_poll() == 1 && handoff_mcu_busy());
    baud_fail = 0;
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(actual_baud == SN_BOOT_BAUD && !s_parser.used && !s_server.valid);
    check_pc_network();
    message("PASS: explicit stop cancels unstarted reservation and EDK data without RELEASE, drains TX and restores UART\n");
}

void test_main(void)
{
    struct netif netif = {.hwaddr = {2, 3, 4, 5, 6, 7}, .flags = NETIF_FLAG_LINK_UP,
                         .linkoutput = link_output};
    IP4_ADDR(&netif.ip_addr, 192, 168, 1, 211);
    IP4_ADDR(&netif.netmask, 255, 255, 255, 0);
    IP4_ADDR(&netif.gw, 192, 168, 1, 1);
    kept_ip = netif.ip_addr;
    kept_mask = netif.netmask;
    kept_gateway = netif.gw;
    s_rx_queue = (QueueHandle_t)(uintptr_t)1U;
    s_tx_done = (SemaphoreHandle_t)(uintptr_t)2U;
    handoff_mcu_attach(&netif);
    check_pc_network();

    /* 保留原有OFF、测试空闲、串口排空门禁；PC连接本身不再阻止授权。 */
    power_state = PWR_CTRL_STATE_ON;
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    poll_connected();
    CHECK(!handoff_mcu_busy());
    power_state = PWR_CTRL_STATE_OFF;
    test_busy = 1;
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    poll_connected();
    CHECK(!handoff_mcu_busy());
    test_busy = 0;
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    poll_connected();
    CHECK(s_state.phase == HANDOFF_ARMED);
    CHECK(handoff_mcu_rk_poll() == 1);
    power_state = PWR_CTRL_STATE_ON;
    poll_connected();
    sn_frame_t hello = {.op = SN_HELLO, .session = 17U, .sequence = 1U};
    receive_serial(&hello);
    CHECK(s_response.status == SN_NOT_READY && !s_active);
    CHECK(transact(SN_RESET, 17U) == SN_NOT_READY && !s_active);
    CHECK(transact(SN_RELEASE, 17U) == SN_INVALID);
    CHECK(transact(SN_HELLO, 18U) == SN_INVALID && s_state.session == 17U);
    uart_idle = 0;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_ARMED && !pending_callback);
    uart_idle = 1;
    callback_reject = 1;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_WITHDRAWING && !s_network_pending);
    callback_reject = 0;
    eth_idle = 0;
    poll_connected();
    CHECK(pending_callback);
    run_callback();
    check_pc_network();
    /* PC自身可以仍在发送以太帧；授权只记地址快照，不撤IP也不清TCP状态。 */
    CHECK(s_network_ok && s_saved);
    poll_connected();
    CHECK(s_state.phase == HANDOFF_OWNED && !s_active);
    CHECK(transact(SN_HELLO, 17U) == SN_OK && s_active);
    CHECK(s_response.length == SN_HELLO_INFO_SIZE);
    CHECK(memcmp(s_response.data + SN_HELLO_IP_OFFSET, &kept_ip.addr, 4U) == 0);
    CHECK(memcmp(s_response.data + SN_HELLO_MASK_OFFSET, &kept_mask.addr, 4U) == 0);
    CHECK(memcmp(s_response.data + SN_HELLO_GATEWAY_OFFSET, &kept_gateway.addr, 4U) == 0);
    CHECK(arp_count == 1);
    eth_idle = 1;
    test_baud();
    check_pc_network();
    test_frame_routes();

    /* SNP停用/复位只影响RK转发，不关闭MCU监听，也不能换会话抢占。 */
    CHECK(transact(SN_STOP, 17U) == SN_OK && !s_active);
    check_pc_network();
    CHECK(transact(SN_RESET, 17U) == SN_OK && s_active);
    CHECK(transact(SN_RELEASE, 18U) == SN_INVALID && s_active);
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_MCU));
    poll_connected();
    CHECK(s_state.phase == HANDOFF_OWNED);

    /* RELEASE立即停止新包；只等本桥接tcpip回调，PC持续发包不能阻止会话结束。 */
    CHECK(transact(SN_RELEASE, 17U) == SN_NOT_READY && !s_active);
    s_tx_pending = 1U;
    poll_connected();
    run_callback();
    CHECK(!s_network_ok && s_state.phase == HANDOFF_RELEASING);
    check_pc_network();
    s_tx_pending = 0U;
    eth_idle = 0;
    poll_connected();
    run_callback();
    CHECK(s_network_ok);
    check_pc_network();
    CHECK(transact(SN_RELEASE, 17U) == SN_NOT_READY);
    /* 网络回调完成后没有listener；已有PC连接仍须使RESTORED推进到EDK_IDLE。 */
    CHECK(s_state.phase == HANDOFF_RELEASING && s_network_done);
    poll_connected();
    CHECK(s_state.phase == HANDOFF_EDK_IDLE && handoff_mcu_busy());
    CHECK(transact(SN_RELEASE, 17U) == SN_OK);
    sn_frame_t replay;
    CHECK(sn_server_handle(&s_server, &s_server.request, &replay, execute, NULL) == 0);
    CHECK(replay.status == SN_OK);
    CHECK(transact(SN_RESET, 17U) == SN_NOT_READY && !s_active);
    CHECK(transact(SN_HELLO, 18U) == SN_INVALID);
    CHECK(!handoff_mcu_power_allowed(0) && handoff_mcu_power_allowed(1));
    CHECK(arp_count == 1);

    /* RELEASE不代表RK已回SPL；仍等真实OFF后恢复115200及唯一TLV消费者。 */
    now += 1000000U;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_EDK_IDLE);
    power_state = PWR_CTRL_STATE_OFF;
    eth_idle = 0;
    s_tx_pending = 1U;
    poll_connected();
    run_callback();
    CHECK(s_state.phase == HANDOFF_EDK_IDLE && !s_network_ok);
    s_tx_pending = 0U;
    poll_connected();
    run_callback();
    CHECK(s_network_ok);
    poll_connected();
    CHECK(handoff_mcu_busy() && !handoff_mcu_power_allowed(0));
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy() && !s_server.valid);
    CHECK(actual_baud == SN_BOOT_BAUD && s_uart_baud == SN_BOOT_BAUD);
    check_pc_network();

    /* 没有RELEASE就掉电也保留原PC连接，先停止转发，再清会话。 */
    grant_connected(99U);
    /* 旧EDK仍可请求2M；必须按已回复的具体值切换，不能偷偷换成新版默认4M。 */
    sn_frame_t legacy_rate = {.op = SN_HELLO, .session = 99U,
        .sequence = s_server.request.sequence + 1U, .length = SN_RATE_REQUEST_SIZE};
    for (unsigned i = 0U; i < SN_RATE_REQUEST_SIZE; ++i)
        legacy_rate.data[i] = (uint8_t)(SN_LEGACY_DATA_BAUD >> (i * 8U));
    CHECK(actual_baud == SN_BOOT_BAUD);
    receive_serial(&legacy_rate);
    CHECK(s_response.status == SN_OK && s_rate_pending);
    CHECK(s_requested_baud == SN_LEGACY_DATA_BAUD && actual_baud == SN_BOOT_BAUD);
    CHECK(memcmp(s_response.data, legacy_rate.data, SN_RATE_REQUEST_SIZE) == 0);
    uart_idle = 0;
    handoff_mcu_rk_poll();
    CHECK(s_rate_pending && actual_baud == SN_BOOT_BAUD);
    uart_idle = 1;
    handoff_mcu_rk_poll();
    CHECK(s_rate_trial && s_uart_baud == SN_LEGACY_DATA_BAUD);
    CHECK(actual_baud == SN_LEGACY_DATA_BAUD && actual_baud != SN_DATA_BAUD);
    legacy_rate.length = 0U;
    ++legacy_rate.sequence;
    receive_serial(&legacy_rate);
    CHECK(s_response.status == SN_OK && !s_rate_trial);
    now += SN_RATE_TRIAL_MS * 2U;
    handoff_mcu_rk_poll();
    CHECK(actual_baud == SN_LEGACY_DATA_BAUD);
    check_pc_network();
    power_state = PWR_CTRL_STATE_OFF;
    poll_connected();
    CHECK(s_state.phase == HANDOFF_RELEASING && !s_active);
    run_callback();
    poll_connected();
    CHECK(s_state.phase == HANDOFF_NORMAL && s_reset_stream);
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(actual_baud == SN_BOOT_BAUD && s_uart_baud == SN_BOOT_BAUD);
    check_pc_network();
    message("PASS: MCU legacy 2M request, ACK/TC switching, confirmation and OFF baud recovery\n");

    /* 尚未授权时取消准备同样不依赖PC主动断开。 */
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_RK));
    poll_connected();
    CHECK(handoff_mcu_console_request(HANDOFF_COMMAND_MCU));
    poll_connected();
    run_callback();
    poll_connected();
    CHECK(handoff_mcu_rk_poll() == 0 && !handoff_mcu_busy());
    CHECK(!s_state.authorized);
    test_receive_wait();
    test_automatic_slt();
    test_explicit_stop();
    check_pc_network();
    message("PASS: MCU coexist grant, connected PC without listener, RELEASE completion and OFF recovery under PC TX\n");
    finish(0U);
}
