[English](README.md)

# esp32-wifi-sta

STA WiFi w ESP-IDF na krótkie sesje z baterii na ESP32-C5: wybudzenie → asocjacja → transmisja → rozłączenie → sen.

Moduł sam prowadzi szybką ścieżkę. Profil sieci, ARP bramy i SNTP żyją w LP SRAM; pierwsze połączenie płaci DHCP (~1,2–2,3 s), kolejne celują w zapisany BSSID i kanał (~90 ms). Aplikacja woła `wifi_init` / `wifi_connect` / `wifi_disconnect` i raz podaje callback `link_check`. O tym, czy cache nadal działa, decyduje moduł.

Wymaga ESP-IDF ≥ 6.0. Sprawdzone na ESP32-C5 (eco2, v6.0.2). Licencja: MIT.

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

Każda funkcja publiczna zwraca `esp_err_t` i nigdy nie przerywa procesu. Wołane przed udanym `wifi_init()` dają `ESP_ERR_INVALID_STATE`. Unieważnianie cache nie jest publiczne: robi to `wifi_connect()` na podstawie werdyktu `link_check`.

`link_check` jest obowiązkowy (`NULL` → `ESP_ERR_INVALID_ARG`). Nie może wołać żadnej funkcji `wifi_*`. Trzy werdykty:

| Werdykt | Znaczenie | Reakcja modułu |
| --- | --- | --- |
| `WIFI_LINK_OK` | ruch przeszedł | brak |
| `WIFI_LINK_UNREACHABLE` | sprawdzenie się wykonało, ruch nie doszedł | naprawa ARP, potem całego profilu |
| `WIFI_LINK_CHECK_FAILED` | sprawdzenia nie dało się wykonać (gniazdo, konfiguracja, brak pamięci) | brak — lokalna awaria nie może kosztować cache |

Zawsze wołaj `wifi_disconnect()` przed deep sleep: to jedyny moment, w którym moduł może odczytać MAC bramy z lwIP.

### Minimalna integracja

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

Nie owijaj tych wywołań w `ESP_ERROR_CHECK`. Decyzja „panika czy kolejny cykl” należy do aplikacji.

## PMK, nie passphrase

`pmk_hex` musi mieć dokładnie 64 znaki hex. Passphrase jest odrzucana: sterownik WiFi nie ma NVS, więc jej przyjęcie oznaczałoby ciche powtarzanie PBKDF2-SHA1 (4096 iteracji) przy każdym boocie. PMK wylicza się raz na hoście, dla tego konkretnego SSID:

```bash
python3 -c 'import hashlib;print(hashlib.pbkdf2_hmac("sha1",b"PASSPHRASE",b"SSID",4096,32).hex())'
```

Wielkie litery hex są akceptowane i sprowadzane do małych, więc `"ABCD…"` i `"abcd…"` dają ten sam `profile_hash` w LP SRAM.

## Wymagany sdkconfig

Wstaw to do `sdkconfig.defaults` projektu-konsumenta. `wifi.c` wywala build przez `#error`, gdy statyczny ARP albo opóźnienie startu SNTP są źle ustawione:

```ini
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y
CONFIG_ESP_PHY_RF_CAL_PARTIAL=y
# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set

CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y

# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set

CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y
```

`CONFIG_ESP_WIFI_NVS_ENABLED` może zostać na `y`; moduł wyłącza NVS sterownika przez `cfg.nvs_enable = false` w `wifi_init()`. Woła `nvs_flash_init()` na potrzeby kalibracji PHY i nigdy nie kasuje ani nie deinicjalizuje partycji hosta. Rozmiar NVS to decyzja aplikacji — biblioteka nie zakłada układu partycji.

## Użycie w innym projekcie

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=6.0"
  esp32-wifi-sta:
    git: https://github.com/webh-pl/esp32-wifi-sta.git
    version: v1.0.0
```

Przy rozwoju lokalnym obok konsumenta:

```yaml
dependencies:
  idf: ">=6.0"
  esp32-wifi-sta:
    git: https://github.com/webh-pl/esp32-wifi-sta.git
    version: v1.0.0
    override_path: ../esp32-wifi-sta
```

Potem `#include "wifi.h"`.

## Ograniczenia

- Jedno zadanie naraz: moduł nie jest reentrantny ani chroniony mutexem.
- Jeden profil sieci. Zmiana SSID lub PMK kasuje cache w LP SRAM (rekord NTP zostaje).
- Tylko 2.4 GHz 802.11g/n. Bez 5 GHz i bez roamingu między pasmami.
- Ścieżka z cache nie odnawia dzierżawy DHCP. Zarezerwuj IP po MAC na routerze.
- Zanik zasilania czyści LP SRAM: następne połączenie to pełny DHCP. Resety bez odcięcia zasilania cache zachowują.
- `link_check` i callbacki lwIP nie mają limitu czasu wewnątrz modułu. Zawieszony callback zawiesza `wifi_connect()`.
