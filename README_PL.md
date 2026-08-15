[English](README.md)

# esp32-wifi-fastwake

Komponent stacji WiFi (ESP-IDF, czyste C) dla bateryjnych urządzeń ESP32, które większość życia śpią: wybudzenie, połączenie, wysłanie kilku pakietów, rozłączenie i powrót do deep sleep.

## Jaki problem rozwiązuje

W urządzeniu na baterii radio jest najdroższą częścią każdego cyklu. Zwykłe połączenie WiFi — skan, asocjacja, DHCP — trwa 1–2 sekundy, a radio pobiera przez cały ten czas pełny prąd. Gdy urządzenie budzi się co kilka minut, samo łączenie kosztuje więcej energii niż właściwa transmisja danych.

Ten moduł zapamiętuje wszystko o ostatnim udanym połączeniu — adres IP, bramę, DNS, BSSID i kanał punktu dostępowego, a nawet MAC bramy — w LP SRAM, niewielkiej pamięci, która przeżywa deep sleep i resety. Przy kolejnym wybudzeniu pomija skan i DHCP i łączy się od razu ze znanym punktem dostępowym.

Efekt, zmierzony na Seeed XIAO ESP32-C5:

| Ścieżka | Czas do działającego łącza |
| --- | --- |
| Pierwsze połączenie (pełny DHCP) | 1,2–2,3 s |
| Ponowne połączenie z cache | **~90 ms** |

Nic nie jest zapisywane do flasha, więc cache nie zużywa pamięci. Cena: zanik zasilania czyści LP SRAM i następne połączenie idzie pełną ścieżką z DHCP — dla urządzenia, które traci zasilanie rzadko, to koszt jednorazowy.

## Co moduł robi za Ciebie

- **Cache profilu sieci** w LP SRAM, walidowany CRC przy każdym starcie, unieważniany automatycznie, gdy przestaje działać.
- **Wypełnienie ARP bramy z góry** — pierwszy pakiet po połączeniu nie czeka na rundę ARP.
- **Synchronizacja czasu (SNTP)** — dzieje się wewnątrz `wifi_connect()`, gdy brakuje zegara albo raz na dobę, więc aplikacja zawsze widzi sensowny czas.
- **Samonaprawa** — gdy ruch przestaje dochodzić, moduł najpierw zapomina wpis ARP, potem cały profil, i wraca do świeżego połączenia z DHCP. Twój kod nigdy nie zarządza cache.

## Szybki start

Cały kod sieciowy aplikacji to trzy wywołania i jeden callback:

```c
#include "wifi.h"

/* Twój ruch (publikacja MQTT, HTTP POST, ping...). Jego wynik mówi modułowi,
 * czy zapamiętane dane sieci nadal działają. */
static wifi_link_result_t app_link_check(void)
{
    if (!send_measurements_possible()) {
        return WIFI_LINK_CHECK_FAILED;   /* problem lokalny: nie oskarżamy sieci */
    }
    return send_measurements() ? WIFI_LINK_OK : WIFI_LINK_UNREACHABLE;
}

void app_main(void)
{
    if (wifi_init(WIFI_SSID, WIFI_PMK_HEX, app_link_check) != ESP_OK) {
        esp_deep_sleep(SLEEP_US);   /* deep sleep to najtańszy reset */
    }

    /* Asocjacja, w razie potrzeby synchronizacja czasu, wywołanie
     * app_link_check() i naprawa cache, gdy werdykt mówi, że jest nieaktualny. */
    (void)wifi_connect();

    wifi_disconnect();              /* zawsze przed deep sleep */
    esp_deep_sleep(SLEEP_US);
}
```

Uwagi:

- `wifi_connect()` zwraca `ESP_OK`, gdy łącze stoi; nieudany ruch nie zmienia wyniku — zamiast tego unieważnia cache na następny cykl.
- Wołaj `wifi_disconnect()` przed deep sleep. To jedyny moment, w którym moduł może nauczyć się MAC bramy do cache ARP.
- Nie owijaj wywołań w `ESP_ERROR_CHECK`; każda funkcja zwraca `esp_err_t` i nigdy nie przerywa procesu. Czy błąd oznacza panikę, czy „spróbuj przy następnym wybudzeniu” — to Twoja decyzja.

## Jak działa link check

Moduł celowo nie generuje własnego ruchu — i tak wysyłasz dane w każdym cyklu, więc Twój ruch pełni jednocześnie rolę testu łącza. Callback zwraca jeden z trzech werdyktów:

| Werdykt | Znaczenie | Co robi moduł |
| --- | --- | --- |
| `WIFI_LINK_OK` | ruch przeszedł | nic |
| `WIFI_LINK_UNREACHABLE` | sprawdzenie się wykonało, pakiety nie doszły | naprawia wpis ARP, przy powtórce cały profil |
| `WIFI_LINK_CHECK_FAILED` | samo sprawdzenie się nie udało (błąd gniazda, brak pamięci) | nic — lokalny błąd nie może kosztować cache sieci |

Callback jest obowiązkowy: bez niego moduł nie miałby jak zauważyć, że zapamiętany adres IP albo MAC bramy się zestarzały. Działa wewnątrz `wifi_connect()`, więc sam nie może wołać żadnej funkcji `wifi_*`.

## Hasło WiFi: PMK zamiast passphrase

Moduł przyjmuje klucz sieci jako 64-znakowy PMK w hex, nie jako zwykłe hasło. Wyliczenie PMK z hasła (PBKDF2, 4096 rund SHA-1) jest kosztowne, a przy wyłączonym zapisie sterownika do flasha urządzenie powtarzałoby je po cichu przy każdym starcie. Policz go raz na komputerze:

```bash
python3 -c 'import hashlib;print(hashlib.pbkdf2_hmac("sha1",b"HASLO",b"SSID",4096,32).hex())'
```

Wielkie i małe litery hex są akceptowane i traktowane jako ten sam klucz.

## Wymagana konfiguracja projektu

Dodaj do `sdkconfig.defaults` swojego projektu:

```ini
# Cache kalibracji PHY w NVS — bez tego każde wybudzenie kalibruje radio od zera
CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=y
CONFIG_ESP_PHY_RF_CAL_PARTIAL=y
# CONFIG_ESP_PHY_INIT_DATA_IN_PARTITION is not set

# Statyczne wpisy ARP (w ESP-IDF powiązane z opcją serwera DHCP,
# mimo że urządzenie pozostaje stacją)
CONFIG_LWIP_DHCPS=y
CONFIG_LWIP_DHCPS_STATIC_ENTRIES=y

# Losowe opóźnienie startu SNTP psułoby krótkie okno synchronizacji czasu
# CONFIG_LWIP_SNTP_STARTUP_DELAY is not set

# Zegar ścienny przeżywa deep sleep
CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER=y
```

Nie da się tego pominąć po cichu: `wifi.c` zatrzymuje build przez `#error`, gdy opcje ARP albo SNTP się nie zgadzają.

Moduł sam woła `nvs_flash_init()` (potrzebuje go cache kalibracji radia) i nigdy nie kasuje ani nie deinicjalizuje Twojej partycji NVS. Nie zakłada żadnego układu partycji.

## Dodanie do projektu

```yaml
# main/idf_component.yml
dependencies:
  idf: ">=6.0"
  esp32-wifi-fastwake:
    git: https://github.com/webh-pl/esp32-wifi-fastwake.git
    version: v1.0.0
```

Przy lokalnym checkoutcie obok Twojego projektu (`path` jest względny wobec `main/`):

```yaml
dependencies:
  idf: ">=6.0"
  esp32-wifi-fastwake:
    path: ../../esp32-wifi-fastwake
```

Potem `#include "wifi.h"` i budujesz jak zwykle — menedżer komponentów pobiera wszystko podczas `idf.py build`.

## Warto wiedzieć / ograniczenia

- **Jedno zadanie naraz.** Moduł nie jest reentrantny i nie ma mutexa; wołaj go z jednego zadania.
- **Jedna sieć.** SSID i PMK podajesz raz w `wifi_init()`. Ich zmiana unieważnia cache.
- **Tylko 2.4 GHz, 802.11 g/n.** Bez 5 GHz i bez roamingu między pasmami.
- **Oczekiwany stabilny adres IP.** Ścieżka z cache nie odnawia dzierżawy DHCP — zarezerwuj IP urządzenia po MAC na routerze.
- **Przypięcie do BSSID.** Cache wskazuje jeden konkretny punkt dostępowy. W sieci z wieloma AP po zmianie punktu pierwsza próba się nie uda i moduł wróci do pełnego połączenia (jeden wolniejszy cykl).
- **Zanik zasilania czyści wszystko.** Zwykłe resety (watchdog, `esp_restart()`, USB) zachowują cache; traci go tylko odcięcie zasilania.
- **Brak limitów czasu wokół Twojego callbacka.** Zawieszony `link_check` zawiesza `wifi_connect()` — pilnuj, żeby Twój ruch miał ograniczony czas.

## Wymagania i licencja

- ESP-IDF **6.0 lub nowszy**; rozwijane i mierzone na ESP32-C5 (Seeed XIAO, IDF v6.0.2). Kod używa LP SRAM (`RTC_NOINIT_ATTR`); inne układy z tą pamięcią powinny działać, ale nie były sprawdzane.
- Sieć WPA2-PSK, 2.4 GHz.
- Licencja: [MIT](LICENSE).
