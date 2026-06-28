#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

#define WIFI_MANAGER_SSID_MAX 128
#define WIFI_MANAGER_DETAIL_MAX 128

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX];
    char detail[WIFI_MANAGER_DETAIL_MAX];
    bool needs_password;
} wifi_manager_network_t;

size_t wifi_manager_scan(wifi_manager_network_t * networks, size_t max_count);
bool wifi_manager_connect(const char * ssid, const char * password);

#endif
