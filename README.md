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
| 3 — ride over | 5 min without a revolution | 1 Hz | links dropped | **System ON Idle** |

Stopping at a traffic light must not cost the rider the watch connection, so
tier 2 keeps the link up. Tier 3 is as deep as this board can go: the links and
the advertising stop, the CPU idles between polls, and the 1 Hz wheel poll is
its own wake source — the wheel starts the device again by itself, with nothing
pressed and nothing soldered. Why not System OFF is in the configuration
section below.

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
| **production** | `debug.conf;diag.conf;power.conf;dfu.conf` | **yes** | magnetless source, three-tier standstill policy, DFU |

> `stream.conf` registers a driver interrupt handler on INT1, which
> `power.conf` uses for its standstill interrupt — do not combine them.

### There is one production image, and no way to wake from System OFF

The wheel still starts the device by itself, but by polling rather than by a
hardware event, and the reason is a property of the board rather than a
preference:

| | |
|---|---|
| Tier 2, 10 s | sensor to 1 Hz, BLE link kept |
| Tier 3, 300 s | links dropped, advertising stopped, **System ON Idle** |
| Chip while parked | CPU in WFI; RAM, LFCLK, GRTC running |
| What wakes it | the 1 Hz poll, from the sampling thread |

Waking from System OFF needs GPIO DETECT on P0/P1 — there is no timer wake on
this part — and the module wires the accelerometer's INT1 and INT2 to P2.00 and
P2.03. Port 2 carries no `gpiote-instance` in the SoC devicetree (`gpiote30`
belongs to `gpio0`, `gpiote20` to `gpio1`), and the board cannot usefully be
modified, so there is no wake source at all. Arming one would power the device
down and never bring it back.

The cost is battery life, not function: advertising every 100 ms plus a
maintained link runs to tens of microamps, while the core idling between two
1 Hz polls costs a few. **That gap is unmeasured** — section 8 of
`app/TEST-PLAN.md` specifies how to measure it, and whether buying it back is
worth a button press is a decision for after that measurement.

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
- **P1.05 is UART RX, and wiring INT1 to it is not available.** It would take a
  physical modification of the board, and the board cannot usefully be modified.
  Without that wire there is no wake source for System OFF, so tier 3 stays in
  System ON Idle and polls instead — see the configuration section. `uart20` is
  disabled everywhere and logging goes over RTT (J-Link).

---

## License

Apache-2.0. The GPIO wheel source in `app/src/wheel_source_gpio.c` is derived
from the `spasoye/nrf52840_zephyr_CSC_sensor` project (MIT); see
`app/reference/`.
