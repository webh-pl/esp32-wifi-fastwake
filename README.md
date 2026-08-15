[Polski](README_PL.md)

# esp32-wifi-sta

ESP-IDF STA WiFi for short battery sessions on ESP32-C5: wake → associate → send → disconnect → sleep.

The module owns the fast path. Network profile, gateway ARP, and SNTP live in LP SRAM; the first join pays DHCP (~1.2–2.3 s), later reconnects target the cached BSSID and channel (~90 ms). The application calls `wifi_init` / `wifi_connect` / `wifi_disconnect` and supplies a `link_check` callback once. The module decides whether the cache is still valid.

Requires ESP-IDF ≥ 6.0. Verified on ESP32-C5 (eco2, v6.0.2). License: MIT.

## API

```c
typedef enum {
    WIFI_LINK_OK = 0,
    WIFI_LINK_UNREACHABLE,
    WIFI_LINK_CHECK_FAILED,
} wifi_link_result_t;

typedef wifi_link_result_t (*wifi_link_check_fn)(void);

esp_err_t wifi_init(const char *ssid, const char *pmk_hex,
                    wifi_link_check_fn link_check);
esp_err_t wifi_connect(void);
esp_err_t wifi_disconnect(void);
esp_err_t wifi_get_debug_state(wifi_debug_state_t *out);
```

Every public function returns `esp_err_t` and never aborts the process. Calls before a successful `wifi_init()` return `ESP_ERR_INVALID_STATE`. Cache invalidation is not public: `wifi_connect()` does it from the `link_check` verdict.

`link_check` is mandatory (`NULL` → `ESP_ERR_INVALID_ARG`). It must not call any `wifi_*` function. Three verdicts:

| Verdict | Meaning | Module reaction |
| --- | --- | --- |
| `WIFI_LINK_OK` | traffic got through | none |
| `WIFI_LINK_UNREACHABLE` | check ran, traffic did not arrive | repair ARP, then the whole profile |
| `WIFI_LINK_CHECK_FAILED` | check could not run (socket, config, OOM) | none — a local failure must not cost the cache |

Always call `wifi_disconnect()` before deep sleep: that is the only moment the module can learn the gateway MAC from lwIP.

### Minimal integration

```c
static wifi_link_result_t app_link_check(void)
{
    if (!send_measurements_possible()) {
        return WIFI_LINK_CHECK_FAILED;
    }
    return send_measurements() ? WIFI_LINK_OK : WIFI_LINK_UNREACHABLE;
}

if (wifi_init(WIFI_SSID, WIFI_PMK_HEX, app_link_check) != ESP_OK) {
    esp_deep_sleep(...);
}

(void)wifi_connect();
wifi_disconnect();
esp_deep_sleep(...);
```

Do not wrap these calls in `ESP_ERROR_CHECK`. Panic-or-retry belongs to the application.

## PMK, not a passphrase

`pmk_hex` must be exactly 64 hex characters. Passphrases are rejected: the WiFi driver's NVS is off, so accepting one would silently rerun PBKDF2-SHA1 (4096 iterations) on every boot. Derive the PMK once on the host for this exact SSID:

```bash
python3 -c 'import hashlib;print(hashlib.pbkdf2_hmac("sha1",b"PASSPHRASE",b"SSID",4096,32).hex())'
```

Uppercase hex is accepted and folded to lowercase before use, so `"ABCD…"` and `"abcd…"` keep the same LP SRAM `profile_hash`.

## Required sdkconfig

Put this in the consuming project's `sdkconfig.defaults`. `wifi.c` fails the build with `#error` if static ARP or SNTP startup delay is wrong:

```ini
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y
CONFIG_ESP_PHY_RF_CAL_PARTIAL=y
# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set

CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y

# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set

CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y
```

`CONFIG_ESP_WIFI_NVS_ENABLED` may stay `y`; the module turns the driver NVS off with `cfg.nvs_enable = false` in `wifi_init()`. The module calls `nvs_flash_init()` for PHY calibration and never erases or deinitialises the host partition. A custom NVS size is the application's choice — this library does not assume a partition layout.

## Use in another project

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=6.0"
  esp32-wifi-sta:
    git: https://github.com/webh-pl/esp32-wifi-sta.git
    version: v1.0.0
```

For local development next to the consumer:

```yaml
dependencies:
  idf: ">=6.0"
  esp32-wifi-sta:
    git: https://github.com/webh-pl/esp32-wifi-sta.git
    version: v1.0.0
    override_path: ../esp32-wifi-sta
```

Then `#include "wifi.h"`.

## Limits

- One task at a time: the module is neither reentrant nor mutexed.
- One network profile. Changing SSID or PMK drops the LP SRAM cache (NTP record stays).
- 2.4 GHz 802.11g/n only. No 5 GHz, no band roaming.
- Cached path does not renew the DHCP lease. Reserve the IP by MAC on the router.
- Power loss wipes LP SRAM: the next join is a full DHCP. Resets that are not a power cut keep the cache.
- `link_check` and lwIP callbacks have no timeout inside the module. A hung callback hangs `wifi_connect()`.
