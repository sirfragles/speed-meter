# Test plan — dokończenie bring-up (sesja z baterią)

**Utworzono:** 2026-09-14 (po sesji debug 13–14.09, gałąź `speed-meter`)
**Cel sesji:** przywrócić firmware na płytce (VTref ≥ 2.8 V), zweryfikować obraz,
definitywnie domknąć dochodzenie „LIS2DH12 ignoruje FS”, a jeśli starczy czasu —
uruchomić detektor na żywo i nagrać dane testowe.

## 0. Stan startowy — co jest obecnie nie tak

- Płytka **HOLYIOT-25008 / nRF54L15**, zasilanie **CR2320**
  (ogniwo pierwotne — ⚠️ **NIE ładować**, tylko wymiana).
- Ostatni flash urwał się w połowie: `@0x0` wektory OK, **`@0x10000` i `@0x2D800`
  puste (0xFFFFFFFF)** → HardFault przy starcie (PC = `EFFFFFFE`), brak BLE, brak RTT.
- VTref ostatnio **1.94–1.97 V** → J-Link nie włącza nawet DAP.
- Próg empiryczny: **zapis RRAM działa od ~2.8 V** (przy 2.89 V pełny cykl
  recover + program + verify się udał). Komfortowo: 3.0–3.3 V.

## 1. Zasilanie (zrób PRZED sesją)

| Opcja | Uwagi |
|---|---|
| świeże CR2320 | OCV ~3.0–3.2 V; wystarcza na 1 pełny cykl flash + testy. Do dłuższej sesji weź 2–3 sztuki na zapas |
| zewnętrzne 3.0–3.3 V | zasilacz lab (limit ~50 mA) / koszyczek 2×AA (3.0 V) / 3.3 V z innej płytki + **wspólna masa (GND)**; na czas zasilania zewnętrznego **wyjmij ogniwo** |

- Zmierz OCV ogniwa multimetrem przed włożeniem; < 2.8 V = nie licz na zapis.
- Nie zostawiaj włączonego RTT loggera / reklamującego BLE bez potrzeby — drenaż ogniwa.

## 2. Health check (1 minuta)

```sh
printf 'r\nqc\n' > /tmp/jw.jlink
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/jw.jlink 2>&1 | grep VTref
```

- **VTref ≥ 2.8 V** → sekcja 3.
- **VTref < 2.4 V** → wymień ogniwo / podłącz zewnętrzne zasilanie
  (próby na siłę kończą się: „Probe access is Secure”, „Failed to download RAMCode”).

## 3. Flash (build diagnostyczny)

```sh
NRF="$HOME/.nrfutil/bin/nrfutil"
HEX=/Users/matthew/lis2dh-zephyr-fifo/builds/speed_meter_diag/zephyr/zephyr.hex
"$NRF" device recover --serial-number 269308656
"$NRF" device program --firmware "$HEX" --serial-number 269308656
```

### 3a. Opcja: najpierw `fake_detector` (szybki test samego układu)

Mały build bez BLE (bin 28 940 B) — sam „live test” zakresów; wyniki po **RTT**
(konsola UART wyłączona). Dobry pierwszy strzał po naprawie zasilania:

```sh
"$NRF" device program \
  --firmware /Users/matthew/lis2dh-zephyr-fifo/builds/fake_detector/zephyr/zephyr.hex \
  --serial-number 269308656
printf 'r\ng\nqc\n' > /tmp/jl_rg.jlink
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/jl_rg.jlink
# podgląd:
rm -f /tmp/fake.log && JLinkRTTLogger -device nRF54L15_M33 -if SWD -speed 4000 -RTTChannel 0 /tmp/fake.log
```

Wynik interpretuj jak sekcja 6.3 (tabela oczekiwanych cnt). Potem można wgrać
`speed_meter_diag` i zrobić pełną macierz przez BLE/SMP. Szczegóły aplikacji:
`fake_detector/README.md`.

## 4. Weryfikacja obrazu

Szybki spot-check — wartości dla hexa z 14.09 (po przebudowie odtwórz `xxd` z `zephyr.bin`):

| Adres | Oczekiwane bajty |
|---|---|
| `0x0` | `50 3e 01 20 61 7e 00 00` |
| `0x10000` | `0a fb 05 f7 9a 42 08 bf` |
| `0x2D800` | `b0 87 02 00 d0 70 01 20` |

```sh
printf 'h\nmem32 0x0, 2\nmem32 0x10000, 2\nmem32 0x2D800, 2\nqc\n' > /tmp/jlv.jlink
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/jlv.jlink
```

Pełna weryfikacja (najlepsza — obraz ma 186 532 B = 0x2D8A4):

```sh
printf 'h\nsavebin /tmp/flash_read.bin 0x0, 0x2D8A4\nqc\n' > /tmp/jlsave.jlink
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/jlsave.jlink
cmp /tmp/flash_read.bin /Users/matthew/lis2dh-zephyr-fifo/builds/speed_meter_diag/zephyr/zephyr.bin && echo "FLASH == BIN OK"
```

Reset i start:

```sh
printf 'r\ng\nSleep 3000\nqc\n' > /tmp/jl_rg.jlink
JLinkExe -device nRF54L15_M33 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/jl_rg.jlink
```

## 5. Smoke test: BLE + SMP

```sh
export PYTHONPATH=/Users/matthew/lis2dh-zephyr-fifo/.tools/pylib312; PY=/opt/homebrew/bin/python3.12
U=$(cat /tmp/board_uuid.txt)          # B1B6B9B1-20A3-7CA5-A452-F30885CBF625
$PY -m smpmgr --ble $U --timeout 15 shell "wheel status"
```

- Dla pewności skan: `tools/wheel_cal.py scan` → ma być „Speed Meter” + CSCS `0x1816`
  (w diag także SMP `0xFEBB`).
- Logi boota po RTT: `rm -f /tmp/rtt.log && JLinkRTTLogger -device nRF54L15_M33 -if SWD -speed 4000 -RTTChannel 0 /tmp/rtt.log`

## 6. Macierz FS — definitywne domknięcie tematu (główny cel)

Uwzględnia uwagi z 14.09: **burst XYZ zamiast pojedynczej osi** (BDU!), kontrola
LPen, metoda różniczkowa ±1 g. Warunki: płytka nieruchomo, `wheel record stop`
jeśli coś nagrywa.

**Kontekst (2026-09-14):** lista znanych klonów z projektu
[flipper-fake-chip-detector](https://github.com/hleserg/flipper-fake-chip-detector/blob/master/fake_chip_detector/SUPPORTED_CHIPS.md)
potwierdza, że `WHO_AM_I` (`0x0F`) = `0x33` dzielą `LIS3DH` / `LIS2DH12` / `LIS2DH`
(„same ID as LIS2DH12”) — pojedynczy bajt ID to wartość, którą relabeler po
prostu kopiuje, a tamto narzędzie nie ma live-testu dla tej rodziny (zaliczyłoby
nasz egzemplarz jako GENUINE). Nasza macierz FS + metoda różniczkowa ±1 g to
dokładnie ten brakujący *live test* — jedyny dowód, jaki w tej sprawie może
istnieć („make the part do its job”).

### 6.1 Baseline rejestrów

Build bench z shellem sterownika (`-DEXTRA_CONF_FILE="debug.conf;diag.conf;stream.conf"`):

```sh
$PY -m smpmgr --ble $U --timeout 12 shell "lis2dh regs"     # zrzut CTRL*/FIFO/INT
$PY -m smpmgr --ble $U --timeout 12 shell "lis2dh status"   # stan FIFO, ODR, liczniki
```

- `WHO_AM_I` (`0x0F`) → ma być `0x33`
- `CTRL_REG1` (`0x20`) → ma być `0x57` (LPen = bit3 = **0**; gdyby był 1 — zanotować!)
- `CTRL_REG2/4/5` (`0x21`/`0x23`/`0x24`), `FIFO_CTRL` (`0x2E`) — zanotować

> **Zmiana po refaktorze (2026):** komendy `wheel reg` / `wheel raw` zostały usunięte
> (surowe SPI tylko w `wheel_power` do uzbrojenia INT1). Zastępuje je natywny shell
> sterownika — patrz `stream.conf`. Shell **czyta** rejestry; **zapisu** (FS, BOOT,
> ST[1:0]) z powłoki nie ma — te testy wykonuje się przez API sterownika
> (`SENSOR_ATTR_FULL_SCALE`, `SENSOR_ATTR_LIS2DH_SELF_TEST`) albo ścieżką „archiwum”
> opisaną w 6.2.

### 6.2 Macierz FS — wariant driverowy (obecny, zalecany)

Zapis CTRL4 nie jest już dostępny z powłoki. FS ustawia się przez API sterownika,
a `wheel accel <n> <odr_hz> <range_g>` robi dokładnie to (`SENSOR_ATTR_FULL_SCALE`)
i od razu wypisuje świeże XYZ w m/s²:

```sh
for r in 2 4 8 16; do
  echo "== FS = ${r} g =="
  $PY -m smpmgr --ble $U --timeout 12 shell "wheel accel 2 100 $r"
done
```

Płytka nieruchomo, jedna oś w grawitacji (1 g ≈ 9,81 m/s²):

| Odczyt przy 1 g | Wniosek |
|---|---|
| ~9,81 m/s² przy **każdym** FS | Skala jest przeliczana poprawnie → **genuine** |
| rośnie ~4× / ~8× przy 4 / 8 / 16 g | Układ **ignoruje FS** (liczy zawsze w ±2 g, a skala brana z konfiguracji) |

> **Uwaga — pułapka interpretacyjna:** samo „brak różnicy między FS” **nie jest**
> dowodem. Prawdziwy LIS2DH12 też pokaże ~9,81 m/s² przy każdym FS, bo to właśnie
> robi poprawna skala (mało countów × duża waga = ta sama fizyka). Dowodem klonu
> jest **zawyżenie** odczytu przy większym FS.

### 6.2a Archiwum: pierwotna pętla „zapis → readback → burst” (sprzed refaktora)

Ta procedura dała wynik z 2026-09-14 (patrz „Zakres ±g” w `tools/README.md`).
Wymaga buildu z surowym SPI, które w refaktorze zostało **usunięte** z aplikacji
(pozostało wyłącznie w `wheel_power` do uzbrojenia INT1). Zachowane jako opis
metody — do odtworzenia trzeba by dodać komendę tymczasową.

```sh
for v in 0x88 0x98 0xA8 0xB8; do
  echo "== CTRL4 = $v =="
  $PY -m smpmgr --ble $U --timeout 12 shell "wheel reg 0x23 $v"     # zapis
  $PY -m smpmgr --ble $U --timeout 12 shell "wheel reg 0x23"       # readback
  $PY -m smpmgr --ble $U --timeout 12 shell "wheel raw 4"          # świeży XYZ (burst)
done
```

Dla każdego FS powtórz burst dodatkowo po:
- **power-cycle ODR**: `wheel reg 0x20 0x00` → `wheel reg 0x20 0x57`,
- **BOOT**: `wheel reg 0x21 0x80` (bit BOOT).

### 6.3 Metoda różniczkowa ±1 g (dowód końcowy)

Bodziec fizyczny: grawitacja (dokładnie 1 g). Trzy orientacje płytki:
- **A** — poziomo (grawitacja na jednej osi),
- **B** — obrót 90° wokół osi w płaszczyźnie (1 g przenosi się na inną oś),
- **C** — obrót 180° (grawitacja odwrócona).

Zmierzone dla **FS = 0x88 (±2 g)** i **FS = 0xB8 (±16 g)** — porównaj Δcnt (A→B) na tej samej osi:

| Scenariusz | Δcnt A→B |
|---|---|
| prawdziwy LIS2DH12 @ ±2 g (1 mg/LSB) | ~1000 |
| prawdziwy @ ±4 g (2 mg/LSB) | ~500 |
| prawdziwy @ ±8 g (4 mg/LSB) | ~250 |
| prawdziwy @ ±16 g (12 mg/LSB) | ~83 |
| **klon ignorujący FS** | **~1000 przy KAŻDYM ustawieniu (identycznie)** |

### 6.4 Punkt odniesienia drivera (sensor API)

```sh
$PY -m smpmgr --ble $U --timeout 12 shell "wheel accel 1 100 2"
$PY -m smpmgr --ble $U --timeout 12 shell "wheel accel 1 100 16"
```

Płytka nieruchomo, 1 g na osi: **genuine** → ~9,81 m/s² przy obu ustawieniach;
**klon** → przy `range=16` odczyt ~8× zawyżony (liczy w ±2 g). Patrz 6.2.

Opcjonalne fingerprinty (ta sama filozofia „live test”, tanie w wykonaniu):

- **FIFO — natywnie przez sterownik** (najciekawszy test po refaktorze; wymaga
  buildu bench z `stream.conf`, bez żadnego surowego SPI):

  ```sh
  $PY -m smpmgr --ble $U --timeout 12 shell "lis2dh start"
  $PY -m smpmgr --ble $U --timeout 20 shell "lis2dh status"      # wypełnianie FIFO
  $PY -m smpmgr --ble $U --timeout 12 shell "lis2dh counters"    # liczniki diagnostyczne
  $PY -m smpmgr --ble $U --timeout 12 shell "lis2dh stop cancel"
  ```

  Klony nierzadko nie mają FIFO → `start`/`status` nie pokażą postępu.
  Historyczna ścieżka surowa: `0x24` = `0x40` (FIFO_EN) + `0x2E` = `0x80` (stream),
  potem odczyt `0x27` (`FIFO_SRC`) — powinien rosnąć do 32.
- **self-test** — bity `ST[1:0]` w CTRL4. Sterownik eksponuje to jako
  `SENSOR_ATTR_LIS2DH_SELF_TEST` (programowo — nie ma komendy shell). Na
  działającym czujniku wychylenie wyraźnie się zmienia (kilkadziesiąt–kilkaset cnt).
  Historyczna ścieżka surowa: CTRL4 = `0x89` / `0x8A` vs `0x88`.
- **temperatura** — `OUT_TEMP_L/H` (`0x0C` / `0x0D`) → sensowna wartość pokojowa.
  Shell sterownika tych rejestrów nie zrzuca; wymaga dostępu surowego (archiwum).

### 6.5 Tabela wyników (do wypełnienia długopisem w edytorze)

| FS (CTRL4) | readback | raw (x,y,z) start | po ODR-cycle | po BOOT | Δcnt A→B | wniosek |
|---|---|---|---|---|---|---|
| 0x88 (±2 g) | | | | | | |
| 0x98 (±4 g) | | | | | | |
| 0xA8 (±8 g) | | | | | | |
| 0xB8 (±16 g) | | | | | | |

Po zakończeniu: zaktualizować sekcję „Zakres ±g” w `tools/README.md` + commit.

## 7. Po macierzy — co dalej (jeśli czas/ogniwo pozwolą)

1. **Detektor jako żywe źródło obrotów** — ✅ zrobione: pozycja
   `CSC_WHEEL_SENSOR_ACCEL` w `choice CSC_WHEEL_SOURCE`, moduł
   `src/wheel_source_accel.c` (własny wątek → `wheel_detector.c` → `csc.c`),
   włączany przez `power.conf`. Źródła mają teraz jednolite nazwy plików:
   `wheel_source_{sim,gpio,accel}.c`.
2. **Ławka**: obroty ręką / wkrętarką: `tools/wheel_cal.py capture --seconds 15 --detect -o bench1.csv`
   → `tools/wheel_cal.py analyze bench1.csv` → strojenie `amp_mg` / `step_mrad` / `alpha_milli`.
3. **Koło** (osobna sesja): `capture --seconds 30 --detect -o jazda1.csv` + `analyze`.
4. **CSCS**: publikacja przeliczeń detektora (licznik obrotów + czas 1/1024 s) zamiast symulacji.

## 8. Procedury awaryjne

- **RTT zawieszone** (WrOff ≠ RdOff w CB): znajdź `_SEGGER_RTT` w mapie i wyzeruj pola —
  `grep -m1 _SEGGER_RTT builds/speed_meter_diag/zephyr/zephyr.map` →
  dla CB = `0x20000470`: `w4 0x20000494, 0` (WrOff) oraz `w4 0x20000498, 0` (RdOff), potem `g`.
- **„Probe access is Secure” / „Failed to download RAMCode”** → to zawsze napięcie
  (VTref), nie konfiguracja narzędzi.
- **SMP milczy** → sprawdź skanem, czy widać `0xFEBB`; `diag.conf` wymaga NET_BUF + ZCBOR + CRC (są).
- **DAP nie wstaje przy < 2 V** → wymiana ogniwa; nie marnować czasu.

## 9. Artefakty (stan 2026-09-14)

| Co | Gdzie | Uwagi |
|---|---|---|
| Build diagnostyczny | `builds/speed_meter_diag/zephyr/zephyr.hex` | bin 186 532 B; pełne `wheel` + SMP |
| Build domyślny | `builds/speed_meter/zephyr/zephyr.hex` | symulacja obrotów |
| Build GPIO | `builds/speed_meter_hw` | GPIO sensor + SC Control Point + RTT |
| Klient BLE | `tools/wheel_cal.py` | capture / analyze / cal |
| UUID płytki | `/tmp/board_uuid.txt` | `B1B6B9B1-20A3-7CA5-A452-F30885CBF625` |
| nrfutil | `~/.nrfutil/bin/nrfutil` | serial `269308656` |

## Definition of done (jutro)

- [ ] VTref ≥ 2.8 V
- [ ] pełny flash, `cmp` z `.bin` OK
- [ ] `wheel status` odpowiada po BLE
- [ ] macierz FS wypełniona (tabela 6.5) + wniosek zapisany w `tools/README.md`
- [ ] (bonus) test ławkowy detektora
