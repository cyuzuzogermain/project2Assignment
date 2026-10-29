# Custom Callback: Sensor Drift Detector

**Project:** Dynamic IoT Event Monitoring and Callback Engine
**Student ID:** 12345
**Custom Callback:** Sensor Drift Detector
**Implementation:** `main.c` — `static void sensor_drift_detector(struct Device *device)`

---

## 1. Purpose

The three built-in monitors (`temperature_monitor`, `pressure_monitor`, `battery_monitor`)
answer one question: *is the current reading inside the normal range?*

That question alone misses an important class of faults. A sensor can stay **inside**
the normal range while jumping from one extreme of that range to the other between two
consecutive readings — a disconnected probe that snaps back to ambient, a replaced
battery pack, a line that was depressurised and repressurised. Every single reading looks
"normal", so the type monitors stay silent, yet the measurement stream is clearly not
continuous.

The **Sensor Drift Detector** closes that gap. It compares the current reading with the
previous reading **of the same device** and raises an alert when the change between two
successive samples exceeds a configurable fraction of the device type's simulated span —
regardless of whether either sample is inside the normal range.

It is a *custom* callback in the same sense as the built-in monitors:

* identical signature `void (*)(struct Device *)`,
* selected and invoked through a function pointer — `process_device(d, sensor_drift_detector)` —
  never by a hard-coded call or an `if/else` chain,
* it runs **after** the type callback, so a reading can legitimately produce two alerts
  (one range alert + one drift alert),
* it only reads its input through the single type-checked accessor `device_value()`,
  so it never touches a union member that does not match `device->type`.

---

## 2. Conditions

| # | Condition | Action |
|---|-----------|--------|
| 1 | `device == NULL` or `device->type` is not a valid enum value | return immediately, nothing is changed |
| 2 | `device->has_previous == 0` (first reading this device ever produced) | store `current` into `previous_value`, set `has_previous = 1`, return — **no drift check is possible yet**, no alert |
| 3 | `delta = fabs(current - previous) <= limit` | no alert; `previous_value` is updated to `current` |
| 4 | `delta = fabs(current - previous) > limit` | raise an alert: `STATUS_ALERT`, `alert.triggered = 1`, reason shows delta **and** limit, range text `"max change <limit> <unit>"`, `callback_name = "sensor_drift_detector"`, `value = current` |
| 5 | Always (end of the function, in every path that had a previous value) | `previous_value = current` — the reference always slides forward |

Notes:

* The device's **normal-range status is untouched** by this callback: if the type monitor
  found the value normal, the status only becomes `STATUS_ALERT` because of the drift.
  That is exactly what makes alert #2 in `callback_dispatch_evidence.txt`
  (`25.00 C -> 60.00 C`) a *drift-only* alert.
* Drift is symmetric: a jump **up** and a jump **down** are treated identically
  (`fabs`).
* The previous value is kept **per device** in `Device.previous_value` /
  `Device.has_previous`, so it survives menu operations and is lost only when that
  device is removed (a re-registered device starts with a fresh baseline).

---

## 3. Algorithm

```
sensor_drift_detector(device):
    if device is NULL or device->type is not a valid type: return

    current = device_value(device)            # single accessor, checks the type
    unit    = unit_of(device->type)           # "C", "kPa" or "V"

    if not device.has_previous:               # first sample: just remember it
        device.previous_value = current
        device.has_previous   = true
        return

    previous = device.previous_value
    delta    = fabs(current - previous)                    # |change|
    limit    = DRIFT_LIMIT_PCT * (sim_max(type) - sim_min(type))   # 0.20 * span

    if delta > limit:
        device.status = STATUS_ALERT
        device.alert.triggered    = 1
        device.alert.callback_name = "sensor_drift_detector"
        device.alert.value        = current
        device.alert.unit         = unit
        device.alert.reason       = "drift delta <delta> <unit> exceeds limit <limit> <unit> (previous <previous>)"
        device.alert.range_text   = "max change <limit> <unit>"

    device.previous_value = current           # always update, whatever happened
```

The engine around it (per reading, in `handle_process` / `handle_autosim`):

1. `device->status = STATUS_NORMAL` — the engine resets the status before each reading;
2. `process_device(d, TYPE_CALLBACKS[d->type])` — type monitor through the dispatch table;
3. if `alert.triggered`: log a copy of the alert, `alerts_by_type[type]++`, clear `triggered`;
4. `drift_checks++`; `process_device(d, sensor_drift_detector)` — the custom callback;
5. if `alert.triggered`: log it, `alerts_by_type[type]++`, `drift_alerts++`, clear `triggered`;
6. `processed++`; the reading counts as **ABNORMAL** if either callback alerted, otherwise
   **NORMAL** (so `normal + abnormal = processed` always holds).

---

## 4. Worked example (exactly the test run in `callback_dispatch_evidence.txt`)

Device `ID=2  temp-south  (Temperature)`, limits: simulated span `100 - (-10) = 110 C`,
`limit = 0.20 x 110 = 22.00 C`, normal range `20 .. 70 C`.

**Reading 1 — option 9 sets `25.00 C`, then option 5 processes it**

| step | value |
|------|-------|
| reset status | `STATUS_NORMAL` |
| `temperature_monitor(25.00)` | `20 <= 25 <= 70` → no alert |
| drift detector: `has_previous == 0` | store `previous_value = 25.00`, `has_previous = 1`, return |
| result | `[process] ID=2 ... 25.00 C -> NORMAL (temperature_monitor)` |

**Reading 2 — option 9 sets `60.00 C`, then option 5 processes it**

| step | value |
|------|-------|
| reset status | `STATUS_NORMAL` |
| `temperature_monitor(60.00)` | `20 <= 60 <= 70` → **no alert** (still inside the normal range) |
| drift detector: `current = 60.00`, `previous = 25.00` | `delta = fabs(60.00 - 25.00) = 35.00 C` |
| compare | `35.00 > 22.00` → **alert** |
| alert fields | `callback = sensor_drift_detector`, `value = 60.00 C`, `reason = "drift delta 35.00 C exceeds limit 22.00 C (previous 25.00)"`, `range_text = "max change 22.00 C"` |
| always update | `previous_value = 60.00` |
| counters | `drift_checks++`, `drift_alerts++`, `alerts_by_type[TEMPERATURE]++`, reading counted **ABNORMAL** |
| result | `[process] ID=2 ... 60.00 C -> ALERT (sensor_drift_detector)` |

Alert list entry:

```
  #2  device 2 'temp-south' (Temperature)
       value          : 60.00 C
       expected range : max change 22.00 C
       reason         : drift delta 35.00 C exceeds limit 22.00 C (previous 25.00)
       callback       : sensor_drift_detector
```

**Counter-example (no alert):** `25.00 C -> 40.00 C` gives `delta = 15.00 <= 22.00`
→ nothing is raised, `previous_value` becomes `40.00`, and only `drift_checks` grows.

**Second example, battery:** `4.10 V -> 3.50 V`, span `4.5 - 2.5 = 2.0 V`,
`limit = 0.20 x 2.0 = 0.40 V`, `delta = 0.60 V > 0.40 V` → drift alert, while
`3.50 V` is still inside the normal `3.3 .. 4.2 V` window, so `battery_monitor`
stays silent and the reading is a battery **drift-only** alert.

**Both at once:** `4.10 V -> 3.10 V` gives `delta = 1.00 V > 0.40 V` *and* `3.10 V`
is below `3.30 V`, so `battery_monitor` and `sensor_drift_detector` both fire on the
same reading and two alert records are stored for one reading (allowed; the reading is
counted ABNORMAL exactly once).

---

## 5. Thresholds

`DRIFT_LIMIT_PCT = 0.20` (a `#define`, printed in the summary) of the **simulated span**
of the device type — *not* of the normal range:

| Type | Simulated range | Span | Drift limit (20 % of span) | Normal range |
|------|-----------------|------|-----------------------------|--------------|
| Temperature | −10 … 100 °C | 110 °C | **22.00 °C** | 20 … 70 °C |
| Pressure | 60 … 140 kPa | 80 kPa | **16.00 kPa** | 80 … 120 kPa |
| Battery | 2.5 … 4.5 V | 2.0 V | **0.40 V** | 3.3 … 4.2 V |

Rationale: 20 % of the full measurement window is large enough that ordinary sensor
noise never triggers it (a noisy ±2 °C thermometer never comes close), but small enough
to catch a genuine discontinuity — e.g. a 35 °C jump in one sample for temperature, or a
5 kPa step for pressure is *not* enough, while a 20 kPa step is.

Everything above is a compile-time `#define`; changing `DRIFT_LIMIT_PCT` (or any range)
in `main.c` changes the behaviour everywhere, and the summary (option 7) re-prints the
resulting limits so the configured thresholds are always visible in the output.
