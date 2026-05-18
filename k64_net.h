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
    uint32_t dns_server;
    uint64_t tx_frames;
    uint64_t rx_frames;
    uint64_t rx_arp;
    uint64_t rx_ipv4;
    uint64_t rx_udp;
    uint64_t rx_icmp;
    uint64_t rx_tcp;
    uint64_t rx_dns;
    uint64_t ping_replies;
    uint16_t last_ethertype;
    uint16_t last_rx_len;
} k64_net_status_t;

void k64_net_init(void);
bool k64_net_register_device(const char* driver, const uint8_t mac[6], k64_net_send_frame_fn send);
void k64_net_unregister_device(void);
bool k64_net_is_ready(void);
void k64_net_receive_frame(const uint8_t* frame, uint16_t len);
bool k64_net_send_arp_request(uint32_t target_ip);
bool k64_net_dhcp_discover(void);
bool k64_net_send_udp(uint32_t dst_ip, uint16_t dst_port, const char* payload);
bool k64_net_send_ping(uint32_t dst_ip, uint16_t sequence);
bool k64_net_resolve_host(const char* host, uint32_t* out_ip, bool* pending);
bool k64_net_http_get(const char* host, const char* path, uint16_t port, char* out, size_t out_size, const char** state);
bool k64_net_http_get_raw(const char* host, const char* path, uint16_t port, uint8_t* out, size_t out_capacity, size_t* out_size, const char** state);
bool k64_net_http_get_range_raw(const char* host, const char* path, uint16_t port, size_t range_start, size_t range_end, uint8_t* out, size_t out_capacity, size_t* out_size, const char** state);
bool k64_net_http_download_to_file(const char* host, const char* path, uint16_t port, const char* dst_path, size_t max_size_or_0, const char** state);
bool k64_net_status(k64_net_status_t* out);
bool k64_net_parse_ipv4(const char* text, uint32_t* out);
void k64_net_format_ipv4(uint32_t ip, char* out, size_t out_size);
