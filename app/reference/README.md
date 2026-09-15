# reference/ — materiał referencyjny (zakomentowany)

## Co tu jest

Zakomentowane kopie plików z projektu
**`spasoye/nrf52840_zephyr_CSC_sensor`** —
<https://github.com/spasoye/nrf52840_zephyr_CSC_sensor>
(autor: Ivan Spasić; repo na licencji MIT — pełny tekst w
`spasoye/LICENSE-MIT.txt`; pliki źródłowe mają w nagłówkach SPDX Apache-2.0).

Pobrano: 2026-09-14. Wszystkie pliki `.c`/`.h` są **w całości zakomentowane**
(każda linia zaczyna się od `//`) i **nie są wymienione w `CMakeLists.txt`** —
istnieją wyłącznie jako materiał do analizy („będziemy badać, czy nam się przyda").

**Status (2026-09-14):** część kodu przeniesiona do projektu — źródło obrotów na
GPIO (refaktor modułu, `src/wheel_source_gpio.c/.h`, tryb `CSC_WHEEL_SENSOR_GPIO`)
oraz SC Control Point (opcja `CONFIG_CSC_SC_CONTROL_POINT` w `src/csc.c`).

## Pliki

| Plik | Co to jest |
|---|---|
| `spasoye/ble_csc.c` + `.h` | Serwis CSC — w praktyce **refaktor upstreamowego sample `samples/bluetooth/peripheral_csc`** do modułu + API `ble_csc_update_wheel()/crank()`. Najcenniejsza nowa część: **pełny SC Control Point** (SET_CWR / UPDATE_LOC / REQ_SUPP_LOC + wskazania odpowiedzi) — u nas celowo pominięty (opcjonalny wg spec CSCS v1.0). |
| `spasoye/reed_switch.c` + `.h` | Wzorzec **wejścia impulsowego na GPIO** dla Zephyra: alias DT, debounce 20 ms przez porównanie `k_uptime_get()` **w ISR** (bez timerów — działa z deep sleep / GPIO SENSE), licznik `atomic_t`, konwersja ms → 1/1024 s, flaga „nowe zdarzenie". |
| `spasoye/main.c` | Pętla główna 1 s: odczyt wejścia impulsowego → `ble_csc_update_wheel()` (gdy subskrypcja włączona) + `bas_notify()`. |
| `spasoye/LICENSE-MIT.txt` | Licencja oryginalnego repo (wymagana przy kopiowaniu — MIT). |

## Co warto porównać z naszym `src/csc.c`

1. **SC Control Point** — u nich pełna implementacja (write + CCC indicate).
   Dodać do nas, jeśli chcemy pełnej zgodności z CSCS v1.0
   (Apple Watch tego nie wymaga, ale np. narzędzia diagnostyczne mogą używać).
2. **Debounce wejścia impulsowego w ISR** (moduł GPIO) — gotowy wzorzec do
   wykorzystania przy dowolnym pulsie na GPIO. Nasza alternatywa: detekcja
   obrotu z akcelerometru (jak w `wheel_speed_sensor`).
3. **API aktualizacji** — porównać `ble_csc_update_wheel(revs, time, has_new_data)`
   z naszym `csc_publish_wheel(revs, time)` + `csc_notifications_enabled()`.

## Zasady używania

- Pliki są w 100% zakomentowane — nic z nich się nie kompiluje.
- Jeśli chcemy coś przenieść do naszego kodu, przepisujemy wybrane fragmenty
  ręcznie do `src/` (albo odkomentowujemy cały plik, przepisujemy i dopiero
  wtedy dodajemy do `CMakeLists.txt`).
- Oryginały (odkomentowane, zawsze aktualne):
  <https://github.com/spasoye/nrf52840_zephyr_CSC_sensor> — gałąź `master`.
