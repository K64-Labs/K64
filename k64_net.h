#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*k64_net_send_frame_fn)(const uint8_t* frame, uint16_t len);

typedef struct {
    bool     link_up;
    char     driver[24];
    uint8_t  mac[6];
    uint32_t ipv4;
    uint32_t gateway;
    uint32_t netmask;
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint64_t rx_arp;
    uint64_t rx_ipv4;
    uint64_t rx_udp;
    uint64_t rx_icmp;
    uint16_t last_ethertype;
    uint16_t last_rx_len;
} k64_net_status_t;

void k64_net_init(void);
bool k64_net_register_device(const char* driver, const uint8_t mac[6], k64_net_send_frame_fn send);
void k64_net_unregister_device(void);
bool k64_net_is_ready(void);
void k64_net_receive_frame(const uint8_t* frame, uint16_t len);
bool k64_net_send_arp_request(uint32_t target_ip);
bool k64_net_send_udp(uint32_t dst_ip, uint16_t dst_port, const char* payload);
bool k64_net_send_ping(uint32_t dst_ip, uint16_t sequence);
bool k64_net_status(k64_net_status_t* out);
bool k64_net_parse_ipv4(const char* text, uint32_t* out);
void k64_net_format_ipv4(uint32_t ip, char* out, size_t out_size);
