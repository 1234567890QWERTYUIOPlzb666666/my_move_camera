#include "../inc/wifi_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void shell_quote_append(char * out, size_t out_size, const char * text)
{
    size_t used = strlen(out);

    if (used + 2 >= out_size) return;
    out[used++] = '\'';
    out[used] = '\0';

    for (const char * p = text; *p != '\0' && used + 5 < out_size; p++) {
        if (*p == '\'') {
            snprintf(out + used, out_size - used, "'\\''");
            used = strlen(out);
        } else {
            out[used++] = *p;
            out[used] = '\0';
        }
    }

    if (used + 2 < out_size) {
        out[used++] = '\'';
        out[used] = '\0';
    }
}

static void text_trim(char * text)
{
    char * start = text;
    char * end;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
}

static bool command_run(const char * command)
{
    int rc = system(command);
    return rc == 0;
}

static bool command_read_line(const char * command, char * out, size_t out_size)
{
    FILE * fp;

    if (out == NULL || out_size == 0) {
        return false;
    }

    out[0] = '\0';
    fp = popen(command, "r");
    if (fp == NULL) {
        return false;
    }

    if (fgets(out, out_size, fp) == NULL) {
        pclose(fp);
        return false;
    }
    pclose(fp);
    text_trim(out);
    return out[0] != '\0';
}

static bool network_add(wifi_manager_network_t * networks,
                        size_t max_count,
                        size_t * count,
                        const char * ssid,
                        const char * detail,
                        bool needs_password)
{
    wifi_manager_network_t * network;

    if (networks == NULL || count == NULL || *count >= max_count ||
        ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    network = &networks[(*count)++];
    snprintf(network->ssid, sizeof(network->ssid), "%s", ssid);
    snprintf(network->detail, sizeof(network->detail), "%s", detail != NULL ? detail : "");
    network->needs_password = needs_password;
    return true;
}

static bool wifi_get_iface(char * out, size_t out_size)
{
#ifdef WSL
    (void)out;
    (void)out_size;
    return false;
#else
    const char * env_iface = getenv("MY_MOVE_CAMERA_WIFI_IFACE");
    DIR * dir;
    struct dirent * entry;

    if (out == NULL || out_size == 0) {
        return false;
    }

    if (env_iface != NULL && env_iface[0] != '\0') {
        snprintf(out, out_size, "%s", env_iface);
        return true;
    }

    dir = opendir("/sys/class/net");
    if (dir == NULL) {
        snprintf(out, out_size, "%s", "wlan0");
        return true;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "wlan", 4) == 0 ||
            strncmp(entry->d_name, "wlp", 3) == 0 ||
            strncmp(entry->d_name, "wifi", 4) == 0) {
            snprintf(out, out_size, "%s", entry->d_name);
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    snprintf(out, out_size, "%s", "wlan0");
    return true;
#endif
}

static void wifi_iface_up(const char * iface)
{
#ifndef WSL
    char command[160] = "ifconfig ";

    if (iface == NULL || iface[0] == '\0') {
        return;
    }

    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " up >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    command_run(command);
#else
    (void)iface;
#endif
}

static void wifi_supplicant_start(const char * iface)
{
#ifndef WSL
    char command[384] = "wpa_supplicant -B -i ";

    if (iface == NULL || iface[0] == '\0') {
        return;
    }

    shell_quote_append(command, sizeof(command), iface);
    strncat(command,
            " -c /userdata/cfg/wpa_supplicant.conf >/dev/null 2>&1 || wpa_supplicant -B -i ",
            sizeof(command) - strlen(command) - 1);
    shell_quote_append(command, sizeof(command), iface);
    strncat(command,
            " -c /etc/wpa_supplicant.conf >/dev/null 2>&1",
            sizeof(command) - strlen(command) - 1);
    command_run(command);
#else
    (void)iface;
#endif
}

static void scan_add_iw_bss(wifi_manager_network_t * networks,
                            size_t max_count,
                            size_t * count,
                            const char * ssid,
                            const char * signal,
                            bool encrypted)
{
    char detail[WIFI_MANAGER_DETAIL_MAX];

    if (ssid == NULL || ssid[0] == '\0' || *count >= max_count) {
        return;
    }

    snprintf(detail,
             sizeof(detail),
             "%s  Signal %s",
             encrypted ? "WPA/WPA2" : "Open",
             signal != NULL && signal[0] != '\0' ? signal : "--");
    network_add(networks, max_count, count, ssid, detail, encrypted);
}

static void wpa_value_quote(char * out, size_t out_size, const char * text)
{
    size_t used = 0;

    if (out == NULL || out_size == 0) {
        return;
    }

    out[used++] = '"';
    out[used] = '\0';

    for (const char * p = text; p != NULL && *p != '\0' && used + 3 < out_size; p++) {
        if (*p == '\\' || *p == '"') {
            out[used++] = '\\';
        }
        out[used++] = *p;
        out[used] = '\0';
    }

    if (used + 1 < out_size) {
        out[used++] = '"';
        out[used] = '\0';
    }
}

size_t wifi_manager_scan(wifi_manager_network_t * networks, size_t max_count)
{
#ifdef WSL
    size_t count = 0;

    if (networks == NULL || max_count == 0) {
        return 0;
    }

    memset(networks, 0, max_count * sizeof(networks[0]));
    network_add(networks, max_count, &count, "Taishan_Camera_5G", "WPA2  Signal 92%", true);
    network_add(networks, max_count, &count, "Office-Lab", "WPA2  Signal 78%", true);
    network_add(networks, max_count, &count, "Guest_WiFi", "Open  Signal 65%", false);
    network_add(networks, max_count, &count, "Phone Hotspot", "WPA2  Signal 54%", true);
    network_add(networks, max_count, &count, "Workshop-IoT", "WPA2  Signal 41%", true);
    return count;
#else
    FILE * fp;
    char iface[32];
    char command[160] = "iw dev ";
    char line[256];
    size_t count = 0;
    char ssid[WIFI_MANAGER_SSID_MAX] = "";
    char signal[32] = "";
    bool encrypted = false;

    if (networks == NULL || max_count == 0) {
        return 0;
    }

    memset(networks, 0, max_count * sizeof(networks[0]));
    if (!wifi_get_iface(iface, sizeof(iface))) {
        return 0;
    }

    wifi_iface_up(iface);
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " scan 2>/dev/null", sizeof(command) - strlen(command) - 1);
    fp = popen(command, "r");
    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL && count < max_count) {
        text_trim(line);
        if (line[0] == '\0') continue;

        if (strncmp(line, "BSS ", 4) == 0) {
            scan_add_iw_bss(networks, max_count, &count, ssid, signal, encrypted);
            ssid[0] = '\0';
            signal[0] = '\0';
            encrypted = false;
        } else if (strncmp(line, "SSID: ", 6) == 0) {
            snprintf(ssid, sizeof(ssid), "%s", line + 6);
        } else if (strncmp(line, "signal: ", 8) == 0) {
            snprintf(signal, sizeof(signal), "%s", line + 8);
        } else if (strncmp(line, "capability:", 11) == 0 && strstr(line, "Privacy") != NULL) {
            encrypted = true;
        } else if (strncmp(line, "RSN:", 4) == 0 || strncmp(line, "WPA:", 4) == 0) {
            encrypted = true;
        }
    }
    if (count < max_count) {
        scan_add_iw_bss(networks, max_count, &count, ssid, signal, encrypted);
    }
    pclose(fp);
    return count;
#endif
}

bool wifi_manager_connect(const char * ssid, const char * password)
{
#ifdef WSL
    (void)ssid;
    (void)password;
    return false;
#else
    char iface[32];
    char network_id[32];
    char ssid_value[160];
    char password_value[160];
    char command[512];

    if (!wifi_get_iface(iface, sizeof(iface))) {
        return false;
    }

    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " add_network 2>/dev/null", sizeof(command) - strlen(command) - 1);
    wifi_iface_up(iface);
    if (!command_read_line(command, network_id, sizeof(network_id)) ||
        strcmp(network_id, "FAIL") == 0) {
        wifi_supplicant_start(iface);
    }
    if (!command_read_line(command, network_id, sizeof(network_id)) ||
        strcmp(network_id, "FAIL") == 0) {
        return false;
    }

    wpa_value_quote(ssid_value, sizeof(ssid_value), ssid);
    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " set_network ", sizeof(command) - strlen(command) - 1);
    shell_quote_append(command, sizeof(command), network_id);
    strncat(command, " ssid ", sizeof(command) - strlen(command) - 1);
    shell_quote_append(command, sizeof(command), ssid_value);
    strncat(command, " >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    if (!command_run(command)) {
        return false;
    }

    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " set_network ", sizeof(command) - strlen(command) - 1);
    shell_quote_append(command, sizeof(command), network_id);
    if (password != NULL && password[0] != '\0') {
        wpa_value_quote(password_value, sizeof(password_value), password);
        strncat(command, " psk ", sizeof(command) - strlen(command) - 1);
        shell_quote_append(command, sizeof(command), password_value);
    } else {
        strncat(command, " key_mgmt NONE", sizeof(command) - strlen(command) - 1);
    }
    strncat(command, " >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    if (!command_run(command)) {
        return false;
    }

    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " enable_network ", sizeof(command) - strlen(command) - 1);
    shell_quote_append(command, sizeof(command), network_id);
    strncat(command, " >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    if (!command_run(command)) {
        return false;
    }

    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " save_config >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    command_run(command);

    snprintf(command, sizeof(command), "wpa_cli -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " reconnect >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    if (!command_run(command)) {
        return false;
    }

    snprintf(command, sizeof(command), "udhcpc -i ");
    shell_quote_append(command, sizeof(command), iface);
    strncat(command, " -q -n >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    command_run(command);
    return true;
#endif
}
