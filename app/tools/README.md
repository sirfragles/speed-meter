# tools/ — zdalne pomiary i kalibracja (SMP over BLE)

Zdalny pomiar danych z akcelerometru i kalibracja detektora obrotów —
**bez kabla**, przez BLE. Zestaw:

- **firmware**: build diagnostyczny (`diag.conf`) dodaje MCUmgr/SMP + shell
  oraz moduł `CSC_DIAG` (nagrywanie XYZ do bufora RAM + parametry kalibracji),
- **klient**: `tools/wheel_cal.py` — nagrywanie, pobieranie do CSV, analiza
  fazy offline i ustawianie parametrów.

## Build i wgranie (firmware diagnostyczny)

```
west build -b holyiot_25008/nrf54l15/cpuapp . -- \
    -DEXTRA_CONF_FILE="debug.conf;diag.conf"
```

Po wgraniu (`nrfutil device program …` + reset J-Link `r;g`) urządzenie
reklamuje również usługę SMP (`0xFEBB`) obok CSCS/BAS.

## Klient na macOS (jednorazowa konfiguracja)

```
export PYTHONPATH=/Users/matthew/lis2dh-zephyr-fifo/.tools/pylib312   # smpmgr + bleak (Python 3.12)
tools/wheel_cal.py scan                  # wypisze UUID urządzenia "Speed Meter"
echo <UUID> > /tmp/board_uuid.txt        # raz; potem skrypt bierze UUID stąd
```

Uwaga: `CONFIG_MCUMGR_TRANSPORT_BT_PERM_RW=y` w `diag.conf` jest celowe —
`smpmgr` na macOS nie obsługuje parowania Secure Connections. Build tylko
do stanowiska (na produkcji wróci `PERM_RW_ENCRYPT`).

## Komendy skryptu

| Komenda | Opis |
|---|---|
| `scan` | skan BLE (nazwa + UUID usług) |
| `status` | stan nagrywania + bieżące parametry kalibracji |
| `live [--interval 1]` | odpytywanie `status` w pętli (podgląd na żywo) |
| `capture --seconds 10 -o ride1.csv [--odr 100] [--range-g 4] [--chunk 64] [--detect]` | nagraj na urządzeniu, pobierz do CSV; `--detect` uruchamia na żywo detektor obrotów (wynik w `status`) |
| `dump -o buf.csv [--start 0] [--count N]` | pobierz bufor już nagrany na urządzeniu |
| `analyze plik.csv [--axes xy] [--lp-hz 20] [--alpha 0.005] [--amp-gate 0.15] [--max-step 1.6] [--circ 2100]` | analiza offline: LPF + filtr centrum, `atan2`, unwrap, obroty, RPM, prędkość + propozycje parametrów |
| `cal-get [klucz]` / `cal-set klucz wartość [klucz wartość …]` | podgląd / zmiana parametrów kalibracji |

## Parametry kalibracji (`cal-set`)

| Klucz | Default | Znaczenie |
|---|---|---|
| `axes` | `xy` | które 2 osie leżą w płaszczyźnie koła (`xy`/`xz`/`yz`) |
| `invert_a`, `invert_b` | 0 | odwrócenie znaku osi (kierunek montażu) |
| `rpm_min` / `rpm_max` | 12 / 1200 | zakres sensownych obrotów (filtr fałszywych) |
| `amp_mg` | 150 | min. amplituda wektora obrotu [milli-g] |
| `step_mrad` | 1600 | maks. wiarygodny skok fazy [milli-rad] |
| `alpha_milli` | 5 | filtr „środka” (grawitacji), alpha×1000 |
| `stop_ms` | 3000 | brak obrotu przez ten czas → postój |
| `circ_mm` | 2100 | obwód koła [mm] (do logów prędkości) |
| `odr_hz` | 100 | ODR nagrywania (1/10/25/50/100/200/400) |
| `range_g` | 16 | zakres LIS2DH (2/4/8/16 g; domyślnie 16 g pod wysokie obroty koła) |

## Format CSV

```
# tool=wheel_cal.py capture
# odr_hz=100
# range_g=4
t_us,ax_mg,ay_mg,az_mg
123456,12,-998,34
```

- `t_us` — znacznik czasu [µs] (uint32, zawija się po ~71 min),
- osie w **milli-g** (1 g = 1000). Analiza zakłada 1 g ≈ 9.807 m/s².

## Typowy przebieg pracy

1. **Nagranie na kole** (telefon może być w kieszeni — zapis trwa na
   urządzeniu; połączenie BLE potrzebne tylko na start/odczyt):
   `tools/wheel_cal.py capture --seconds 30 --range-g 16 -o jazda1.csv`
   (bufor 4096 próbek ≈ 41 s @100 Hz albo ≈20 s @200 Hz)
2. **Analiza offline** — dobór `axes`, `alpha`, `amp_mg`, `step_mrad`:
   `tools/wheel_cal.py analyze jazda1.csv --axes xy`
   Skrypt wypisze rozkład amplitudy wektora, kroków fazy, obroty, RPM, prędkość
   i **propozycje parametrów** (do weryfikacji na większej liczbie danych).
3. **Zapis kalibracji na urządzeniu**:
   `tools/wheel_cal.py cal-set axes xy amp_mg 200 step_mrad 1600`
4. **Weryfikacja na żywo**: `tools/wheel_cal.py live` (RPM/obroty 1×s).

## Fizyka i filtracja (zalecenia)

- **LPF przed `atan2` jest obowiązkowy** — wibracje nawierzchni (>20 Hz) inaczej
  powodują skoki fazy i fałszywe obroty. Analiza ma `--lp-hz` (domyślnie
  20 Hz), a filtr „środka” (`--alpha`) usuwa offset/DC — razem tworzą
  efektywny filtr pasmowoprzepustowy.
- **Dobór LPF vs prędkość**: przy 60 km/h na kole 28" obrót ma ~11,7 Hz — nie
  ustawiaj LPF poniżej ~1,5–2× częstotliwości obrotu (na szybkie jazdy:
  `--odr 200` i `--lp-hz 25..30`).
- **Zakres ±g — WAŻNE (znalezisko HW 2026-09-14)**: nasz egzemplarz LIS2DH12
  **ignoruje FS** — rejestr CTRL4 przyjmuje zapis (readback `0xB8`), ale
  konwersja danych zawsze pozostaje w skali ±2 g; power-cycle ODR oraz
  procedura BOOT nie pomagają (zachowanie klonu). Dlatego pracujemy w ±2 g;
  przy r≈1 cm obcięcie (±2 g) występuje dopiero >50 km/h — montuj blisko osi.
  Na prawdziwym LIS2DH12 FS zadziała normalnie.
- **Montaż**: PCB dokładnie na osi obrotu piasty (współosiowo z kołem); im
  mniejsze r, tym mniejsze odśrodkowe — celuj w r < 1,5 cm. Obrót musi
  wypadać w płaszczyźnie wybranych `axes`.
- **Zapis surowy**: obecny prototyp odpytuje rejestr wyjściowy (mogą zdarzać
  się duplikaty próbek o dt≈0 — analiza je toleruje); docelowo FIFO/RTIO
  (sterownik LIS2DH FIFO z gałęzi `test/dev`) dla pełnego strumienia.
- **Diagnostyka rejestrowa (na urządzeniu)**: używaj **shella sterownika** —
  build bench dołóż `stream.conf` (`-DEXTRA_CONF_FILE="debug.conf;diag.conf;stream.conf"`),
  wtedy dostępne są komendy natywne LIS2DH:
  `lis2dh regs` (zrzut rejestrów kontrolnych/FIFO/przerwań, w tym CTRL4/FS),
  `lis2dh status` (stan FIFO, ODR, liczniki), `lis2dh counters`,
  `lis2dh start` / `lis2dh stop [cancel]` (sterowanie sprzętowym FIFO).
  Ręczne `wheel reg` / `wheel raw` **zostały usunięte** w refaktorze
  (surowe SPI zostało tylko w `wheel_power` do uzbrojenia przerwania ruchu
  przy przejściu w standby).
  Uwaga: shell sterownika tylko **czyta** rejestry — zapis FS/BOOT nie jest
  dostępny z powłoki (FS ustawia się przez `SENSOR_ATTR_FULL_SCALE` w API).
- **Detektor**: `wheel_detector` (zliczanie cykli grawitacji przez zero-crossing
  z histerezą + kontrola dośrodkowa `sqrt(a/r)`) działa na urządzeniu — włącz
  przez `capture --detect`; wynik (`revs`, `rpm`, `speed_kmh`) widać w `status`.
  Progi wymagają strojenia na nagraniach z prawdziwego koła.
- **Alternatywy z czujników komercyjnych** (na później): detekcja przejść
  przez zero z histerezą na przefiltrowanych osiach oraz IMU 6-DOF
  (żyroskop mierzy ω bezpośrednio) — do rozważenia przy kolejnej rewizji
  sprzętu.

## Ograniczenia / uwagi

- Pobieranie przez BLE idzie paczkami po ≤128 linii (~64 linii = ~2 s);
  pełny bufor 4096 próbek ≈ 2–3 min — do strojenia wystarczają krótsze
  nagrania (300–600 próbek).
- Nagrywanie używa odpytania `sensor_sample_fetch` (SPI), nie FIFO — prostsze,
  wystarczające do pierwszego prototypu; FIFO/RTIO wdrożymy w detektorze.
- Detektor obrotów (faza→obroty) jeszcze nie publikuje na CSCS; najpierw
  zbieramy dane i stroimy filtry (patrz `README.md` sekcja „Następne kroki”).
- Alternatywa bez BLE (ławka z J-Linkiem): logi RTT z `debug.conf` — ale
  pełny zapis XYZ tylko przez ten skrypt.
