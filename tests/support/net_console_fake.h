#pragma once

#include "net_console.h"

/* 两个网络助手的独立输入、输出和阻塞状态，供控制台与诊断回归注入。 */
typedef struct {
    uint32_t session;
    int blocked;
    char output[16384];
    size_t output_length;
    uint8_t input[512];
    size_t input_length;
    uint32_t dropped;
} fake_net_peer_t;

extern fake_net_peer_t fake_net_peers[2];
