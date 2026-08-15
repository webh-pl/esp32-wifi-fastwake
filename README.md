[Polski](README_PL.md)

# esp32-wifi-fastwake

A WiFi station component (ESP-IDF, plain C) for battery-powered ESP32 devices that spend most of their life asleep: wake up, connect, send a few packets, disconnect, go back to deep sleep.

## The problem it solves

On a battery device the radio is the most expensive part of every wake cycle. A regular WiFi connection — scan, associate, DHCP — takes 1–2 seconds with the radio drawing full current the whole time. If the device wakes every few minutes, connecting costs more energy than the actual data transfer.

This module remembers everything about the last successful connection — IP address, gateway, DNS, the access point's BSSID and channel, even the gateway's MAC address — in LP SRAM, a small memory region that survives deep sleep and resets. On the next wake it skips the scan and DHCP entirely and reconnects straight to the known access point.

The result, measured on a Seeed XIAO ESP32-C5:

| Path | Time to a usable link |
| --- | --- |
| First connection (full DHCP) | 1.2–2.3 s |
| Reconnection from cache | **~90 ms** |

Nothing is ever written to flash, so the cache adds no flash wear. The price: a power cut wipes LP SRAM and the next connection is a full DHCP one — for a device that loses power rarely, that is a one-time cost.

## What the module handles for you

- **Network profile cache** in LP SRAM, validated with CRC on every boot, invalidated automatically when it stops working.
- **Gateway ARP pre-fill** — the first packet after connecting doesn't have to wait for an ARP round trip.
- **Time sync (SNTP)** — runs inside `wifi_connect()` when the clock is missing or once a day, so your application always sees a sane wall clock.
- **Self-repair** — if traffic stops getting through, the module first forgets the ARP entry, then the whole profile, and falls back to a fresh DHCP connection. Your code never has to manage the cache.

## Quick start

Your whole networking code is three calls and one callback:

```c
#include "wifi.h"
#include "secrets.h"   /* WIFI_SSID and WIFI_PMK_HEX — see below */

/* Your traffic (MQTT publish, HTTP POST, ping...). Its result tells the
 * module whether the cached network data still works. */
static wifi_link_result_t app_link_check(void)
{
    if (!send_measurements_possible()) {
        return WIFI_LINK_CHECK_FAILED;   /* local problem: don't blame the network */
    }
    return send_measurements() ? WIFI_LINK_OK : WIFI_LINK_UNREACHABLE;
}

void app_main(void)
{
    if (wifi_init(WIFI_SSID, WIFI_PMK_HEX, app_link_check) != ESP_OK) {
        esp_deep_sleep(SLEEP_US);   /* deep sleep is the cheapest reset */
    }

    /* Associates, syncs time if needed, runs app_link_check(),
     * repairs the cache when the verdict says it is stale. */
    (void)wifi_connect();

    wifi_disconnect();              /* always before deep sleep */
    esp_deep_sleep(SLEEP_US);
}
```

Notes:

- `wifi_connect()` returns `ESP_OK` when the link is up; a failed link check does not change the result — it invalidates the cache for the next cycle instead.
- Call `wifi_disconnect()` before deep sleep. It is the only moment the module can learn the gateway MAC for the ARP cache.
- Don't wrap the calls in `ESP_ERROR_CHECK`; every function returns `esp_err_t` and never aborts. Whether an error means panic or "try again next wake" is your decision.

## How the link check works

The module deliberately does not generate its own traffic — you already send data every cycle, so your traffic doubles as the health check. Your callback reports one of three verdicts:

| Verdict | Meaning | What the module does |
| --- | --- | --- |
| `WIFI_LINK_OK` | traffic got through | nothing |
| `WIFI_LINK_UNREACHABLE` | the check ran, packets didn't arrive | repairs the ARP entry, on repeat the whole profile |
| `WIFI_LINK_CHECK_FAILED` | the check itself couldn't run (socket error, no memory) | nothing — a local bug must not cost the network cache |

The callback is mandatory: without it the module would have no way to notice that a cached IP or gateway MAC went stale. It runs inside `wifi_connect()`, so it must not call any `wifi_*` function itself.

## Credentials: secrets.h and the PMK

`WIFI_SSID` and `WIFI_PMK_HEX` are your application's business — the module only receives them as `wifi_init()` arguments. The usual pattern is a small header kept out of version control. This repo ships a template, [`secrets.h.example`](secrets.h.example):

```bash
cp managed_components/esp32-wifi-fastwake/secrets.h.example main/secrets.h
echo "main/secrets.h" >> .gitignore   # never commit real credentials
```

The module takes the network key as a 64-character hex PMK, not a plain-text passphrase. Deriving a PMK from a passphrase (PBKDF2, 4096 SHA-1 rounds) is expensive, and with the driver's flash storage disabled the device would silently redo it on every single boot. Compute it once on your computer instead, for this exact SSID:

```bash
python3 -c 'import hashlib;print(hashlib.pbkdf2_hmac("sha1",b"PASSPHRASE",b"SSID",4096,32).hex())'
```

Upper- and lowercase hex are both accepted and treated as the same key.

## Required project configuration

Add to your project's `sdkconfig.defaults`:

```ini
# PHY calibration cache in NVS — without it every wake recalibrates the radio
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y
CONFIG_ESP_PHY_RF_CAL_PARTIAL=y
# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set

# Static ARP entries (in ESP-IDF they are tied to the DHCP-server option,
# even though the device stays a station)
CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y

# A random SNTP start delay would break the short time-sync window
# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set

# Keep the wall clock across deep sleep
CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y
```

You can't get this silently wrong: `wifi.c` stops the build with `#error` when the ARP or SNTP options don't match.

The module calls `nvs_flash_init()` itself (the radio calibration cache needs it) and never erases or deinitialises your NVS partition. It makes no assumptions about your partition layout.

## Adding it to a project

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=6.0"
  esp32-wifi-fastwake:
    git: https://github.com/webh-pl/esp32-wifi-fastwake.git
    version: v1.0.0
```

For a local checkout sitting next to your project (`path` is relative to `main/`):

```yaml
dependencies:
  idf: ">=6.0"
  esp32-wifi-fastwake:
    path: ../../esp32-wifi-fastwake
```

Then `#include "wifi.h"` and build as usual — the component manager fetches everything during `idf.py build`.

## Good to know / limits

- **One task at a time.** The module is not reentrant and has no mutex; call it from a single task.
- **One network.** SSID and PMK are given once in `wifi_init()`. Changing them invalidates the cache.
- **2.4 GHz, 802.11 g/n only.** No 5 GHz and no band roaming.
- **Stable IP expected.** The cached path does not renew the DHCP lease — reserve the device's IP by MAC on your router.
- **BSSID pinning.** The cache points at one specific access point. In a multi-AP network, after the AP changes, the first attempt fails and falls back to a full connection (one slower cycle).
- **Power loss clears everything.** Ordinary resets (watchdog, `esp_restart()`, USB) keep the cache; only cutting power loses it.
- **No timeouts around your callback.** A hung `link_check` hangs `wifi_connect()` — keep your traffic bounded.

## Requirements and license

- ESP-IDF **6.0 or newer**; developed and measured on ESP32-C5 (Seeed XIAO, IDF v6.0.2). The code uses LP SRAM (`RTC_NOINIT_ATTR`); other chips with that memory should work but are unverified.
- WPA2-PSK network, 2.4 GHz.
- License: [MIT](LICENSE).
