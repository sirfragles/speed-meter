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

**`verify10` never locks — the movement gate latches itself off.** The
instrumentation in `det.stats`, printed by the replay, says the plane fit is
never even attempted: 0 attempts, 0 successes, 0 phase resets. The detector
never leaves IDLE.

The cause is an estimate that feeds on its own output:

```c
gate   = max(4 * noise_ms2, still_gate_ms2);
moving = offset_from_gravity(v) > gate;   /* below the gate counts as "still" */
update_still(v);                          /* noise = peak-to-peak of those */
```

If the wheel starts turning while the first 50-sample "still" window is open,
that window catches part of the rotation, the peak-to-peak comes out at
6.42 m/s², and the gate becomes 4 × 6.42 = 25.69 m/s² — larger than the whole
gravity circle (at most ~2 g ≈ 19.6 m/s²). After that nothing can be classified
as moving again.

Measured across the four captures:

| Capture | moving samples | still windows | noise | gate | plane attempts |
| --- | ---: | ---: | ---: | ---: | ---: |
| `autocal_capture` | 1723/1725 | 0 | 0.00 | 0.50 | 1 |
| `ride1` | 2167/2179 | 0 | 0.00 | 0.50 | 1 |
| `verify10` | **48/1710** | 33 | **6.42** | **25.69** | **0** |
| `verify_manual` | 3669/4096 | 0 | 0.00 | 0.50 | 1 |

The replay also prints the largest per-axis excursion over the whole capture —
the same quantity the still window measures, but over everything instead of
the last 50 samples. It exists to answer one question: was that window open
through rest or through motion?

| Capture | whole capture, per axis (mg) | still window saw |
| --- | --- | ---: |
| `autocal_capture` | x=2624 y=2484 z=2624 | 0 |
| `ride1` | x=1932 y=2073 z=3064 | 0 |
| `verify10` | x=844 y=1311 **z=1827** | **655** |
| `verify_manual` | x=1921 y=1405 z=2624 | 0 |

For `verify10` the window saw 655 mg against a full range of 1827 mg: a third
of the capture's excursion, roughly two orders of magnitude above sensor rest
noise. It was open while the wheel was turning. The three captures that lock
report 0 — their windows closed on a genuinely stationary wheel, which is why
they never latch.

An earlier version of this file said "it is not a gate problem: the samples *do*
exceed the gate". That was measured against the raw sample-to-sample change,
which is not what the gate compares — it compares against the running mean
gravity. The claim was wrong; this is the correction.

Not fixed here. The fix changes detector behaviour, and the plan puts that
behind a recording with independently counted revolutions — otherwise there is
no way to tell an improvement from a different wrong answer.

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
