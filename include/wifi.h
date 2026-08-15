#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool cache_used;
    bool arp_restored;
} wifi_debug_state_t;

/**
 * Verdict of the application traffic check wifi_connect() runs once the link
 * carries an IP. Only UNREACHABLE escalates: a check that could not run says
 * nothing about the network, so it must not cost a cached profile.
 */
typedef enum {
    WIFI_LINK_OK = 0,
    WIFI_LINK_UNREACHABLE,
    WIFI_LINK_CHECK_FAILED,
} wifi_link_result_t;

/**
 * Application traffic check called from inside wifi_connect(). Must not call
 * any public wifi_* function: the module is neither reentrant nor mutexed.
 */
typedef wifi_link_result_t (*wifi_link_check_fn)(void);

/**
 * Init netif/WiFi STA, fix the radio to 2.4 GHz 802.11g/n, start the driver.
 * Reuses an already initialized esp-netif stack/default event loop, but owns
 * the default STA netif it creates.
 * Loads the network cache from LP SRAM; nothing the module owns lives in flash,
 * so a power loss costs one full DHCP path. Calls nvs_flash_init() internally
 * (no app-side init) because esp_phy needs it for its calibration cache, but
 * never erases or deinitialises global NVS owned by the host application.
 *
 * pmk_hex must be the 64-hex-character WPA2 PMK for this SSID. The module
 * rejects passphrases: the WiFi driver's NVS is disabled, so accepting one
 * would silently repeat PBKDF2-SHA1 on every boot.
 *
 * link_check is mandatory (NULL gives ESP_ERR_INVALID_ARG). Without it the
 * module could never tell that a restored static ARP entry or a cached IP went
 * stale, so it would have no way left to repair either.
 *
 * Returns ESP_ERR_INVALID_STATE when already initialized, and also after a
 * failed init whose rollback could not fully undo the global driver state; in
 * that case the device must be reset. Every other failure leaves the module
 * uninitialized and safe to retry.
 */
esp_err_t wifi_init(const char *ssid, const char *pmk_hex,
                    wifi_link_check_fn link_check);

/**
 * Associate and wait for IP. A failed cached attempt falls back to one full
 * DHCP attempt without hints; the network cache is refreshed after that attempt
 * succeeds. TX power is not adapted — the radio stays at the driver default.
 *
 * Once the link carries an IP, optional SNTP runs first (missing time, full
 * association path, or once per day) so application traffic sees a wall-clock.
 * Then the link_check given to wifi_init() decides whether the cached network
 * data still works, and this function repairs it itself: gateway ARP first,
 * then the whole profile. Returns ESP_OK whenever the link is up, because
 * unreachable traffic invalidates the cache for the next cycle without
 * breaking the current session. Its total duration therefore includes up to
 * two short SNTP waits and up to two link_check runs. A failed NTP sync is
 * logged and does not change the return value; if the system still has no
 * sensible time afterward, link_check is skipped for that cycle.
 */
esp_err_t wifi_connect(void);

/**
 * Learn the gateway MAC into LP SRAM when missing, then disconnect and wait
 * until the link is down. No-op when already down.
 */
esp_err_t wifi_disconnect(void);

/**
 * Read-only snapshot of the path taken. Does not change module behaviour.
 * Zeroes *out and reports ESP_ERR_INVALID_STATE when the module is not
 * initialized, so callers never read stale values by accident.
 *
 * cache_used is false both on a cold start and after a fallback to the full
 * path, which is exactly when the cycle paid for a DHCP round.
 */
esp_err_t wifi_get_debug_state(wifi_debug_state_t *out);

#ifdef __cplusplus
}
#endif
