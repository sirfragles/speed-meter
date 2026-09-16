# Wheel source integration tests

Integration tests for `app/src/wheel_source_accel.c` — the path between the
sensor driver and the CSCS wire. The unit tests on either side cannot reach it:
the sensor API, the reconfiguration, the discontinuity handling and the sampling
cadence all live inside its thread.

## How it works

The source keeps its collaborators simple enough to control:

| Real | Here | Why |
|---|---|---|
| the LIS2DH12 driver | a fake (`vnd,fake-accel`) | it must be able to report a fresh sample, `-ENODATA` or a failing bus, and to reject the ODR and the full scale independently |
| `wheel_config_get()` | a stub the test writes to | NVS persistence is not what is under test |
| `csc_publish_wheel()` | a stub that records | the test asserts on what went on the wire |
| `wheel_detector.c`, `wheel_csc.c`, `wheel_reconfig.c` | built for real | they are part of the path |

The sampling thread is **not started**: `wheel_source_accel.c` omits
`K_THREAD_DEFINE` under `CONFIG_ZTEST`, and the test calls
`wheel_source_accel_step()` itself. A thread racing the scheduler and the kernel
clock is not something a test can make assertions about.

## Running

```sh
west twister -T tests/wheel_source -p native_sim/native/64
```

## What each test is evidence of

The cadence test (`test_a_stale_deadline_is_rebased_not_chased`) fails against
the behaviour it replaces — the deadline stayed in the past
(`next=10000, now=60000`) and the following passes would have fetched back to
back.

The two fetch-classification tests fail against the old handling too: revert to
"every outcome is the same" and they report `stats.lost == 0` and
`stats.no_data == 0`. They do need the counters to exist first, so they cannot
run against the code as it was *before* the counters — only against the old
*behaviour*.

## If you extend this

- The fake driver must use `DEVICE_API(sensor, ...)` and
  `SENSOR_DEVICE_DT_INST_DEFINE()`. A plain `const struct sensor_driver_api`
  compiles and links, then trips the sensor subsystem's section assertion at
  runtime.
- The suite's `before` hook re-runs `wheel_source_accel_init()`. The
  reconfiguration gate is static and would otherwise carry over between tests,
  and a later test would find the sensor already configured and see no
  `attr_set()` calls at all.
- `vnd` is Zephyr's registered stand-in vendor prefix. Declaring a project
  prefix of your own needs a correctly tab-separated `vendor-prefixes.txt`.
