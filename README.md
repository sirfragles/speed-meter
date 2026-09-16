# Speed Meter — magnetless BLE cycling speed sensor

Firmware for a Bluetooth Cycling Speed and Cadence sensor (CSCS, service UUID
`0x1816`) running on a **HOLYIOT-25008** module (Nordic **nRF54L15**). Wheel
revolutions come from the **accelerometer** — no magnet, no Hall sensor on the
wheel — and the device is built to run for a season on a **CR2032**.

**[→ How it works](#how-it-works) · [Building](#building) · [Flashing](#flashing) · [Releasing](#releasing)**

---

## How it works

A spinning accelerometer sees gravity rotate once per wheel turn. A
self-calibrating detector counts those rotations, so the board may be mounted in
any orientation: the rotation plane, the axis signs and the sensor-to-axis
distance are all derived from the data.

```
LIS2DH12 ──SPI──> revolution detector ──> CSCS 0x1816 ──BLE──> Apple Watch,
(100 Hz)          (gravity phase)                          bike computer
                        │
                        └──> power state machine (sleep / wake)
```

### Power state machine

This is what makes a coin cell last. A connected client would otherwise keep the
device awake around the clock.

| Tier | Trigger | Accelerometer | BLE | SoC |
|---|---|---|---|---|
| 1 — riding | wheel turns | 100 Hz | CSC ≈ 1 Hz | woken per sample |
| 2 — standstill | 10 s without a revolution | 1 Hz + motion interrupt | link **kept** | idle between events |
| 3 — ride over | 5 min without a revolution | 1 Hz + motion interrupt | links dropped | **System OFF** |

Stopping at a traffic light must not cost the rider the watch connection, so
tier 2 keeps the link up. Tier 3 is the deep sleep: the first wheel movement
wakes the chip through INT1 (P1.05) and the watch reconnects from its stored
bond on its own.

### Battery

The CR2032 feeds VDD directly, and the nRF54L15 SAADC can measure VDD through an
internal channel — no divider, no extra pin, nothing to solder. The voltage is
published as a percentage through the standard BLE Battery Service.

### Also included

- **Two simultaneous centrals** (watch + bike computer), both fed by the same
  `CSC Measurement` notifications.
- **On-demand DFU**: hold the button for 5 s, the device advertises as
  `Wheel DFU` and a phone uploads a signed image over BLE (MCUboot + MCUmgr).
- **Configuration in flash** (settings/NVS): wheel circumference, output data
  rate, range and detector thresholds survive a battery change.
- **LED signalling**: red = DFU, green = firmware confirmed, blue = Bluetooth.

The full feature list and implementation notes are in [`app/README.md`](app/README.md).

---

## Publishing (first time)

The repository does not exist on GitHub yet. Everything is ready to push:

```sh
cd speed-meter
git init -b main
git add .
git commit -m "Speed Meter: magnetless BLE CSCS firmware for HOLYIOT-25008"
gh repo create sirfragles/speed-meter --public --source=. --push
```

The `Build` workflow then runs on every push and pull request. Cut a release
with:

```sh
git tag v1.0.0
git push origin v1.0.0
```

---

## Repository layout

This repository is the **west manifest repository** (T2 topology): `west.yml`
describes the workspace, and the firmware lives in `app/`.

```
west.yml                  pinned Zephyr revision + modules
app/                      the firmware
├── CMakeLists.txt        Zephyr application
├── Kconfig               project options (CSC_*)
├── prj.conf              base configuration
├── debug.conf            RTT logging over J-Link
├── diag.conf             bench: MCUmgr/SMP + `wheel` diagnostics shell
├── stream.conf           bench: LIS2DH FIFO/RTIO stack (needs the fork)
├── gpio.conf             wheel source: GPIO pulse input
├── power.conf            production: accelerometer source + sleep/wake
├── dfu.conf              production: encrypted DFU over BLE
├── boards/               board devicetree overlay (SAADC battery channel, ...)
├── sysbuild/             MCUboot configuration
└── src/                  a module per concern, see app/README.md
tests/wheel_detector/     replay tests for the detector (native_sim)
scripts/                  CI helpers (footprint summary)
```

### Zephyr dependency

`west.yml` pins Zephyr to a commit on the project fork
(`sirfragles/zephyr`), because the LIS2DH FIFO work the diagnostics build uses
is not upstream yet: `lis2dh_fifo.c`, `lis2dh_stream.c`, `lis2dh_rtio.c`, the
`LIS2DH_STREAM` / `LIS2DH_FIFO_POLL` / `LIS2DH_SHELL` options and the FIFO
watermark binding.

The **production** build does not use any of that and also builds against a
stock Zephyr — only `app/stream.conf` needs the fork.

---

## Building

No local SDK needed: the same container image is used for local builds and CI,
so the toolchain cannot drift.

```sh
west init -m https://github.com/sirfragles/speed-meter --mr main
cd speed-meter
west update

# Production firmware (accelerometer source, sleep/wake, MCUboot, DFU)
west build --sysbuild -b holyiot_25008/nrf54l15/cpuapp app --pristine=always -- \
    -DEXTRA_CONF_FILE="debug.conf;diag.conf;power.conf;dfu.conf"
```

Or entirely in the container, which is what CI does:

```sh
docker run --rm -v "$PWD":/workdir ghcr.io/zephyrproject-rtos/zephyr-build:main \
  sh -c 'cd /workdir && west build -b holyiot_25008/nrf54l15/cpuapp app --pristine=always'
```

### Configurations

| Name | `-DEXTRA_CONF_FILE` | `--sysbuild` | What it is |
|---|---|---|---|
| base | *(none)* | no | simulation only, runs with no hardware |
| gpio | `debug.conf;gpio.conf` | no | Hall sensor / reed contact on a GPIO |
| bench | `debug.conf;diag.conf` | no | MCUmgr/SMP + `wheel` diagnostics shell |
| bench-fifo | `debug.conf;diag.conf;stream.conf` | no | adds the LIS2DH FIFO shell |
| **production** | `debug.conf;diag.conf;power.conf;dfu.conf` | **yes** | magnetless source, sleep/wake, DFU — **needs the INT1 wire** |
| **stock** | `debug.conf;diag.conf;stock.conf;dfu.conf` | **yes** | the same firmware for a board with **no modifications** |

> `stream.conf` registers a driver interrupt handler on INT1, which
> `power.conf` uses for System OFF wake arming — do not combine them.

### Which one to flash

The two production variants differ in exactly one thing, and it is a hardware
fact rather than a preference: whether the LIS2DH12 INT1 is wired to P1.05.

| | `production` | `stock` |
|---|---|---|
| INT1 wire | required | none |
| Tier 2 (sensor 1 Hz, link kept) | yes | yes |
| Tier 3, parking | System OFF, wake on rotation | **System ON Idle**, radio off |
| Chip while parked | powered down | CPU in WFI; RAM, LFCLK, GRTC running |
| What wakes it | INT1 on P1.05, a reboot | the 1 Hz poll, from the sampling thread |
| Battery | a year on a CR2032 | a few µA more; the radio, not the core, is the load |

`stock.conf` cannot reach System OFF even by accident: the option depends on
`CSC_POWER_WAKE_LINE_WIRED`, which only `power.conf` sets, so setting it on an
unmodified board is a Kconfig error rather than a device that powers down and
never comes back.

That gate protects against a wrong *configuration*, not against flashing the
wrong *file*: a `production` image on an unmodified board still goes to System
OFF. Both file headers say so, and the failure is one reset away from recovery
rather than permanent.

---

## Testing

Everything under `tests/` runs on the host — no board, no cross toolchain:

```sh
west twister -T tests -p native_sim/native/64
```

| Suite | What it covers |
|---|---|
| `wheel_detector` | the revolution detector, replaying motion recorded from a real board |
| `wheel_csc` | the CSCS wire state: event-time clock and cumulative counter |
| `wheel_reconfig` | the fail-closed gate around sensor reconfiguration |
| `wheel_source` | the sampling loop, against a controllable fake accelerometer |

The first three are pure logic, so they are deterministic. `wheel_source` goes
through the Zephyr sensor API: it supplies a fake driver (`vnd,fake-accel`) that
can report a fresh sample, `-ENODATA` or a failing bus, and drives the sampling
loop one pass at a time instead of racing its own thread.

### What the tests are evidence of

Not every suite proves the same thing, and it matters which is which.

- Each fix in this series was **written to fail first** against the behaviour
  it replaces, and the failing output is recorded in the commit that fixed it.
  That includes the fetch-classification tests in `wheel_source`, which fail
  against the old "every outcome is the same" handling.
- `wheel_detector`'s recorded captures pin the detector's *current* behaviour,
  not a known-correct revolution count. They catch any change; they do not yet
  prove the count is right. That needs a recording with independently counted
  revolutions — see `tests/wheel_detector/README.md`.
- Two defects are recorded rather than fixed, because they need that recording:
  `verify10` never locks, and the radius fit does not converge.

### What CI runs

| Job | What it checks |
|---|---|
| `host tests` | every suite under `tests/` on `native_sim` |
| `build` | all five configurations; a ROM/RAM report lands in the run summary |
| `Release` | on a `v*` tag, publishes the three firmware images |

Every action is pinned to a commit SHA, the way Zephyr pins its own workflows.

---

## Flashing

**Over SWD (J-Link)** — the merged image contains bootloader + signed
application:

```sh
west flash -d build
# or, with the J-Link tools directly:
nrfjprog --program build/merged_holyiot_25008_nrf54l15_cpuapp.hex --chiperase --reset
```

**Over BLE (DFU)** — hold the button for 5 s (the red LED blinks faster and
faster), then upload the signed image with any MCUmgr client:

```sh
mcumgr --conntype ble --connstring peer_name='Wheel DFU' image upload \
    build/app/zephyr/zephyr.signed.bin
```

The device must be powered from its own cell during the transfer: a J-Link
supplying the board would make the SAADC measure the debugger's rail instead of
the battery.

---

## Releasing

```sh
git tag v1.0.0
git push origin v1.0.0
```

The `Build` workflow builds every configuration, then the `Release` job attaches
three files to the GitHub release:

| Asset | Use |
|---|---|
| `v1.0.0-full.hex` | whole device over SWD — MCUboot + signed application |
| `v1.0.0-dfu.bin` | upload over BLE with an MCUmgr client |
| `v1.0.0-dfu.hex` | the same image in Intel HEX form |

Zephyr itself is pinned to a commit in `west.yml`, so a tag always rebuilds the
same firmware. Bump the pin deliberately to pick up driver changes.

---

## Hardware

| | |
|---|---|
| Module | HOLYIOT-25008 (nRF54L15, 1.5 MB RRAM, 256 kB RAM) |
| Accelerometer | LIS2DH12 over SPI (SCK P2.01, MOSI P2.02, MISO P2.04, CS P2.05) |
| Wake line | LIS2DH12 INT1 → **P1.05** — needs a board modification, see below |
| Button | P1.13 |
| LEDs | red P2.09, green P1.10, blue P2.07 |
| Power | CR2032 on VDD, measured through the internal SAADC channel |

### Ground rules

- **Never short a wheel sensor to a P2 pin.** Port 2 on nRF54L15 has no GPIOTE
  and no SENSE/DETECT, so it can neither raise an interrupt nor wake the chip —
  P2 is polling-only.
- **P1.05 is UART RX.** Driving the LIS2DH12 INT1 into it requires a **physical
  modification of the board** — it is not how the module ships. Without that
  wire everything works except the wake from System OFF: tier 3 of the power
  state machine will not come back on its own. `uart20` is disabled everywhere
  and logging goes over RTT (J-Link), so a bootloader that enables UART cannot
  fight the INT1 push-pull output. **Flash the `stock` variant if you have not
  made the modification**, and `production` if you have.

---

## License

Apache-2.0. The GPIO wheel source in `app/src/wheel_source_gpio.c` is derived
from the `spasoye/nrf52840_zephyr_CSC_sensor` project (MIT); see
`app/reference/`.
