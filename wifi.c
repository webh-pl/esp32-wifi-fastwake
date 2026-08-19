#include "wifi.h"

#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if !ETHARP_SUPPORT_STATIC_ENTRIES
#error "gateway ARP cache requires CONFIG_LWIP_DHCPS_STATIC_ENTRIES"
#endif

#ifdef CONFIG_LWIP_SNTP_STARTUP_DELAY
#error "SNTP startup delay would exceed wifi NTP sync wait; disable CONFIG_LWIP_SNTP_STARTUP_DELAY"
#endif

static const char *TAG = "wifi_sta";

/* 802.11g/n: the 2.4 GHz bitmap must keep the 11b bit, so the 11b rates are
 * dropped separately. 11ax stays off. */
#define WIFI_PROTO_BITMAP (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)

#define WIFI_ASSOC_BIT           BIT0
#define WIFI_GOT_IP_BIT          BIT1
#define WIFI_DISCONNECTED_BIT    BIT2

#define WIFI_CONNECT_TIMEOUT_MS  6000
#define WIFI_DISCONNECT_TIMEOUT_MS 2000
#define WIFI_STALE_RETRY_DELAY_MS  100
#define WIFI_FIRST_TRAFFIC_DELAY_MS 20

#define WIFI_NTP_SANITY_UNIX     1704067200LL /* 2024-01-01 UTC */
#define WIFI_NTP_INTERVAL_S      86400
#define WIFI_NTP_SYNC_TIMEOUT_MS 3000
#define WIFI_NTP_SERVER_PRIMARY  "213.222.200.99"
/* tempus1.gum.gov.pl — update if GUM renumbers this host. */
#define WIFI_NTP_SERVER_BACKUP   "194.146.251.100"
/* Europe/Warsaw: CET/CEST with EU DST (last Sunday of March/October). */
#define WIFI_TZ_WARSAW           "CET-1CEST,M3.5.0/2,M10.5.0/3"

#define CACHE_MAGIC   0x57494643u /* 'WIFC' */
#define CACHE_VERSION 4

#define NTP_MAGIC     0x574e5450u /* 'WNTP' */
#define NTP_VERSION   1

/*
 * Common prefix of every struct kept in LP SRAM. magic/version/size sit outside
 * the checksum on purpose: they reject a foreign or stale layout before crc32 is
 * computed over a length that may no longer be the right one. Everything from
 * profile_hash to the end of the owning struct is covered by the CRC.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t crc32;
    uint32_t profile_hash;
} wifi_rtc_hdr_t;

typedef struct {
    wifi_rtc_hdr_t hdr;
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t dns_main;
    esp_ip4_addr_t dns_backup;
    uint8_t bssid[6];
    uint8_t gw_mac[6]; /* zero = unknown; learned at wifi_disconnect() */
    uint8_t channel;
    uint8_t reserved[3];
} wifi_cache_t;

/*
 * Last successful NTP sync. profile_hash in the shared header is always 0:
 * wall time is device-global and must survive an SSID/PMK change.
 */
typedef struct {
    wifi_rtc_hdr_t hdr;
    int64_t last_success_unix;
} wifi_rtc_ntp_t;

typedef struct {
    err_t err;
    bool found;
    ip4_addr_t ip;
    struct eth_addr eth;
    struct netif *netif;
    uint8_t mac[6];
} arp_cb_ctx_t;

/*
 * RTC_NOINIT_ATTR, not RTC_DATA_ATTR: .rtc.data is reloaded from the image on
 * every boot that is not a deep sleep wake, so a USB/JTAG or watchdog reset
 * would silently wipe the network profile and gateway MAC. .rtc_noinit keeps
 * its content across those resets and only loses it on power loss, which the
 * per-struct validation below already treats as garbage.
 */
static RTC_NOINIT_ATTR wifi_cache_t s_rtc_cache;
static RTC_NOINIT_ATTR wifi_rtc_ntp_t s_rtc_ntp;

/*
 * BROKEN is reached only when wifi_init() failed *and* its rollback could not
 * fully undo the global driver state. Retrying on top of that would build on
 * half-torn-down IDF internals, so the module refuses further calls and the
 * application is expected to reset the device.
 */
typedef enum {
    WIFI_STATE_UNINIT = 0,
    WIFI_STATE_READY,
    WIFI_STATE_BROKEN,
} wifi_state_t;

static wifi_state_t s_state;
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_netif_ip_info_t s_ip_info;
static esp_netif_dns_info_t s_dns_main;
static esp_netif_dns_info_t s_dns_backup;
static bool s_ip_ready;
static bool s_ever_connected;
static bool s_use_cache;
static bool s_arp_restored;
static bool s_hints_pending;
static bool s_force_full;
static bool s_last_rssi_valid;
static uint32_t s_profile_hash;
static uint8_t s_last_reason;
static int8_t s_last_rssi;
static char s_ssid[33];
static char s_pmk_hex[65];
static wifi_cache_t s_cache;
static wifi_link_check_fn s_link_check;

static int64_t s_t0_us;
static int64_t s_ip_us;

/* ---------- shared LP SRAM header ---------- */

/* The helpers below reach the header through a void pointer, which only holds
 * while it stays the first member of every struct they are called with. */
_Static_assert(offsetof(wifi_cache_t, hdr) == 0, "cache header must be first");
_Static_assert(offsetof(wifi_rtc_ntp_t, hdr) == 0, "ntp header must be first");

static uint32_t rtc_crc(const void *s, size_t size)
{
    const size_t off = offsetof(wifi_rtc_hdr_t, profile_hash);
    return esp_rom_crc32_le(0, (const uint8_t *)s + off, size - off);
}

/* Call after every field write; the CRC only means something once it is last. */
static void rtc_seal(void *s, size_t size, uint32_t magic, uint16_t version)
{
    wifi_rtc_hdr_t *hdr = (wifi_rtc_hdr_t *)s;
    hdr->magic = magic;
    hdr->version = version;
    hdr->size = (uint16_t)size;
    hdr->crc32 = rtc_crc(s, size);
}

static bool rtc_hdr_ok(const void *s, size_t size, uint32_t magic, uint16_t version,
                       uint32_t profile_hash)
{
    const wifi_rtc_hdr_t *hdr = (const wifi_rtc_hdr_t *)s;
    return hdr->magic == magic &&
           hdr->version == version &&
           hdr->size == size &&
           hdr->profile_hash == profile_hash &&
           hdr->crc32 == rtc_crc(s, size);
}

/* ---------- network profile cache in LP SRAM ---------- */

static bool ipv4_unicast_host(const esp_ip4_addr_t *addr)
{
    if (addr->addr == IPADDR_ANY || addr->addr == IPADDR_BROADCAST ||
        ip4_addr_ismulticast(addr)) {
        return false;
    }

    const uint8_t first_octet = ip4_addr1(addr);
    return first_octet != 0 && first_octet != 127;
}

static bool mac_is_zero_or_broadcast(const uint8_t mac[6])
{
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) {
            all_zero = false;
        }
        if (mac[i] != 0xff) {
            all_ff = false;
        }
    }
    return all_zero || all_ff;
}

static bool cache_semantics_ok(const wifi_cache_t *c)
{
    if (c->channel < 1 || c->channel > 13) {
        return false;
    }

    if (mac_is_zero_or_broadcast(c->bssid)) {
        return false;
    }

    const esp_ip4_addr_t *ip = &c->ip.ip;
    const esp_ip4_addr_t *mask = &c->ip.netmask;
    const esp_ip4_addr_t *gw = &c->ip.gw;

    if (!ipv4_unicast_host(ip) || !ipv4_unicast_host(gw) ||
        mask->addr == IPADDR_ANY || mask->addr == IPADDR_BROADCAST ||
        !ip4_addr_netmask_valid(mask->addr) ||
        !ip4_addr_net_eq(ip, gw, mask)) {
        return false;
    }

    const uint32_t network = ip->addr & mask->addr;
    const uint32_t broadcast = network | ~mask->addr;
    if (ip->addr == network || ip->addr == broadcast ||
        gw->addr == network || gw->addr == broadcast) {
        return false;
    }
    return true;
}

static bool cache_valid(const wifi_cache_t *c, uint32_t profile_hash)
{
    return rtc_hdr_ok(c, sizeof(*c), CACHE_MAGIC, CACHE_VERSION, profile_hash) &&
           cache_semantics_ok(c);
}

static bool cache_load(uint32_t profile_hash, wifi_cache_t *out)
{
    if (out == NULL) {
        return false;
    }

    if (cache_valid(&s_rtc_cache, profile_hash)) {
        *out = s_rtc_cache;
        return true;
    }

    memset(&s_rtc_cache, 0, sizeof(s_rtc_cache));
    return false;
}

static void cache_save(wifi_cache_t *cache)
{
    memset(cache->reserved, 0, sizeof(cache->reserved));
    rtc_seal(cache, sizeof(*cache), CACHE_MAGIC, CACHE_VERSION);
    s_rtc_cache = *cache;
}

/* ---------- gateway ARP cache ---------- */

static bool arp_mac_ok(const uint8_t mac[6])
{
    return !mac_is_zero_or_broadcast(mac) && (mac[0] & 0x01u) == 0;
}

static void arp_add_cb(void *arg)
{
    arp_cb_ctx_t *ctx = (arp_cb_ctx_t *)arg;
    ctx->err = etharp_add_static_entry(&ctx->ip, &ctx->eth);
}

static void arp_find_cb(void *arg)
{
    arp_cb_ctx_t *ctx = (arp_cb_ctx_t *)arg;
    struct eth_addr *eth_ret = NULL;
    const ip4_addr_t *ip_ret = NULL;

    ctx->found = false;
    if (etharp_find_addr(ctx->netif, &ctx->ip, &eth_ret, &ip_ret) >= 0 && eth_ret != NULL) {
        memcpy(ctx->mac, eth_ret->addr, sizeof(ctx->mac));
        ctx->found = true;
    }
}

static void arp_cleanup_cb(void *arg)
{
    etharp_cleanup_netif((struct netif *)arg);
}

static esp_err_t arp_restore_to_lwip(void)
{
    if (!arp_mac_ok(s_cache.gw_mac) || s_cache.ip.gw.addr != s_ip_info.gw.addr) {
        return ESP_ERR_INVALID_STATE;
    }

    arp_cb_ctx_t ctx = {0};
    ctx.ip.addr = s_ip_info.gw.addr;
    memcpy(ctx.eth.addr, s_cache.gw_mac, sizeof(ctx.eth.addr));

    err_t post = tcpip_callback_wait(arp_add_cb, &ctx);
    if (post != ERR_OK) {
        return ESP_FAIL;
    }
    if (ctx.err != ERR_OK) {
        ESP_LOGW(TAG, "arp restore: %d", (int)ctx.err);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void arp_learn_from_lwip(void)
{
    if (arp_mac_ok(s_cache.gw_mac)) {
        return;
    }
    if (!cache_valid(&s_rtc_cache, s_profile_hash)) {
        return;
    }
    if (s_ip_info.gw.addr == 0) {
        return;
    }

    struct netif *netif = (struct netif *)esp_netif_get_netif_impl(s_netif);
    if (netif == NULL) {
        return;
    }

    arp_cb_ctx_t ctx = {0};
    ctx.ip.addr = s_ip_info.gw.addr;
    ctx.netif = netif;

    if (tcpip_callback_wait(arp_find_cb, &ctx) != ERR_OK || !ctx.found || !arp_mac_ok(ctx.mac)) {
        return;
    }

    memcpy(s_rtc_cache.gw_mac, ctx.mac, sizeof(s_rtc_cache.gw_mac));
    rtc_seal(&s_rtc_cache, sizeof(s_rtc_cache), CACHE_MAGIC, CACHE_VERSION);
    memcpy(s_cache.gw_mac, ctx.mac, sizeof(s_cache.gw_mac));

    ESP_LOGI(TAG, "arp learned gw " IPSTR " %02x:%02x:%02x:%02x:%02x:%02x",
             IP2STR(&s_ip_info.gw),
             s_cache.gw_mac[0], s_cache.gw_mac[1], s_cache.gw_mac[2],
             s_cache.gw_mac[3], s_cache.gw_mac[4], s_cache.gw_mac[5]);
}

/*
 * Drop the gateway MAC from the profile and clear this STA netif's ARP table.
 * etharp_cleanup_netif() also frees static entries and cannot fail; it may
 * drop unrelated dynamic mappings on the same netif (accepted: at most one
 * extra ARP query after repair).
 */
static void arp_forget(void)
{
    memset(s_cache.gw_mac, 0, sizeof(s_cache.gw_mac));
    if (cache_valid(&s_rtc_cache, s_profile_hash)) {
        memset(s_rtc_cache.gw_mac, 0, sizeof(s_rtc_cache.gw_mac));
        rtc_seal(&s_rtc_cache, sizeof(s_rtc_cache), CACHE_MAGIC, CACHE_VERSION);
    }

    struct netif *netif = (struct netif *)esp_netif_get_netif_impl(s_netif);
    if (netif == NULL) {
        ESP_LOGW(TAG, "arp forget: no netif");
        return;
    }
    if (tcpip_callback_wait(arp_cleanup_cb, netif) != ERR_OK) {
        ESP_LOGW(TAG, "arp forget: tcpip callback failed");
    }
}

typedef struct {
    esp_err_t err;
    bool retry_safe;
    bool link_started;
    uint8_t reason;
    bool rssi_valid;
    int8_t rssi;
} wifi_attempt_result_t;

static void wifi_format_rssi(bool valid, int8_t rssi, char *buf, size_t buflen)
{
    if (valid) {
        snprintf(buf, buflen, "%d dBm", (int)rssi);
    } else {
        snprintf(buf, buflen, "n/a");
    }
}

static bool pmk_hex_valid(const char *pmk_hex)
{
    if (pmk_hex == NULL || strnlen(pmk_hex, sizeof(s_pmk_hex)) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; i++) {
        const char c = pmk_hex[i];
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) {
            return false;
        }
    }
    return true;
}

/* ---------- netif / events ---------- */

static esp_err_t wifi_apply_dns(void)
{
    if (s_dns_main.ip.u_addr.ip4.addr != 0) {
        esp_err_t err = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &s_dns_main);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_dns_backup.ip.u_addr.ip4.addr != 0) {
        esp_err_t err = esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &s_dns_backup);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t wifi_stop_dhcpc(void)
{
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return ESP_OK;
    }
    return err;
}

/* stop DHCP → set IP → apply DNS → remember lease. Caller fills s_dns_* first. */
static esp_err_t wifi_commit_static_ip(const esp_netif_ip_info_t *ip_info)
{
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = wifi_stop_dhcpc();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_set_ip_info(s_netif, ip_info);
    if (err != ESP_OK) {
        return err;
    }
    err = wifi_apply_dns();
    if (err != ESP_OK) {
        return err;
    }
    s_ip_info = *ip_info;
    s_ip_ready = true;
    return ESP_OK;
}

/* Freeze the lease as a static address so the next association skips DHCP. */
static esp_err_t wifi_lock_dhcp_ip(const esp_netif_ip_info_t *ip_info)
{
    memset(&s_dns_main, 0, sizeof(s_dns_main));
    memset(&s_dns_backup, 0, sizeof(s_dns_backup));
    esp_err_t err = esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &s_dns_main);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_BACKUP, &s_dns_backup);
    if (err != ESP_OK) {
        return err;
    }
    return wifi_commit_static_ip(ip_info);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(s_events, WIFI_ASSOC_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_last_reason = event->reason;
        s_last_rssi = event->rssi;
        s_last_rssi_valid = true;
        xEventGroupClearBits(s_events, WIFI_ASSOC_BIT | WIFI_GOT_IP_BIT);
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_ip_info = event->ip_info;
        /* set_ip_info in wifi_lock_dhcp_ip posts another GOT_IP; keep the first. */
        if (s_ip_us == 0) {
            s_ip_us = esp_timer_get_time();
        }
        xEventGroupSetBits(s_events, WIFI_GOT_IP_BIT);
    }
}

/*
 * Write the STA config only when it actually differs from what the driver
 * already holds. The driver NVS is disabled, but a redundant reconfiguration
 * still needlessly resets internal connection state.
 */
static esp_err_t wifi_apply_sta_config(bool with_hints)
{
    wifi_config_t current = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &current) != ESP_OK) {
        memset(&current, 0, sizeof(current));
    }

    wifi_config_t wanted = current;
    memset(wanted.sta.ssid, 0, sizeof(wanted.sta.ssid));
    memset(wanted.sta.password, 0, sizeof(wanted.sta.password));
    /*
     * Both fields are length-based arrays with no room for a terminator at full
     * length. A 32-char SSID is legal; the PMK always fills all 64 bytes.
     */
    memcpy(wanted.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(wanted.sta.ssid)));
    memcpy(wanted.sta.password, s_pmk_hex, sizeof(wanted.sta.password));
    wanted.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wanted.sta.scan_method = WIFI_FAST_SCAN;

    if (with_hints) {
        wanted.sta.channel = s_cache.channel;
        wanted.sta.bssid_set = true;
        memcpy(wanted.sta.bssid, s_cache.bssid, sizeof(wanted.sta.bssid));
    } else {
        wanted.sta.channel = 0;
        wanted.sta.bssid_set = false;
        memset(wanted.sta.bssid, 0, sizeof(wanted.sta.bssid));
    }

    if (memcmp(&wanted, &current, sizeof(wanted)) == 0) {
        return ESP_OK;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &wanted);
}

static esp_err_t wifi_apply_cached_ip(void)
{
    memset(&s_dns_main, 0, sizeof(s_dns_main));
    memset(&s_dns_backup, 0, sizeof(s_dns_backup));
    s_dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    s_dns_main.ip.u_addr.ip4 = s_cache.dns_main;
    s_dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    s_dns_backup.ip.u_addr.ip4 = s_cache.dns_backup;

    return wifi_commit_static_ip(&s_cache.ip);
}

static esp_err_t wifi_tz_apply(void)
{
    /* libc localtime uses TZ; NTP still stores/sets UTC underneath. */
    if (setenv("TZ", WIFI_TZ_WARSAW, 1) != 0) {
        return ESP_ERR_NO_MEM;
    }
    tzset();
    return ESP_OK;
}

esp_err_t wifi_init(const char *ssid, const char *pmk_hex,
                    wifi_link_check_fn link_check)
{
    if (ssid == NULL || pmk_hex == NULL || link_check == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != WIFI_STATE_UNINIT) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t ssid_len = strnlen(ssid, sizeof(s_ssid));
    if (ssid_len == 0 || ssid_len > sizeof(s_ssid) - 1 ||
        !pmk_hex_valid(pmk_hex)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_tz_apply();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "timezone Warsaw: %s", esp_err_to_name(err));
        return err;
    }

    const int64_t t0 = esp_timer_get_time();
    err = ESP_OK;
    bool event_loop_owned = false;
    bool wifi_ready = false;
    bool wifi_started = false;
    bool wifi_handler_ready = false;
    bool ip_handler_ready = false;

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_link_check = link_check;

    memcpy(s_ssid, ssid, ssid_len);
    s_ssid[ssid_len] = '\0';
    for (size_t i = 0; i < 64; i++) {
        const char c = pmk_hex[i];
        s_pmk_hex[i] = (c >= 'A' && c <= 'F') ? (char)(c + ('a' - 'A')) : c;
    }
    s_pmk_hex[64] = '\0';

    const uint8_t ssid_len_u8 = (uint8_t)ssid_len;
    const uint8_t pmk_len_u8 = 64;
    s_profile_hash = esp_rom_crc32_le(0, &ssid_len_u8, sizeof(ssid_len_u8));
    s_profile_hash = esp_rom_crc32_le(s_profile_hash,
                                      (const uint8_t *)s_ssid,
                                      ssid_len);
    s_profile_hash = esp_rom_crc32_le(s_profile_hash,
                                      &pmk_len_u8,
                                      sizeof(pmk_len_u8));
    s_profile_hash = esp_rom_crc32_le(s_profile_hash,
                                      (const uint8_t *)s_pmk_hex,
                                      64);

    /* Network profile lives in LP SRAM only; a power loss costs one full path. */
    s_arp_restored = false;
    s_use_cache = cache_load(s_profile_hash, &s_cache);

    /* PHY cal_data lives in NVS; never erase or deinit — the host may own other namespaces. */
    err = nvs_flash_init();
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        event_loop_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /*
     * The driver has nothing left worth persisting: the PMK arrives ready-made
     * because wifi_init() is given the PSK in hex, and the BSSID/channel hints
     * live in our own profile. Turning its NVS off removes the sta.pwr_reset
     * write pair it performed on every connect/disconnect.
     */
    cfg.nvs_enable = false;
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        goto fail;
    }
    wifi_ready = true;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        goto fail;
    }

    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTO_BITMAP,
        .ghz_5g = 0,
    };
    err = esp_wifi_set_protocols(WIFI_IF_STA, &protocols);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_wifi_config_11b_rate(WIFI_IF_STA, true);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    wifi_handler_ready = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (err != ESP_OK) {
        goto fail;
    }
    ip_handler_ready = true;

    err = wifi_apply_sta_config(s_use_cache);
    if (err != ESP_OK) {
        goto fail;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        goto fail;
    }
    wifi_started = true;

    /* set_protocols(ghz_5g = 0) leaves the 5 GHz band enabled: without this
     * the full-path scan sweeps 5 GHz too (passive dwell on DFS channels).
     * The API needs a started driver, hence after esp_wifi_start(). */
    err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (err != ESP_OK) {
        goto fail;
    }

    if (s_use_cache) {
        err = wifi_apply_cached_ip();
        if (err != ESP_OK) {
            goto fail;
        }
        ESP_LOGI(TAG, "cache hit: IP " IPSTR " ch %u bssid %02x:%02x:%02x:%02x:%02x:%02x",
                 IP2STR(&s_cache.ip.ip), s_cache.channel,
                 s_cache.bssid[0], s_cache.bssid[1], s_cache.bssid[2],
                 s_cache.bssid[3], s_cache.bssid[4], s_cache.bssid[5]);
    }

    s_state = WIFI_STATE_READY;
    ESP_LOGI(TAG, "init total %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "wifi_init failed: %s", esp_err_to_name(err));

    /*
     * Track every rollback step: only a complete teardown makes a retry safe.
     * esp_netif_init() has no counterpart in IDF 6.0 and is idempotent, and the
     * global NVS belongs to the host application, so neither is undone here.
     */
    bool rolled_back = true;
    if (wifi_started && esp_wifi_stop() != ESP_OK) {
        rolled_back = false;
    }
    if (ip_handler_ready &&
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler) != ESP_OK) {
        rolled_back = false;
    }
    if (wifi_handler_ready &&
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler) != ESP_OK) {
        rolled_back = false;
    }
    if (wifi_ready && esp_wifi_deinit() != ESP_OK) {
        rolled_back = false;
    }
    if (s_netif != NULL) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    if (event_loop_owned && esp_event_loop_delete_default() != ESP_OK) {
        rolled_back = false;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    memset(&s_dns_main, 0, sizeof(s_dns_main));
    memset(&s_dns_backup, 0, sizeof(s_dns_backup));
    memset(&s_cache, 0, sizeof(s_cache));
    s_ip_ready = false;
    s_ever_connected = false;
    s_use_cache = false;
    s_arp_restored = false;
    s_hints_pending = false;
    s_force_full = false;

    if (rolled_back) {
        s_state = WIFI_STATE_UNINIT;
    } else {
        s_state = WIFI_STATE_BROKEN;
        ESP_LOGE(TAG, "rollback incomplete, module unusable until device reset");
    }
    return err;
}

static esp_err_t wifi_wait_link(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_GOT_IP_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_DISCONNECTED_BIT) {
        return ESP_ERR_WIFI_CONN;
    }
    return ESP_ERR_TIMEOUT;
}

/*
 * Make sure a timed-out connection attempt is fully stopped before changing
 * STA configuration or starting another attempt. A disconnect event observed
 * by wifi_wait_link() already means the driver is idle.
 */
static esp_err_t wifi_abort_attempt(void)
{
    EventBits_t previous = xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    if (previous & WIFI_DISCONNECTED_BIT) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_DISCONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT_MS));
    return (bits & WIFI_DISCONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static wifi_attempt_result_t wifi_attempt(void)
{
    wifi_attempt_result_t result = {
        .err = ESP_OK,
        .retry_safe = true,
        .link_started = false,
        .reason = 0,
        .rssi_valid = false,
        .rssi = 0,
    };

    s_last_reason = 0;
    s_last_rssi = 0;
    s_last_rssi_valid = false;
    s_ip_us = 0;
    xEventGroupClearBits(s_events, WIFI_ASSOC_BIT | WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);

    if (s_ip_ready) {
        /* netif validates the static address when the link comes up */
        result.err = esp_netif_set_ip_info(s_netif, &s_ip_info);
        if (result.err != ESP_OK) {
            return result;
        }
        /* esp_netif_set_ip_info() on a netif with stopped DHCP clears the
         * global lwIP DNS servers (dns_clear_servers); put ours back. */
        result.err = wifi_apply_dns();
        if (result.err != ESP_OK) {
            return result;
        }
    }

    result.err = esp_wifi_connect();
    if (result.err != ESP_OK) {
        return result;
    }
    result.link_started = true;

    result.err = wifi_wait_link();
    if (result.err == ESP_OK && !s_ip_ready) {
        result.err = wifi_lock_dhcp_ip(&s_ip_info);
    }
    /* Snapshot before cleanup: esp_wifi_disconnect() can emit STA_LEAVING and
     * overwrite reason/rssi, which would misclassify a timeout as stale deauth. */
    result.reason = s_last_reason;
    result.rssi_valid = s_last_rssi_valid;
    result.rssi = s_last_rssi;

    if (result.err != ESP_OK) {
        esp_err_t abort_err = wifi_abort_attempt();
        if (abort_err != ESP_OK) {
            ESP_LOGW(TAG, "abort failed attempt: %s", esp_err_to_name(abort_err));
            result.retry_safe = false;
        }
    }
    return result;
}

/*
 * A late deauth from the session we just left can abort the new attempt. That
 * says nothing about the cache, so it earns one more cached try instead of an
 * immediate drop to DHCP.
 */
static bool wifi_reason_is_stale_session(uint8_t reason)
{
    return reason == WIFI_REASON_STA_LEAVING ||
           reason == WIFI_REASON_ASSOC_LEAVE ||
           reason == WIFI_REASON_AUTH_LEAVE;
}

/*
 * Back to a plain DHCP association: no BSSID/channel hints, no frozen lease.
 * The transition mutates module state before it can finish, so any failure
 * re-arms s_force_full: the next wifi_connect() must run this again instead of
 * associating with a half-applied configuration.
 */
static esp_err_t wifi_enter_full_path(void)
{
    s_use_cache = false;
    s_ip_ready = false;
    memset(&s_dns_main, 0, sizeof(s_dns_main));
    memset(&s_dns_backup, 0, sizeof(s_dns_backup));

    arp_forget();

    esp_err_t err = wifi_apply_sta_config(false);
    if (err != ESP_OK) {
        s_force_full = true;
        return err;
    }

    err = esp_netif_dhcpc_start(s_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        s_force_full = true;
    }
    return err;
}

static void wifi_store_cache(const wifi_ap_record_t *ap)
{
    wifi_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    cache.hdr.profile_hash = s_profile_hash;
    cache.ip = s_ip_info;
    cache.dns_main = s_dns_main.ip.u_addr.ip4;
    cache.dns_backup = s_dns_backup.ip.u_addr.ip4;
    memcpy(cache.bssid, ap->bssid, sizeof(cache.bssid));
    cache.channel = ap->primary;

    cache_save(&cache);
    s_cache = cache;
    /* Applied on the next attempt: esp_wifi_set_config() is rejected while the
     * station is associated. */
    s_hints_pending = true;
}

static void wifi_arp_invalidate(void)
{
    arp_forget();
    ESP_LOGW(TAG, "arp cache invalidated");
}

static void wifi_cache_invalidate(void)
{
    arp_forget();
    memset(&s_rtc_cache, 0, sizeof(s_rtc_cache));
    memset(&s_cache, 0, sizeof(s_cache));
    s_use_cache = false;
    s_force_full = true;
    ESP_LOGW(TAG, "cache invalidated");
}

/* ---------- SNTP: full path, missing time, or once per day ---------- */

static bool ntp_record_valid(const wifi_rtc_ntp_t *n)
{
    return rtc_hdr_ok(n, sizeof(*n), NTP_MAGIC, NTP_VERSION, 0) &&
           n->last_success_unix >= WIFI_NTP_SANITY_UNIX;
}

static void ntp_save_success(time_t now)
{
    wifi_rtc_ntp_t n;
    memset(&n, 0, sizeof(n));
    n.hdr.profile_hash = 0;
    n.last_success_unix = (int64_t)now;
    rtc_seal(&n, sizeof(n), NTP_MAGIC, NTP_VERSION);
    s_rtc_ntp = n;
}

static bool wifi_time_ok(void)
{
    time_t now = 0;
    time(&now);
    return now >= (time_t)WIFI_NTP_SANITY_UNIX;
}

static bool wifi_ntp_needed(bool assoc_full)
{
    if (!wifi_time_ok()) {
        return true;
    }
    if (assoc_full) {
        return true;
    }
    /* Stale/missing LP SRAM record with a live RTC clock (OTA, corruption):
     * refresh so the daily cadence has a trusted anchor again. */
    if (!ntp_record_valid(&s_rtc_ntp)) {
        return true;
    }

    time_t now = 0;
    time(&now);
    return (now - (time_t)s_rtc_ntp.last_success_unix) >= WIFI_NTP_INTERVAL_S;
}

static esp_err_t wifi_ntp_try_server(const char *server)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        /* init may leave partial state; deinit is always safe. */
        esp_netif_sntp_deinit();
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(WIFI_NTP_SYNC_TIMEOUT_MS));
    esp_netif_sntp_deinit();
    return err;
}

static void wifi_ntp_sync(void)
{
    static const char *const servers[] = {
        WIFI_NTP_SERVER_PRIMARY,
        WIFI_NTP_SERVER_BACKUP,
    };

    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++) {
        const esp_err_t err = wifi_ntp_try_server(servers[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ntp %s: %s", servers[i], esp_err_to_name(err));
            continue;
        }

        time_t now = 0;
        time(&now);
        ntp_save_success(now);

        struct tm tm_local;
        char buf[32];
        if (localtime_r(&now, &tm_local) == NULL ||
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local) == 0) {
            ESP_LOGI(TAG, "ntp synced via %s (Warsaw time unavailable)", servers[i]);
        } else {
            ESP_LOGI(TAG, "ntp synced via %s Warsaw %s", servers[i], buf);
        }
        return;
    }

    ESP_LOGW(TAG, "ntp sync failed");
}

/*
 * The module only sees association and a lease, never whether packets arrive.
 * The application's check closes that gap, and the repair escalates from cheap
 * to expensive: gateway ARP first, the whole profile second.
 *
 * Both steps are conditional, because clearing data the cycle never used costs
 * energy without fixing anything: a cycle that took the full DHCP path has no
 * restored ARP entry to drop, and its profile was written moments ago by
 * wifi_store_cache(), so dropping it would only buy a second full path.
 *
 * Runs after SNTP: wall-clock is required for application traffic, so the
 * module must not call link_check while time is still unset.
 */
static void wifi_repair_after_link_check(void)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_FIRST_TRAFFIC_DELAY_MS));

    if (s_link_check() != WIFI_LINK_UNREACHABLE) {
        return;
    }
    if (!s_arp_restored) {
        ESP_LOGW(TAG, "traffic unreachable on a fresh link; nothing cached to repair");
        return;
    }

    wifi_arp_invalidate();
    vTaskDelay(pdMS_TO_TICKS(WIFI_FIRST_TRAFFIC_DELAY_MS));

    if (s_link_check() != WIFI_LINK_UNREACHABLE) {
        return;
    }
    if (!s_use_cache) {
        ESP_LOGW(TAG, "traffic still unreachable; profile is fresh, keeping it");
        return;
    }
    wifi_cache_invalidate();
}

esp_err_t wifi_connect(void)
{
    if (s_state != WIFI_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool reconnect = s_ever_connected;
    const int64_t t_entry_us = esp_timer_get_time();
    char rssi_txt[16];

    if (s_force_full) {
        esp_err_t reset = wifi_enter_full_path();
        s_hints_pending = false;
        if (reset != ESP_OK) {
            /* wifi_enter_full_path() re-armed s_force_full for the next call. */
            return reset;
        }
        s_force_full = false;
    } else if (s_hints_pending) {
        esp_err_t hints = wifi_apply_sta_config(true);
        s_hints_pending = false;
        if (hints != ESP_OK) {
            ESP_LOGW(TAG, "cache hints: %s", esp_err_to_name(hints));
        } else {
            s_use_cache = true;
        }
    }

    const bool cached = s_use_cache;

    s_t0_us = esp_timer_get_time();

    wifi_attempt_result_t attempt = wifi_attempt();

    /* 1) Late deauth from our own disconnect — says nothing about the cache. */
    if (attempt.err != ESP_OK && attempt.retry_safe && attempt.link_started && cached &&
        wifi_reason_is_stale_session(attempt.reason)) {
        wifi_format_rssi(attempt.rssi_valid, attempt.rssi, rssi_txt, sizeof(rssi_txt));
        ESP_LOGW(TAG, "stale deauth (reason %u, rssi %s), cached retry",
                 attempt.reason, rssi_txt);
        vTaskDelay(pdMS_TO_TICKS(WIFI_STALE_RETRY_DELAY_MS));
        s_t0_us = esp_timer_get_time();
        attempt = wifi_attempt();
    }

    /* 2) Cache may be stale — one full DHCP attempt without hints. */
    if (attempt.err != ESP_OK && attempt.retry_safe && attempt.link_started && cached) {
        wifi_format_rssi(attempt.rssi_valid, attempt.rssi, rssi_txt, sizeof(rssi_txt));
        ESP_LOGW(TAG, "cached attempt failed (%s, reason %u, rssi %s), full retry",
                 esp_err_to_name(attempt.err), attempt.reason, rssi_txt);
        esp_err_t path = wifi_enter_full_path();
        if (path == ESP_OK) {
            s_t0_us = esp_timer_get_time();
            attempt = wifi_attempt();
        } else {
            attempt.err = path;
        }
    }

    if (attempt.err != ESP_OK) {
        wifi_format_rssi(attempt.rssi_valid, attempt.rssi, rssi_txt, sizeof(rssi_txt));
        ESP_LOGE(TAG, "%s failed: %s (reason %u, rssi %s%s) after %" PRId64 " ms",
                 reconnect ? "reconnect" : "connect", esp_err_to_name(attempt.err),
                 attempt.reason, rssi_txt,
                 attempt.retry_safe ? "" : "; driver not idle",
                 (esp_timer_get_time() - t_entry_us) / 1000);
        return attempt.err;
    }

    s_ever_connected = true;

    wifi_ap_record_t ap = {0};
    bool have_ap = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    bool have_rssi = have_ap && ap.rssi < 0 && ap.rssi != INT8_MIN;
    if (have_ap && !s_use_cache) {
        wifi_store_cache(&ap);
    }

    s_arp_restored = (arp_restore_to_lwip() == ESP_OK);

    wifi_format_rssi(have_rssi, ap.rssi, rssi_txt, sizeof(rssi_txt));
    /* Snapshot before repair: wifi_cache_invalidate() clears s_use_cache and
     * would otherwise look like a full association path for the NTP gate. */
    const bool assoc_full = !s_use_cache;

    ESP_LOGI(TAG, "%s %s SSID '%s' IP " IPSTR " %" PRId64 " ms rssi %s arp %s",
             reconnect ? "reconnect" : "connect",
             assoc_full ? "full" : "cached",
             s_ssid, IP2STR(&s_ip_info.ip),
             (s_ip_us - s_t0_us) / 1000,
             rssi_txt,
             s_arp_restored ? "cached" : "fresh");

    /* Time before application traffic: link_check may depend on wall-clock. */
    if (wifi_ntp_needed(assoc_full)) {
        wifi_ntp_sync();
    }
    if (!wifi_time_ok()) {
        ESP_LOGW(TAG, "application traffic skipped: system time unset");
        return ESP_OK;
    }
    wifi_repair_after_link_check();
    return ESP_OK;
}

esp_err_t wifi_get_debug_state(wifi_debug_state_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (s_state != WIFI_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    out->cache_used = s_use_cache;
    out->arp_restored = s_arp_restored;
    return ESP_OK;
}

esp_err_t wifi_disconnect(void)
{
    if (s_state != WIFI_STATE_READY) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Learn gateway MAC before the link (and ARP table) goes away. */
    if ((xEventGroupGetBits(s_events) & WIFI_ASSOC_BIT) != 0) {
        arp_learn_from_lwip();
    } else {
        xEventGroupSetBits(s_events, WIFI_DISCONNECTED_BIT);
        return ESP_OK;
    }

    xEventGroupClearBits(s_events, WIFI_DISCONNECTED_BIT);
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_DISCONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_DISCONNECT_TIMEOUT_MS));
    return (bits & WIFI_DISCONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

