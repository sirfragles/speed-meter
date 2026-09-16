# Wheel detector replay tests

Unit tests for `app/src/wheel_detector.c`. The detector makes no Zephyr calls on
purpose, so these tests compile the production source file as it ships and
replay recorded wheel motion through it. What runs here is the detector itself,
not a model of it.

## Running

```sh
west twister -T tests/wheel_detector -p native_sim/native/64
```

To keep the build around for inspection:

```sh
west build -b native_sim/native/64 tests/wheel_detector -t run
```

## The captures

`fixtures/*.csv` are recordings of real wheel motion taken over BLE from a
HOLYIOT-25008 with `app/tools/wheel_cal.py`. Each file carries the metadata the
test needs (sample rate, full scale) in its `#` header, and
`tools/csv_to_header.py` embeds the samples at build time - so the test needs no
filesystem and no working directory.

| Fixture | Samples | Peak-to-peak | What it is |
| --- | ---: | ---: | --- |
| `autocal_capture` | 1725 | 2.6 g | taken while the detector was calibrating |
| `ride1` | 2179 | 3.1 g | a ride |
| `verify10` | 1710 | 1.8 g | a verification capture |
| `verify_manual` | 4096 | 2.6 g | manual verification |

Replayed with the production geometry from `app/src/wheel_config.c` (28" wheel,
100 Hz, 10 mm nominal sensor-to-axis distance):

| Fixture | Revolutions | Locks at | Speed |
| --- | ---: | ---: | ---: |
| `autocal_capture` | 16 | sample 388 | 9.3 km/h |
| `ride1` | 19 | sample 330 | 6.6 km/h |
| `verify10` | **0** | never | — |
| `verify_manual` | 22 | sample 482 | 3.1 km/h |

Those numbers live in the `expectations` table in `src/main.c`. They are
characterisation values, not targets: a change there means the detector's
behaviour changed, which has to be explained rather than quietly re-recorded.

## Two things these tests found

Both need a hardware session with a known revolution count to settle.

**`verify10` never locks.** The capture holds real motion — 1.8 g peak-to-peak,
with 61% of samples moving faster than the detector's 0.5 m/s² movement gate —
yet the detector ends in IDLE having counted nothing. The other three captures
lock within 400 samples. Recorded as `locks = false` so a passing suite cannot
hide it.

It is not a gate problem: the samples *do* exceed the gate. Something later in
the path — the plane fit quality, or the phase unwrap — is rejecting the data.
The capture's peak movement (651 mg) is well below the other three
(1893–2618 mg), which is the first thing to look at.

**The radius fit does not converge to anything physical.** The fitted
sensor-to-axis distance comes out as 0.49 m, 0.094 m and 0.0003 m across the
three captures that lock, against a nominal 10 mm and a wheel radius of 334 mm.
The test prints it but does not assert it, because pinning a wrong value as
"expected" would be worse than leaving it visible in the log.

## Adding a capture

```sh
app/tools/wheel_cal.py scan
app/tools/wheel_cal.py capture --seconds 20 -o tests/wheel_detector/fixtures/ride2.csv
```

Add a row to the `expectations` table in `src/main.c` using what the test
prints, then run the suite. `west twister` and the CMake glob pick the new file
up on their own.
