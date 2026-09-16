# Speed Meter — rowerowy czujnik prędkości BLE (CSCS)

> **Kontekst repo:** ten plik opisuje samą aplikację (`app/`). Repozytorium jest
> manifestem west (T2) — komendy `west init` / `west update` / `west build`
> uruchamia się z katalogu **nadrzędnego** względem `app/`; opis całego
> workspace'u, wariantów builda, wgrywania i wydań jest w [`../README.md`](../README.md).
> Poniższe przykłady `west build ... .` działają po `cd app`.

Nowy projekt Zephyra: rowerowy czujnik prędkości zgodny ze standardowym profilem
Bluetooth SIG **CSCS (Cycling Speed and Cadence Service, UUID `0x1816`)** —
rozpoznawanym przez **Apple Watch (watchOS 10+)** oraz iPhone
(*Ustawienia → Bluetooth → Czujniki rowerowe*). Działa też z dowolnym innym
odbiornikiem CSCS (Garmin, Wahoo, liczniki itd.).

## Standard, na którym opiera się projekt

- Usługa: **CSCS** — `0x1816`. UUID usługi **musi** być w ramce *advertising*,
  inaczej Apple Watch nie wystawi czujnika w menu czujników rowerowych.
- Charakterystyki:
  - `0x2A5B` CSC Measurement — **Notify** (Apple subskrybuje ją po połączeniu),
  - `0x2A5C` CSC Feature — Read (bit 0 = dane obrotu koła, bit 1 = dane obrotu korby),
  - `0x2A5D` Sensor Location — Read (np. 9 = przedni hub, 12 = tylne koło).
- Format pomiaru: `flags | (uint32 licznik obrotów koła + uint16 czas) |`
  `(uint16 licznik obrotów korby + uint16 czas)`.
- Czasy są w jednostkach **1/1024 s** i zawijają się (uint16) **co 64 s** —
  tak mówi specyfikacja; odbiornik liczy prędkość z różnic, więc to normalne.

## Uwaga o Zephyrze: brak wbudowanego CSCS

W Zephyrze **nie ma** wbudowanego serwera CSCS — nie istnieje opcja
`CONFIG_BT_CSCS`, nagłówek `zephyr/bluetooth/services/cscs.h` ani funkcja
`bt_cscs_measurement_send()`. W drzewie Zephyra jest jedynie przykładowa
implementacja usługi w `samples/bluetooth/peripheral_csc`.
Dlatego serwer usługi jest zaimplementowany w tym projekcie
(**`src/csc.c` / `src/csc.h`**), na wzorcach z tego sampla oraz z
wcześniejszego projektu `samples/bluetooth/wheel_speed_sensor`
(gałąź `feature/lis2dh-fifo`).

## Struktura projektu

```
Konfiguracja (nakładana przez -DEXTRA_CONF_FILE):
  prj.conf        — baza: BLE (2 połączenia), RTT (logi), LED, settings/NVS
  debug.conf      — logi po RTT (J-Link); trzymany dla zgodności komend budowania
  diag.conf       — bench: MCUmgr/SMP + shell + moduł CSC_DIAG
  stream.conf     — bench: stos FIFO/RTIO + shell sterownika LIS2DH (TYLKO bench!)
  power.conf      — produkcja: źródło akcelerometrowe + sen/wake (TYLKO produkcja!)
  dfu.conf        — produkcja: DFU przez MCUmgr (SMP szyfrowane)
  sysbuild.conf   — włączenie MCUboot przy budowaniu z --sysbuild
  sysbuild/       — konfiguracja i overlay bootloadera MCUboot

Źródła (src/):
  main.c                  — cienki start: config, bt_enable, settings_load, init
  bt.c/.h                 — BLE: advertising, 2 połączenia, parowanie, LED niebieski
  csc.c/.h                — serwer usługi CSC (pomiary, feature, lokalizacja)
  led_sw_blink.c/.h       — miganie LED w sofcie (sterownik GPIO nie ma sprzętowego)
  wheel_config.c/.h       — konfiguracja koła (obwód, ODR, zakres, progi) + NVS
  battery.c/.h            — pomiar CR2032 przez SAADC (wejście VDD) + Battery Service

  Źródła obrotów (choice CSC_WHEEL_SOURCE — buduje się dokładnie jedno):
  wheel_source_sim.c      — symulacja (CONFIG_CSC_SIMULATE)
  wheel_source_gpio.c/.h  — wejście impulsowe na GPIO (CONFIG_CSC_WHEEL_SENSOR_GPIO)
  wheel_source_accel.c/.h — akcelerometr, bezmagnesowe (CONFIG_CSC_WHEEL_SENSOR_ACCEL)
                            + sterowanie snem/wybudzeniem

  wheel_detector.c/.h     — samokalibrujący się detektor pełnych obrotów
  wheel_diag.c            — diagnostyka: shell `wheel` + nagrywanie + kalibracja
  wheel_power.c/.h        — oszczędzanie energii, System OFF + wake na INT1
  boot.c/.h               — self-test przy starcie + potwierdzenie obrazu MCUboot
  dfu_mode.c/.h           — wejście w tryb DFU (advertising SMP)
  dfu_button.c/.h         — przycisk: 5 s hold → DFU + sygnalizacja LED (czerwony)

  tools/                — skrypty hosta (wheel_cal.py, flash_mcuboot.sh)
  reference/            — zakomentowane materiały referencyjne (spasoye, MIT)
  boards/holyiot_25008_nrf54l15_cpuapp.overlay — zegary XO/LFCLK + alias wheel-sensor
  sysbuild/mcuboot.overlay — TYLKO bootloader (chosen code-partition!)
```

> **Zasada refaktora:** cała obsługa czujnika idzie przez **sterownik Zephyra**
> (LIS2DH), a diody przez **sterownik LED** (`led_*_dt`). Jedyny świadomy wyjątek
> to `wheel_power.c`, które przed `sys_poweroff()` zapisuje rejestry LIS2DH surowo
> po SPI — sterownik nie eksponuje uzbrojenia INT1 dla System OFF.

## Build

W istniejącym west workspace:

```
west build -b holyiot_25008/nrf54l15/cpuapp .
```

Z logami po RTT (J-Link):

```
west build -b holyiot_25008/nrf54l15/cpuapp . -- -DEXTRA_CONF_FILE=debug.conf
```

Bez lokalnego west/SDK — przez kontener (tak budowano poprzednie firmware):

```
container run --rm --cpus 4 --memory 6G \
  --volume /Users/matthew/Work/zephyrproject:/workdir:ro \
  --volume /Users/matthew/lis2dh-zephyr-fifo/upstream/dev-rebased:/workdir/zephyr:ro \
  --volume /Users/matthew/lis2dh-zephyr-fifo/zephyr:/app:ro \
  --volume /Users/matthew/lis2dh-zephyr-fifo/builds:/out \
  --tmpfs /tmp ghcr.io/zephyrproject-rtos/zephyr-build:main \
  sh -c 'cd /workdir && west build -b holyiot_25008/nrf54l15/cpuapp /app \
         -d /out/speed_meter --pristine=always'
```

> Ten checkout (`lis2dh-zephyr-fifo/zephyr`, gałąź `speed-meter`) służy teraz jako
> katalog projektu — źródła Zephyra do budowania pochodzą z worktree
> `upstream/dev-rebased` (gałąź `test/dev-rebased`).

## Test z Apple Watch (jeszcze bez czujnika!)

Domyślnie włączona jest **symulacja obrotów koła**: `CONFIG_CSC_SIM_WHEEL_RPS=1`
(czyli 1 obrót/s ≈ 7,6 km/h przy obwodzie koła 2,1 m).

1. Wgraj firmware i uruchom płytkę.
2. Watch: *Ustawienia → Bluetooth → Czujniki rowerowe* → **Speed Meter**.
3. Prędkość na zegarku jest przeliczana **jego własnym obwodem koła**.

Zmiana prędkości symulacji: `CONFIG_CSC_SIM_WHEEL_RPS` (1..20, w `prj.conf`).
Wyłączenie symulacji: `CONFIG_CSC_SIMULATE=n`.
Kadencja (opcjonalnie): `CONFIG_CSC_CRANK_REV_DATA=y` + `CONFIG_CSC_SIMULATE_CRANK`.

## Źródło obrotów koła: symulacja albo sensor na GPIO

Wybór przez `choice CSC_WHEEL_SOURCE` w `Kconfig`:

- **`CSC_SIMULATE`** (domyślnie) — symulacja obrotów; działa bez sprzętu,
- **`CSC_WHEEL_SENSOR_GPIO`** — sensor na GPIO (np. czujnik Hall; dowolny
  puls zwierający pin do GND): licznik z przerwań z debounce
  (`CSC_WHEEL_SENSOR_DEBOUNCE_MS`, domyślnie 20 ms), publikacja raz na
  sekundę. Pin pochodzi z aliasu devicetree `wheel-sensor` w overlayu —
  **domyślnie P1.06** (zmień na pad, który masz wyprowadzony; port musi
  mieć GPIOTE: P0/P1, NIE P2). Podłączenie: element zwierający między pin
  i GND (pull-up jest w SoC).

`src/wheel_source_gpio.c/.h` to źródło obrotów z wejścia impulsowego; to refaktor
modułu z projektu
`spasoye/nrf52840_zephyr_CSC_sensor` (Ivan Spasić, MIT — patrz
`reference/README.md`): debounce przez `k_uptime_get()` w ISR, licznik
`atomic_t`, konwersja ms → 1/1024 s. Źródło akcelerometryczne (biblioteka
sensor + LIS2DH12) dołoży się do tego samego API.

Opcjonalnie **`CONFIG_CSC_SC_CONTROL_POINT=y`** włącza SC Control Point
(`0x2A55`): SET CUMULATIVE VALUE (kalibracja licznika), UPDATE SENSOR
LOCATION i REQUEST SUPPORTED SENSOR LOCATIONS (CSCS v1.0; Apple tego nie
używa, ale np. nRF Connect pozwala to przetestować).

## Zdalne pomiary i kalibracja (BLE)

Build diagnostyczny (`diag.conf`) dodaje MCUmgr/SMP + shell i moduł
`CSC_DIAG` (nagrywanie XYZ + parametry kalibracji):

```
west build -b holyiot_25008/nrf54l15/cpuapp . -- -DEXTRA_CONF_FILE="debug.conf;diag.conf"
```

Pomiary i kalibracja **bez kabla** przez skrypt `tools/wheel_cal.py`
(szczegóły w `tools/README.md`):

```
tools/wheel_cal.py status
tools/wheel_cal.py capture --seconds 10 -o ride1.csv
tools/wheel_cal.py analyze ride1.csv
tools/wheel_cal.py cal-set amp_mg 200 step_mrad 1600
```

## Metoda pomiaru (dlaczego nie sieć neuronowa)

Nie estymujemy prędkości z „charakteru" drgań jak CarSpeedNet (13 h jazdy,
~178 tys. parametrów, MAE 2,6–4,7 km/h): czujnik obraca się razem z kołem,
więc grawitacja tworzy regularny sygnał okresowy i mierzymy **okres obrotu
wprost** (v = C / T) — jak enkoder, tylko bez magnesu. Pierwszy test: ~19
obrotów, rozrzut okresów ~1,9% (w tym realne zwalnianie), przy stabilnym 100 Hz.

Z publikacji bierzemy tylko idee, nie model:

- **okno czasowe zamiast pojedynczej próbki** — filtr medianowy okresu z
  ostatnich 2–4 obrotów (pojedyncze uderzenie nie psuje wyniku),
- **automatyczne rozpoznawanie postoju/ruchu** — stan
  `IDLE → CALIBRATING → LOCKED`,
- **jakość i ufność wyniku** — `plane_q` × spójność okresów → `conf` (0–1).

Nie całkujemy przyspieszenia do prędkości — akcelerometr służy wyłącznie jako
bezmagnesowy czujnik obrotu (faza grawitacji + kontrola dośrodkowa).

## Następne kroki

- [x] Sensor na GPIO (pulse input) — `src/wheel_source_gpio.c`
      (tryb `CSC_WHEEL_SENSOR_GPIO`),
- [x] Potwierdzone na HOLYIOT-25008: boot + advertising 0x1816 (skan BLE),
- [ ] Podłączyć czujnik/zworkę pod wybrany pad (domyślnie P1.06) albo
      zmienić pin w overlayu,
- [x] Akcelerometr jako główne źródło — `src/wheel_source_accel.c`
      (tryb `CSC_WHEEL_SENSOR_ACCEL`, włączany przez `power.conf`): własny wątek
      próbkuje LIS2DH przez sterownik, detektor liczy pełne obroty, wynik idzie
      na CSCS; ten sam wątek steruje snem/wybudzeniem,
- [x] Zdalny pomiar/kalibracja (SMP over BLE) — `diag.conf` + `tools/wheel_cal.py`,
- [x] DFU przez BLE na żądanie — MCUboot (sysbuild) + MCUmgr/SMP; wejście przez
      przytrzymanie `sw0` 5 s (`dfu_button.c`, akcelerujący blink) albo rozkaz SMP;
      sygnalizacja LED: czerwony = DFU (patrz „Sygnalizacja LED” niżej),
- [x] Refaktor „driver-first": moduły rozdzielone (`bt`/`boot`/`sim`/`dfu_*`),
      rejestr/diody przez sterowniki Zephyra; `wheel reg`/`wheel raw` usunięte,
      zastąpione shellem sterownika LIS2DH (`stream.conf`, bench),
- [x] Konfiguracja w pamięci trwałej — `src/wheel_config.c` (settings + NVS na
      partycji `storage`, blob z wersją i magic; bonds BT też w NVS),
- [x] Dwa jednoczesne połączenia (`BT_MAX_CONN=2`) + automatyczne ponowne
      łączenie (`settings_load()` przywraca bonds po restarcie),
- [ ] Pomiar napięcia baterii → `bt_bas_set_battery_level()` (obecnie stała
      `CONFIG_CSC_BAS_LEVEL`, domyślnie 100%; na HOLYIOT brak znanego układu
      pomiaru),
- [ ] `CONFIG_CSC_SENSOR_LOCATION` dostroić do miejsca montażu czujnika,
- [ ] Weryfikacja na sprzęcie: wake z System OFF, próg `CSC_POWER_WAKE_THS`,
      `CSC_POWER_IDLE_TIMEOUT_S` i czułość detektora na prawdziwym kole.

## Pokrycie wymagań

| Wymaganie | Realizacja |
|---|---|
| Wykrywanie pełnych obrotów koła | `wheel_detector.c` — faza grawitacji + kontrola dośrodkowa, samokalibracja |
| Automatyczne wybudzenie po ruszeniu | `wheel_power.c` — INT1 na P1.05, GPIO SENSE + `sys_poweroff()`; po wake restart i reconnect |
| Automatyczne uśpienie po zatrzymaniu | `wheel_power.c` — trzy stopnie: 10 s → czujnik w 1 Hz (połączenie utrzymane), 5 min → rozłączenie + System OFF |
| Standardowy BLE CSC | `csc.c` — 0x1816, pomiary + feature + sensor location |
| Watch + wyświetlacz jednocześnie | `CONFIG_BT_MAX_CONN=2`, advertising trwa dopóki jest wolny slot (`bt.c`) |
| Battery Service | `CONFIG_BT_BAS=y`; poziom z **pomiaru SAADC** (`battery.c`, wewnętrzne wejście VDD), a `CONFIG_CSC_BAS_LEVEL` tylko jako fallback |
| Automatyczne ponowne łączenie | advertising wznawiany po rozłączeniu + bonds z NVS (`settings_load()`) |
| Bezpieczny DFU przez BLE | MCUboot + MCUmgr/SMP z `PERM_RW_ENCRYPT` (`dfu.conf`), obraz podpisany |
| DFU po 5 s przytrzymania | `dfu_button.c` (`CSC_DFU_BUTTON_LONG_MS`, domyślnie 5000) |
| Sygnalizacja LED | `bt.c` (niebieski), `boot.c` (zielony/czerwony), `dfu_button.c` (czerwony) |
| Konfiguracja w pamięci | `wheel_config.c` — settings/NVS, blob wersjonowany |
| Bardzo niski pobór energii | LIS2DH 1 Hz + przerwanie ruchu, System OFF (~µA), advertising 100/150 ms. RAM: 34 kB (base/GPIO), 104 kB (bench), 123 kB (DFU — zawiera recorder 48 kB z `diag.conf`) |

## Sygnalizacja LED

> **Uwaga implementacyjna:** sterownik LED dla GPIO (`led_gpio.c`) implementuje
> tylko `set_brightness` — **nie ma sprzętowego `blink`**. `led_blink()` zwraca
> wtedy `-ENOSYS` i nic nie robi (bez ostrzeżenia w kompilacji!). `led_on/off`
> działają, bo API emuluje je przez `set_brightness`. Dlatego miganie realizuje
> `src/led_sw_blink.c` — `k_work_delayable`, nie blokuje workqueue i da się
> anulować. Dodatkowo `led_on_dt()` **nie zatrzymuje** trwającego migania.

| Dioda | Alias | Znaczenie |
|---|---|---|
| czerwona | `led0` (P2.09) | DFU: blink 500/500 ms = tryb DFU aktywny; 100/100 ms = transfer obrazu; **świeci** = obraz odebrany, czeka na restart; blink 100/100 przez 600 ms = błąd DFU |
| zielona | `led1` (P1.10) | firmware potwierdzony (self-test OK) — 3 błyski po starcie |
| niebieska | `led2` (P2.07) | wyłącznie Bluetooth: 3 błyski = połączono / sparowano, szybki blink = trwa parowanie |

Wejście w DFU: przytrzymaj `sw0`; czerwona dioda blinkuje **coraz szybciej**
(600 → 50 ms w 5 s), po 5 s zapala się na stało i wchodzimy w tryb DFU.
Błąd self-testu przy starcie: naprzemiennie niebieska/czerwona, 4 błyski.

## Zarządzanie energią (trzy stopnie)

Polityka postoju wzorowana na komercyjnych czujnikach prędkości (Garmin/Wahoo:
wybudzenie ruchem → krótka aktywność → głęboki sen po zakończeniu jazdy):

| Stopień | Warunek | Akcelerometr | BLE | SoC |
|---|---|---|---|---|
| 1 — jazda | obroty koła | 100 Hz | CSC ~1 Hz, połączenie aktywne | budzony do pomiaru |
| 2 — postój | `CSC_POWER_IDLE_TIMEOUT_S` (10 s) | 1 Hz + przerwanie ruchu | połączenie **utrzymane** | idle między connection events |
| 3 — koniec jazdy | `CSC_POWER_DEEP_SLEEP_TIMEOUT_S` (5 min) | 1 Hz + przerwanie ruchu | rozłączenie | **System OFF** |

Stopień 3 jest warunkiem koniecznym roku pracy z CR2032: bez niego połączony
zegarek trzymałby urządzenie przebudzone przez całą dobę. Pierwszy obrót koła
wybudza układ (INT1 → P1.05), a zegarek łączy się sam z zapisanego bonda.

> Stopień 2 **nie** zrywa połączenia — zatrzymanie na światłach nie może
> kosztować rowerzysty kontaktu z zegarkiem. To zresztą najtańszy stan
> energetycznie: radio budzi się tylko na krótkie connection events, a CPU
> pozostaje w `idle`.

> Licznika bezczynności **nie** resetujemy przy połączeniu klienta — inaczej
> zegarek zostawiony na rowerze trzymałby urządzenie w nieskończoność.

## Pomiar baterii (CR2032)

CR2032 zasila bezpośrednio VDD, a nRF54L15 potrafi **wewnętrznie** podać VDD na
kanał SAADC — bez dzielnika, bez dodatkowego pinu, bez lutowania. Nie trzeba do
tego żadnej biblioteki Nordic: cały tor to sterownik Zephyra
(`adc_nrfx_saadc`) + publiczne API (`adc_channel_setup_dt`, `adc_sequence_init_dt`,
`adc_read_dt`, `adc_raw_to_millivolts_dt`).

Konfiguracja kanału jest w overlayu płytki (`boards/holyiot_25008_nrf54l15_cpuapp.overlay`):

```
zephyr,gain = "ADC_GAIN_1_4";          /* 0,9 V / (1/4) = 3,6 V pełnej skali */
zephyr,reference = "ADC_REF_INTERNAL"; /* referencja wewnętrzna = 900 mV */
zephyr,input-positive = <NRF_SAADC_VDD>;
zephyr,resolution = <12>;
zephyr,oversampling = <4>;             /* 2^4 = 16 uśrednień sprzętowych */
```

> **Dlaczego gain 1/4:** referencja wewnętrzna nRF54L15 to **0,9 V**
> (`ANALOG_REF_INTERNAL_VAL = 900` dla `NRF54L_SERIES`). Przy `ADC_GAIN_1` pełna
> skala to 0,9 V, więc świeża CR2032 (3,0–3,3 V) wyszłaby jako nasycenie.
> Przy 1/4 pełna skala to 3,6 V i ogniwo się mieści.

> **Uwaga implementacyjna:** na nRF54L15 VDD wybiera się **innym mechanizmem** niż
> na nRF52 (`PSELP.CONNECT = Internal` + `PSELP.INTERNAL = Vdd`, a nie
> `PSELP.PSELP = VDD`). Wartość w devicetree jest ta sama (`NRF_SAADC_VDD` = 128),
> a sterownik ma `BUILD_ASSERT`, że oba kodowania się zgadzają — więc w DT piszemy
> `NRF_SAADC_VDD` niezależnie od rodziny.

**Kadencja pomiaru** (`battery.c`): po starcie od razu, potem co
`CONFIG_CSC_BATTERY_INTERVAL_RIDING_S` (domyślnie 5 min) podczas jazdy i co
`CONFIG_CSC_BATTERY_INTERVAL_IDLE_S` (domyślnie 1 h) na postoju. Rozpoznanie
„jazda/postój" idzie z `wheel_power_is_standby()`, czyli z tego samego sygnału,
który steruje uśpieniem.

**Kalibracja:** procent jest zawsze orientacyjny (krzywa CR2032 zależy od
obciążenia). Napięcie natomiast można dotrymować jednym współczynnikiem —
porównaj `wheel battery` z multimetrem i ustaw `CONFIG_CSC_BATTERY_CAL_PPM`.

Diagnostyka (bench): `wheel battery` → `battery: 3012 mV, 90% (last published 90%)`.

> **Ważne przy testach:** J-Link nie może zasilać płytki. Jego VTref ma tylko
> *mierzyć* napięcie celu — inaczej SAADC zmierzy szynę debuggera, nie CR2032.

## Warianty buildów

Dwa obrazy **produkcyjne**, różniące się tym, jak urządzenie zasypia. Reszta
(polling pojedynczej próbki, detektor, CSCS) jest w obu ta sama.

| Wariant | Komenda (`-DEXTRA_CONF_FILE=`) | Płytka | Co śpi po 300 s |
|---|---|---|---|
| **stock** | `debug.conf;diag.conf;stock.conf;dfu.conf` + `--sysbuild` | **fabryczna, bez przeróbek** | radio |
| **production** | `debug.conf;diag.conf;power.conf;dfu.conf` + `--sysbuild` | z drucikiem INT1 → P1.05 | SoC (System OFF) |

Warianty pomocnicze (nie do wypuszczania):

| Wariant | Komenda | Uwagi |
|---|---|---|
| baza (symulacja) | — | tylko `prj.conf`; `CONFIG_CSC_SIMULATE=y` |
| gpio | `debug.conf;gpio.conf` | czujnik Halla / kontaktron |
| bench | `debug.conf;diag.conf` | MCUmgr/SMP + shell `wheel` |
| bench + FIFO | `debug.conf;diag.conf;stream.conf` | dodatkowo shell sterownika `lis2dh` |

### Dlaczego dwa warianty i dlaczego akurat tak

Na fabrycznym module **nie ma czym wybudzić głębokiego snu**, i to jest fakt
z devicetree SoC, nie ostrożność:

| Linia | Pin | GPIOTE / SENSE |
|---|---|---|
| LIS2DH INT1 | P2.00 (legacy trace) | ❌ port P2 nie ma `gpiote-instance` |
| LIS2DH INT2 | P2.03 | ❌ to samo |
| przycisk | P1.13 | ✅ ale wymaga człowieka |

Na nRF54L15 port 2 nie ma instancji GPIOTE — `gpiote30` należy do `gpio0`,
`gpiote20` do `gpio1`, a `gpio2` nie ma tej właściwości wcale. System OFF nie ma
na tym układzie wybudzania timerem, więc jedyne źródła to GPIO SENSE, NFC,
analog i RESET. Skoro nie ma pinu, **wariant stock nie może wchodzić
w System OFF** — wszedłby i nigdy nie wrócił.

Wariantu stock nie ratuje zmiana `CSC_POWER_WAKE_INT2`: INT2 też siedzi na P2.

Dlatego tier 3 ma dwa zakończenia:

| | stock | production |
|---|---|---|
| po 300 s | `bt_prepare_sleep()` — radio off | `sys_poweroff()` — System OFF |
| tryb SoC | **System ON Idle** (CPU w WFI, RAM/LFCLK/GRTC działają) | wyłączony |
| sensor | 1 Hz (już od tier 2) | 1 Hz |
| co budzi | **polling 1 Hz** (`STANDBY_POLL_US`) | INT1 na P1.05, restart układu |
| autostart bez przycisku | tak, do ~1 s | tak |

> **System ON Idle to opis, nie przełącznik.** Na nRF54L15 to po prostu
> `arch_cpu_idle()` → `__WFI()`. Jedyny pokrewny symbol to
> `CONFIG_SOC_NRF_FORCE_CONSTLAT`, domyślnie wyłączony — włączenie go
> **zwiększa** pobór (stała latencja budzenia kosztuje).
>
> **Nie sięgaj po `CONFIG_PM`.** nRF54L15 nie selektuje `HAS_PM` i nie ma
> `pm_state_set()` (mają je tylko nrf54h i nrf92), więc `CONFIG_PM=y` **nie
> wchodzi**. Kconfig to zgłasza — `PM` „was assigned the value 'y' but got the
> value 'n'" — ale build kończy się `exit 0` i w `.config` jest `PM=n`. Da się
> to przeoczyć, bo jedynym sygnałem jest ostrzeżenie Kconfig; nic się nie psuje
> i nic się nie dzieje.

Rdzeń zostaje w System ON idle i próbkuje co 1 s. To, co naprawdę zjada
ogniwo, to radio — reklamowanie co 100 ms plus podtrzymywane łącza to dziesiątki
µA, podczas gdy rdzeń w idle między dwoma pollami to kilka. Wariant stock jest
więc droższy o kilka µA, ale **nie wymaga lutowania**.

> **Nie zweryfikowane na sprzęcie:** o ile dokładnie droższy. Pomiar poboru dla
> obu wariantów jest do zrobienia i powinien trafić do planu testów, zanim ktoś
> obieca „rok na CR2032" dla stocka.

> `power.conf` i `stock.conf` różnią się jednym: pierwszy ustawia
> `CSC_POWER_WAKE_LINE_WIRED=y`, a na tym symbolu wisi `CSC_POWER_SYSTEM_OFF`.
> **System OFF bez zadeklarowanego drucika to błąd Kconfig**, nie cicha pomyłka:
> wymuszenie `SYSTEM_OFF=y` przy `stock.conf` daje `n` w `.config` i ostrzeżenie
> Kconfig nazywające symbol.
>
> Bramka chroni przed złą **konfiguracją**, nie przed wgraniem złego **pliku**:
> obraz `production` na fabrycznej płytce nadal wejdzie w System OFF i nie
> wstanie. Dlatego oba pliki mają to w nagłówku.

> **Nie łącz `stream.conf` z `power.conf`** w jednym buildzie: `stream.conf`
> rejestruje handler przerwania sterownika na linii INT1, której `wheel_power`
> używa do uzbrojenia wybudzania z System OFF — oba na raz walczyłyby o ten pin.
> Gdy FIFO wejdzie do produkcji, wymaga to jawnego uzgodnienia kolejności
> (patrz plan).


## Kontekst gałęzi

Projekt żyje na gałęzi `speed-meter` w checkoucie forka Zephyra
(`sirfragles/zephyr`) — gałąź celowo zaczyna się od pustego commita, żeby
historia projektu była czysta. Kod roboczy LIS2DH FIFO pozostaje na gałęziach
`test/dev` i `dev` (oraz w worktree `upstream/lis2dh`, `upstream/rtio`).
