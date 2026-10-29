/*
 * =====================================================================
 * main.c - Dynamic IoT Event Monitoring and Callback Engine
 *
 * Build : gcc -Wall -Wextra -std=c11 main.c -o main
 *         (add -lm if the platform keeps fabs() in libm)
 *         gcc -Wall -Wextra -std=c11 -fsanitize=address,undefined \
 *             main.c -o main_asan
 *
 * Single file, no global mutable state: every piece of state lives in
 * one struct Engine that is passed around by pointer.
 * =====================================================================
 */

#define _POSIX_C_SOURCE 200809L   /* isatty(), for transcript-friendly input echo */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
 * HEADER (printed at start and again inside the summary)
 * ------------------------------------------------------------------- */
#define STUDENT_ID      "12345"
#define PROJECT_NAME    "Dynamic IoT Event Monitoring and Callback Engine"
#define CUSTOM_CALLBACK "Sensor Drift Detector"

/* ---------------------------------------------------------------------
 * RANGES (all configurable here, printed again in the summary)
 * ------------------------------------------------------------------- */
#define TEMP_SIM_MIN    (-10.0)   /* simulated temperature range  -10 .. 100 C  */
#define TEMP_SIM_MAX    100.0
#define TEMP_NORM_MIN   20.0      /* normal temperature range      20 ..  70 C  */
#define TEMP_NORM_MAX   70.0

#define PRESS_SIM_MIN   60.0      /* simulated pressure range      60 .. 140 kPa */
#define PRESS_SIM_MAX   140.0
#define PRESS_NORM_MIN  80.0      /* normal pressure range         80 .. 120 kPa */
#define PRESS_NORM_MAX  120.0

#define BATT_SIM_MIN    2.5       /* simulated battery range      2.5 .. 4.5 V   */
#define BATT_SIM_MAX    4.5
#define BATT_NORM_MIN   3.3       /* normal battery range         3.3 .. 4.2 V   */
#define BATT_NORM_MAX   4.2

#define DRIFT_LIMIT_PCT 0.20      /* drift limit = 20% of the simulated span     */

/* ---------------------------------------------------------------------
 * MEMORY / BUFFER SIZES
 * ------------------------------------------------------------------- */
#define INITIAL_CAPACITY 4        /* device array starts here, doubles on growth */
#define LOG_INITIAL_CAPACITY 4    /* alert log also grows by doubling            */
#define NAME_LEN    40
#define REASON_LEN  192
#define RANGE_LEN   128
#define CALLBACK_LEN 40
#define UNIT_LEN     8
#define INPUT_LEN   128

/* ---------------------------------------------------------------------
 * DATA MODEL
 * ------------------------------------------------------------------- */
enum DeviceType {
    TYPE_TEMPERATURE,
    TYPE_PRESSURE,
    TYPE_BATTERY,
    NUM_TYPES
};

enum DeviceStatus {
    STATUS_NORMAL,
    STATUS_ALERT
};

struct AlertInfo {
    int triggered;                       /* set by a callback, cleared by engine */
    char reason[REASON_LEN];             /* why the alert fired                  */
    char range_text[RANGE_LEN];          /* expected range / allowed change      */
    char callback_name[CALLBACK_LEN];    /* which callback raised it             */
    double value;                        /* offending value                      */
    char unit[UNIT_LEN];                 /* unit of that value                   */
};

struct Device {
    int id;
    char name[NAME_LEN];
    int type;                            /* enum DeviceType                      */
    union {
        float temperature;
        int pressure;
        float voltage;
    } readings;
    int status;                          /* enum DeviceStatus                    */
    int has_reading;                     /* does readings hold a valid sample?   */
    double previous_value;               /* drift detector memory                */
    int has_previous;
    struct AlertInfo alert;
};

/* The alert log stores COPIES of the device data + alert so it remains
 * valid after the original device has been removed and freed. */
struct AlertRecord {
    int number;
    struct Device device;                /* copy of the device at alert time     */
    struct AlertInfo alert;              /* copy of the alert info               */
};

/* All program state in one struct, always passed by pointer. */
struct Engine {
    struct Device *devices;
    int count;
    int capacity;
    struct AlertRecord *alerts;
    int alert_count;
    int alert_capacity;
    long processed;                      /* readings processed                   */
    long normal;                         /* readings that stayed NORMAL          */
    long abnormal;                       /* readings that alerted at least once  */
    long alerts_by_type[NUM_TYPES];      /* alerts per device type               */
    long drift_checks;                   /* drift detector invocations           */
    long drift_alerts;                   /* alerts raised by the drift detector  */
    unsigned int seed;
    int running;
};

/* =====================================================================
 * SMALL HELPERS: type metadata (no switch/if-else around monitoring)
 * ==================================================================== */
static const char *type_name(int type)
{
    if (type == TYPE_TEMPERATURE) return "Temperature";
    if (type == TYPE_PRESSURE)    return "Pressure";
    if (type == TYPE_BATTERY)     return "Battery";
    return "Unknown";
}

static const char *type_unit(int type)
{
    if (type == TYPE_TEMPERATURE) return "C";
    if (type == TYPE_PRESSURE)    return "kPa";
    if (type == TYPE_BATTERY)     return "V";
    return "?";
}

static double type_sim_min(int type)
{
    if (type == TYPE_TEMPERATURE) return TEMP_SIM_MIN;
    if (type == TYPE_PRESSURE)    return PRESS_SIM_MIN;
    if (type == TYPE_BATTERY)     return BATT_SIM_MIN;
    return 0.0;
}

static double type_sim_max(int type)
{
    if (type == TYPE_TEMPERATURE) return TEMP_SIM_MAX;
    if (type == TYPE_PRESSURE)    return PRESS_SIM_MAX;
    if (type == TYPE_BATTERY)     return BATT_SIM_MAX;
    return 0.0;
}

static double type_norm_min(int type)
{
    if (type == TYPE_TEMPERATURE) return TEMP_NORM_MIN;
    if (type == TYPE_PRESSURE)    return PRESS_NORM_MIN;
    if (type == TYPE_BATTERY)     return BATT_NORM_MIN;
    return 0.0;
}

static double type_norm_max(int type)
{
    if (type == TYPE_TEMPERATURE) return TEMP_NORM_MAX;
    if (type == TYPE_PRESSURE)    return PRESS_NORM_MAX;
    if (type == TYPE_BATTERY)     return BATT_NORM_MAX;
    return 0.0;
}

/* Simulated span of a type; the drift limit is DRIFT_LIMIT_PCT of this. */
static double type_sim_span(int type)
{
    return type_sim_max(type) - type_sim_min(type);
}

static int type_is_valid(int type)
{
    return type >= 0 && type < NUM_TYPES;
}

/* The ONE accessor that checks the type before touching the union.
 * Used by the drift detector (and for display). */
static double device_value(const struct Device *d)
{
    if (d->type == TYPE_TEMPERATURE) return (double)d->readings.temperature; /* union access: temperature */
    if (d->type == TYPE_PRESSURE)    return (double)d->readings.pressure;    /* union access: pressure    */
    if (d->type == TYPE_BATTERY)     return (double)d->readings.voltage;     /* union access: voltage     */
    return 0.0;
}

/* =====================================================================
 * INPUT: fgets + strtol/strtod with checks (never gets / bare scanf)
 * ==================================================================== */

/* Reads one line. Returns 0 on end-of-file. When stdin is not a
 * terminal (piped test scripts) the entered line is echoed so the
 * transcript reads like an interactive session. */
static int read_line(const char *prompt, char *buf, size_t size)
{
    int ch;

    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, (int)size, stdin) == NULL) {
        printf("\n[end of input]\n");
        return 0;
    }
    if (strchr(buf, '\n') == NULL) {
        /* Line did not fit: discard the rest of it. */
        while ((ch = getchar()) != '\n' && ch != EOF) {
            /* discard overflow */
        }
    }
    buf[strcspn(buf, "\n")] = '\0';
    if (!isatty(fileno(stdin))) {
        printf("%s\n", buf);
        fflush(stdout);
    }
    return 1;
}

static int parse_long_strict(const char *s, long *out)
{
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (end == s || errno == ERANGE) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return 0;
    *out = value;
    return 1;
}

static int parse_double_strict(const char *s, double *out)
{
    char *end = NULL;
    double value;

    errno = 0;
    value = strtod(s, &end);
    if (end == s || errno == ERANGE) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != '\0') return 0;
    *out = value;
    return 1;
}

static int string_has_content(const char *s)
{
    while (*s != '\0') {
        if (*s != ' ' && *s != '\t' && *s != '\r') return 1;
        s++;
    }
    return 0;
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* =====================================================================
 * CALLBACKS
 * Each callback reads ONLY its own union member (after checking the
 * type) and fills device->alert on violation.
 * ==================================================================== */

/* Sets the alert fields shared by every callback. */
static void raise_alert(struct Device *device, const char *callback_name,
                        double value, const char *unit,
                        const char *reason, const char *range_text)
{
    device->status = STATUS_ALERT;
    device->alert.triggered = 1;
    device->alert.value = value;
    copy_string(device->alert.unit, sizeof device->alert.unit, unit);
    copy_string(device->alert.callback_name, sizeof device->alert.callback_name, callback_name);
    copy_string(device->alert.reason, sizeof device->alert.reason, reason);
    copy_string(device->alert.range_text, sizeof device->alert.range_text, range_text);
}

static void temperature_monitor(struct Device *device)
{
    float raw;
    double value;
    char reason[REASON_LEN];
    char range[RANGE_LEN];

    if (device == NULL || device->type != TYPE_TEMPERATURE) return;

    raw = device->readings.temperature;          /* union access: temperature (type checked) */
    value = (double)raw;

    if (value < TEMP_NORM_MIN || value > TEMP_NORM_MAX) {
        snprintf(reason, sizeof reason, "temperature %.2f C is %s normal %.2f C",
                 value, (value < TEMP_NORM_MIN) ? "below" : "above",
                 (value < TEMP_NORM_MIN) ? TEMP_NORM_MIN : TEMP_NORM_MAX);
        snprintf(range, sizeof range, "normal range %.2f to %.2f C",
                 TEMP_NORM_MIN, TEMP_NORM_MAX);
        raise_alert(device, "temperature_monitor", value, "C", reason, range);
    }
}

static void pressure_monitor(struct Device *device)
{
    int raw;
    double value;
    char reason[REASON_LEN];
    char range[RANGE_LEN];

    if (device == NULL || device->type != TYPE_PRESSURE) return;

    raw = device->readings.pressure;             /* union access: pressure (type checked) */
    value = (double)raw;

    if (value < PRESS_NORM_MIN || value > PRESS_NORM_MAX) {
        snprintf(reason, sizeof reason, "pressure %.2f kPa is %s normal %.2f kPa",
                 value, (value < PRESS_NORM_MIN) ? "below" : "above",
                 (value < PRESS_NORM_MIN) ? PRESS_NORM_MIN : PRESS_NORM_MAX);
        snprintf(range, sizeof range, "normal range %.2f to %.2f kPa",
                 PRESS_NORM_MIN, PRESS_NORM_MAX);
        raise_alert(device, "pressure_monitor", value, "kPa", reason, range);
    }
}

static void battery_monitor(struct Device *device)
{
    float raw;
    double value;
    char reason[REASON_LEN];
    char range[RANGE_LEN];

    if (device == NULL || device->type != TYPE_BATTERY) return;

    raw = device->readings.voltage;              /* union access: voltage (type checked) */
    value = (double)raw;

    if (value < BATT_NORM_MIN || value > BATT_NORM_MAX) {
        snprintf(reason, sizeof reason, "voltage %.2f V is %s normal %.2f V",
                 value, (value < BATT_NORM_MIN) ? "below" : "above",
                 (value < BATT_NORM_MIN) ? BATT_NORM_MIN : BATT_NORM_MAX);
        snprintf(range, sizeof range, "normal range %.2f to %.2f V",
                 BATT_NORM_MIN, BATT_NORM_MAX);
        raise_alert(device, "battery_monitor", value, "V", reason, range);
    }
}

/* Dispatch table indexed by device->type: no if/else or switch holds
 * the monitoring logic, only this table lookup. */
static void (*const TYPE_CALLBACKS[NUM_TYPES])(struct Device *) = {
    temperature_monitor,
    pressure_monitor,
    battery_monitor
};

static const char *const CALLBACK_NAMES[NUM_TYPES] = {
    "temperature_monitor",
    "pressure_monitor",
    "battery_monitor"
};

static const char DRIFT_CALLBACK_NAME[] = "sensor_drift_detector";

/* ---------------------------------------------------------------------
 * CUSTOM CALLBACK: Sensor Drift Detector
 * Same signature as the type monitors, invoked through process_device()
 * with a function pointer, after the type callback has run.
 *
 * 1. current = accessor(device)          (checks the type, one place)
 * 2. no previous  -> store current, return
 * 3. delta = fabs(current - previous)
 *    limit = DRIFT_LIMIT_PCT * simulated span of the type
 * 4. delta > limit -> raise an alert
 * 5. always update previous_value at the end
 * ------------------------------------------------------------------- */
static void sensor_drift_detector(struct Device *device)
{
    double current;
    double previous;
    double delta;
    double limit;
    const char *unit;
    char reason[REASON_LEN];
    char range[RANGE_LEN];

    if (device == NULL || !type_is_valid(device->type)) return;

    current = device_value(device);
    unit = type_unit(device->type);

    if (!device->has_previous) {
        device->previous_value = current;
        device->has_previous = 1;
        return;
    }

    previous = device->previous_value;
    delta = fabs(current - previous);
    limit = DRIFT_LIMIT_PCT * type_sim_span(device->type);

    if (delta > limit) {
        snprintf(reason, sizeof reason,
                 "drift delta %.2f %s exceeds limit %.2f %s (previous %.2f)",
                 delta, unit, limit, unit, previous);
        snprintf(range, sizeof range, "max change %.2f %s", limit, unit);
        raise_alert(device, DRIFT_CALLBACK_NAME, current, unit, reason, range);
    }

    device->previous_value = current;   /* always update at the end */
}

/* The engine's indirection point: everything runs through a pointer. */
static void process_device(struct Device *device, void (*callback)(struct Device *))
{
    if (device == NULL || callback == NULL) return;
    callback(device);
}

/* =====================================================================
 * ENGINE: memory management
 * ==================================================================== */
static void print_header(void)
{
    printf("+----------------------------------------------------------------+\n");
    printf("Student ID: %s\n", STUDENT_ID);
    printf("Project: %s\n", PROJECT_NAME);
    printf("Custom Callback: %s\n", CUSTOM_CALLBACK);
    printf("+----------------------------------------------------------------+\n");
}

static int engine_reserve(struct Engine *e, int wanted)
{
    int new_capacity;
    struct Device *tmp;

    if (wanted <= e->capacity) return 1;

    new_capacity = (e->capacity > 0) ? e->capacity : INITIAL_CAPACITY;
    while (new_capacity < wanted) new_capacity *= 2;

    tmp = realloc(e->devices, (size_t)new_capacity * sizeof *e->devices);
    if (tmp == NULL) return 0;              /* old pointer stays valid */

    printf("  [memory] device array grew: capacity %d -> %d\n",
           e->capacity, new_capacity);
    e->devices = tmp;
    e->capacity = new_capacity;
    return 1;
}

static void engine_shrink_if_sparse(struct Engine *e)
{
    int new_capacity;
    struct Device *tmp;

    if (e->capacity <= INITIAL_CAPACITY) return;
    if (e->count >= e->capacity / 4) return;

    new_capacity = e->capacity / 2;
    if (new_capacity < INITIAL_CAPACITY) new_capacity = INITIAL_CAPACITY;

    tmp = realloc(e->devices, (size_t)new_capacity * sizeof *e->devices);
    if (tmp == NULL) {
        printf("  [memory] shrink skipped (allocation failed); capacity stays %d\n",
               e->capacity);
        return;
    }
    printf("  [memory] device array shrank: capacity %d -> %d\n",
           e->capacity, new_capacity);
    e->devices = tmp;
    e->capacity = new_capacity;
}

static struct Device *engine_find(struct Engine *e, int id)
{
    struct Device *d;

    if (e->count == 0) return NULL;
    for (d = e->devices; d < e->devices + e->count; ++d) {
        if (d->id == id) return d;
    }
    return NULL;
}

/* Returns a pointer to the new (zeroed) slot, or NULL on failure. */
static struct Device *engine_add(struct Engine *e)
{
    struct Device *d;

    if (!engine_reserve(e, e->count + 1)) return NULL;

    d = e->devices + e->count;
    memset(d, 0, sizeof *d);
    d->status = STATUS_NORMAL;
    d->type = TYPE_TEMPERATURE;
    e->count++;
    return d;
}

static int engine_log_alert(struct Engine *e, const struct Device *device)
{
    struct AlertRecord *tmp;
    int new_capacity;

    if (e->alert_count == e->alert_capacity) {
        new_capacity = (e->alert_capacity > 0) ? e->alert_capacity : LOG_INITIAL_CAPACITY;
        new_capacity *= 2;
        tmp = realloc(e->alerts, (size_t)new_capacity * sizeof *e->alerts);
        if (tmp == NULL) return 0;
        e->alerts = tmp;
        e->alert_capacity = new_capacity;
    }

    e->alerts[e->alert_count].number = e->alert_count + 1;
    e->alerts[e->alert_count].device = *device;   /* copy of the device data  */
    e->alerts[e->alert_count].alert = device->alert; /* copy of the alert info */
    e->alert_count++;
    return 1;
}

static int engine_init(struct Engine *e, unsigned int seed)
{
    static const char *names[6] = {
        "temp-north", "temp-south", "press-line1", "press-line2", "batt-pack-a", "batt-pack-b"
    };
    static const int types[6] = {
        TYPE_TEMPERATURE, TYPE_TEMPERATURE, TYPE_PRESSURE,
        TYPE_PRESSURE, TYPE_BATTERY, TYPE_BATTERY
    };
    int i;

    memset(e, 0, sizeof *e);
    e->seed = seed;
    e->running = 1;

    e->devices = malloc(INITIAL_CAPACITY * sizeof *e->devices);
    if (e->devices == NULL) return 0;
    e->capacity = INITIAL_CAPACITY;
    e->count = 0;

    e->alerts = malloc(LOG_INITIAL_CAPACITY * sizeof *e->alerts);
    if (e->alerts == NULL) {
        free(e->devices);
        e->devices = NULL;
        return 0;
    }
    e->alert_capacity = LOG_INITIAL_CAPACITY;
    e->alert_count = 0;

    /* Start with 6 devices: 2 per type. */
    for (i = 0; i < 6; ++i) {
        struct Device *d = engine_add(e);
        if (d == NULL) {
            free(e->devices);
            free(e->alerts);
            e->devices = NULL;
            e->alerts = NULL;
            return 0;
        }
        d->id = i + 1;
        copy_string(d->name, sizeof d->name, names[i]);
        d->type = types[i];
    }
    return 1;
}

static void engine_destroy(struct Engine *e)
{
    free(e->devices);
    e->devices = NULL;
    free(e->alerts);
    e->alerts = NULL;
    e->count = 0;
    e->capacity = 0;
    e->alert_count = 0;
    e->alert_capacity = 0;
}

/* =====================================================================
 * READINGS
 * ==================================================================== */
static double random_between(double lo, double hi)
{
    return lo + (hi - lo) * ((double)rand() / ((double)RAND_MAX + 1.0));
}

/* Option 4: one reading per device, writing ONLY the matching member. */
static void generate_reading(struct Device *d)
{
    if (d->type == TYPE_TEMPERATURE) {
        double v = random_between(TEMP_SIM_MIN, TEMP_SIM_MAX);
        d->readings.temperature = (float)v;      /* union access: temperature (matches type) */
    } else if (d->type == TYPE_PRESSURE) {
        int lo = (int)PRESS_SIM_MIN;
        int hi = (int)PRESS_SIM_MAX;
        int v = lo + rand() % (hi - lo + 1);
        d->readings.pressure = v;                /* union access: pressure (matches type)    */
    } else if (d->type == TYPE_BATTERY) {
        double v = random_between(BATT_SIM_MIN, BATT_SIM_MAX);
        d->readings.voltage = (float)v;          /* union access: voltage (matches type)     */
    } else {
        return;
    }
    d->has_reading = 1;
}

/* =====================================================================
 * MENU HANDLERS
 * ==================================================================== */
static void print_menu(void)
{
    printf("\n--- MENU ---------------------------------------------------------\n");
    printf(" 1) Register device\n");
    printf(" 2) Display devices\n");
    printf(" 3) Remove device by ID\n");
    printf(" 4) Generate readings\n");
    printf(" 5) Process readings\n");
    printf(" 6) Display alerts\n");
    printf(" 7) Summary\n");
    printf(" 8) Auto-simulate N rounds\n");
    printf(" 9) Set a reading manually (testing aid)\n");
    printf(" 0) Exit\n");
    printf("------------------------------------------------------------------\n");
}

static void handle_exit(struct Engine *e)
{
    e->running = 0;
    printf("Exiting the engine.\n");
}

/* --- 1) Register ---------------------------------------------------- */
static void handle_register(struct Engine *e)
{
    char id_buf[INPUT_LEN];
    char name_buf[INPUT_LEN];
    char type_buf[INPUT_LEN];
    long id = 0;
    long type = 0;
    struct Device *d;

    /* Always consume exactly three lines so input stays predictable. */
    if (!read_line("  ID (positive integer): ", id_buf, sizeof id_buf)) return;
    if (!read_line("  Name (non-empty): ", name_buf, sizeof name_buf)) return;
    if (!read_line("  Type (1=Temperature, 2=Pressure, 3=Battery): ", type_buf, sizeof type_buf)) return;

    if (!parse_long_strict(id_buf, &id) || id <= 0) {
        printf("  ERROR: ID must be a positive integer (got '%s').\n", id_buf);
        return;
    }
    if (!string_has_content(name_buf)) {
        printf("  ERROR: name must not be empty.\n");
        return;
    }
    if (!parse_long_strict(type_buf, &type) || type < 1 || type > NUM_TYPES) {
        printf("  ERROR: type must be 1, 2 or 3 (got '%s').\n", type_buf);
        return;
    }
    if (engine_find(e, (int)id) != NULL) {
        printf("  ERROR: duplicate ID %ld, device already registered.\n", id);
        return;
    }

    d = engine_add(e);
    if (d == NULL) {
        printf("  ERROR: out of memory, device not registered.\n");
        return;
    }
    d->id = (int)id;
    copy_string(d->name, sizeof d->name, name_buf);
    d->type = (int)type - 1;                  /* menu is 1..3, enum is 0..2 */

    printf("  Registered device %d '%s' (%s).\n", d->id, d->name, type_name(d->type));
    printf("  count=%d capacity=%d\n", e->count, e->capacity);
}

/* --- 2) Display ----------------------------------------------------- */
static void handle_display(struct Engine *e)
{
    struct Device *d;

    printf("Devices: count=%d capacity=%d\n", e->count, e->capacity);
    if (e->count == 0) {
        printf("  (no devices registered)\n");
        return;
    }

    printf("  %4s  %-16s %-12s %-8s  %s\n", "ID", "Name", "Type", "Status", "Reading");
    for (d = e->devices; d < e->devices + e->count; ++d) {
        const char *status = (d->status == STATUS_ALERT) ? "ALERT" : "NORMAL";
        if (d->has_reading) {
            printf("  %4d  %-16s %-12s %-8s  %.2f %-4s  (normal %.2f to %.2f %s)\n",
                   d->id, d->name, type_name(d->type), status,
                   device_value(d), type_unit(d->type),
                   type_norm_min(d->type), type_norm_max(d->type), type_unit(d->type));
        } else {
            printf("  %4d  %-16s %-12s %-8s  (no reading yet)\n",
                   d->id, d->name, type_name(d->type), status);
        }
    }
}

/* --- 3) Remove ------------------------------------------------------ */
static void handle_remove(struct Engine *e)
{
    char id_buf[INPUT_LEN];
    long id = 0;
    struct Device *d;
    struct Device *p;

    if (!read_line("  ID to remove: ", id_buf, sizeof id_buf)) return;

    if (!parse_long_strict(id_buf, &id) || id <= 0) {
        printf("  ERROR: ID must be a positive integer (got '%s').\n", id_buf);
        printf("  count=%d capacity=%d\n", e->count, e->capacity);
        return;
    }

    d = engine_find(e, (int)id);
    if (d == NULL) {
        printf("  ERROR: no device with ID %ld.\n", id);
        printf("  count=%d capacity=%d\n", e->count, e->capacity);
        return;
    }

    printf("  Removed device %d '%s' (%s).\n", d->id, d->name, type_name(d->type));

    /* Shift later elements down (pointer traversal). */
    for (p = d; p < e->devices + e->count - 1; ++p) {
        *p = *(p + 1);
    }
    e->count--;
    memset(e->devices + e->count, 0, sizeof *e->devices);
    engine_shrink_if_sparse(e);
    printf("  count=%d capacity=%d\n", e->count, e->capacity);
}

/* --- 4) Generate ---------------------------------------------------- */
static void handle_generate(struct Engine *e)
{
    struct Device *d;
    int generated = 0;

    if (e->count == 0) {
        printf("  No devices registered, nothing to generate.\n");
        return;
    }
    for (d = e->devices; d < e->devices + e->count; ++d) {
        generate_reading(d);
        generated++;
        printf("[generate] ID=%d name=%s type=%s -> %.2f %s\n",
               d->id, d->name, type_name(d->type),
               device_value(d), type_unit(d->type));
    }
    printf("  Generated %d reading(s).\n", generated);
}

/* =====================================================================
 * THE PROCESSING PIPELINE for one reading
 * (shared by option 5 and by auto-simulation)
 *   1. reset the status,
 *   2. run the type callback selected from the function-pointer table,
 *   3. log/ count/ clear any alert it raised,
 *   4. run the custom drift callback through the same indirection,
 *   5. log/ count/ clear any alert it raised,
 *   6. count the reading as NORMAL or ABNORMAL and print one line.
 * Returns 1 if the device had a reading (0 otherwise).
 * ==================================================================== */
static int process_one_reading(struct Engine *e, struct Device *d)
{
    const char *first_cb = "";
    const char *second_cb = "";
    int alerted = 0;
    double value;
    const char *unit;

    if (!d->has_reading) return 0;

    /* The engine resets the status before each reading. */
    d->status = STATUS_NORMAL;

    /* 1) type callback selected through the function-pointer table */
    process_device(d, TYPE_CALLBACKS[d->type]);
    if (d->alert.triggered) {
        first_cb = CALLBACK_NAMES[d->type];
        if (!engine_log_alert(e, d)) {
            printf("  WARNING: alert log out of memory, alert not stored.\n");
        }
        e->alerts_by_type[d->type]++;
        alerted = 1;
        d->alert.triggered = 0;               /* cleared after logging */
    }

    /* 2) custom callback, same indirection, runs after the type one */
    e->drift_checks++;
    process_device(d, sensor_drift_detector);
    if (d->alert.triggered) {
        second_cb = DRIFT_CALLBACK_NAME;
        if (!engine_log_alert(e, d)) {
            printf("  WARNING: alert log out of memory, alert not stored.\n");
        }
        e->alerts_by_type[d->type]++;
        e->drift_alerts++;
        alerted = 1;
        d->alert.triggered = 0;               /* cleared after logging */
    }

    e->processed++;
    if (alerted) e->abnormal++;
    else         e->normal++;

    /* exactly one line per reading */
    value = device_value(d);
    unit = type_unit(d->type);
    if (alerted && first_cb[0] != '\0' && second_cb[0] != '\0') {
        printf("[process] ID=%d name=%s type=%s value=%.2f %s -> ALERT (%s, %s)\n",
               d->id, d->name, type_name(d->type), value, unit, first_cb, second_cb);
    } else if (alerted) {
        printf("[process] ID=%d name=%s type=%s value=%.2f %s -> ALERT (%s)\n",
               d->id, d->name, type_name(d->type), value, unit,
               (first_cb[0] != '\0') ? first_cb : second_cb);
    } else {
        printf("[process] ID=%d name=%s type=%s value=%.2f %s -> NORMAL (%s)\n",
               d->id, d->name, type_name(d->type), value, unit,
               CALLBACK_NAMES[d->type]);
    }
    return 1;
}

/* --- 5) Process ----------------------------------------------------- */
static void handle_process(struct Engine *e)
{
    struct Device *d;
    int processed_now = 0;
    int skipped = 0;

    printf("[dispatch] table[0]=%s table[1]=%s table[2]=%s ; custom=%s (via process_device)\n",
           CALLBACK_NAMES[TYPE_TEMPERATURE], CALLBACK_NAMES[TYPE_PRESSURE],
           CALLBACK_NAMES[TYPE_BATTERY], DRIFT_CALLBACK_NAME);

    if (e->count == 0) {
        printf("  No devices registered, nothing to process.\n");
        return;
    }

    for (d = e->devices; d < e->devices + e->count; ++d) {
        if (process_one_reading(e, d)) processed_now++;
        else                           skipped++;
    }

    if (processed_now == 0 && skipped > 0) {
        printf("  No readings to process (%d device(s) without a reading).\n", skipped);
    } else if (skipped > 0) {
        printf("  Skipped %d device(s) without a reading.\n", skipped);
    }
    printf("  This pass: %d reading(s) processed. Totals: processed=%ld normal=%ld abnormal=%ld\n",
           processed_now, e->processed, e->normal, e->abnormal);
}

/* --- 6) Alerts ------------------------------------------------------ */
static void handle_alerts(struct Engine *e)
{
    int i;

    if (e->alert_count == 0) {
        printf("Alert log: empty (0 alert(s), capacity %d).\n", e->alert_capacity);
        return;
    }
    printf("Alert log: %d alert(s), capacity %d (copies survive device removal)\n",
           e->alert_count, e->alert_capacity);
    for (i = 0; i < e->alert_count; ++i) {
        const struct AlertRecord *r = &e->alerts[i];
        printf("  #%d  device %d '%s' (%s)\n",
               r->number, r->device.id, r->device.name, type_name(r->device.type));
        printf("       value          : %.2f %s\n", r->alert.value, r->alert.unit);
        printf("       expected range : %s\n", r->alert.range_text);
        printf("       reason         : %s\n", r->alert.reason);
        printf("       callback       : %s\n", r->alert.callback_name);
    }
}

/* --- 7) Summary ----------------------------------------------------- */
static void print_summary(struct Engine *e)
{
    print_header();
    printf("Ranges (configurable #defines):\n");
    printf("  Temperature: simulated %.0f to %.0f C, normal %.0f to %.0f C\n",
           TEMP_SIM_MIN, TEMP_SIM_MAX, TEMP_NORM_MIN, TEMP_NORM_MAX);
    printf("  Pressure   : simulated %.0f to %.0f kPa, normal %.0f to %.0f kPa\n",
           PRESS_SIM_MIN, PRESS_SIM_MAX, PRESS_NORM_MIN, PRESS_NORM_MAX);
    printf("  Battery    : simulated %.1f to %.1f V, normal %.1f to %.1f V\n",
           BATT_SIM_MIN, BATT_SIM_MAX, BATT_NORM_MIN, BATT_NORM_MAX);
    printf("  Drift limit: %.0f%% of the simulated span -> temperature %.2f C, pressure %.2f kPa, battery %.2f V\n",
           DRIFT_LIMIT_PCT * 100.0,
           DRIFT_LIMIT_PCT * type_sim_span(TYPE_TEMPERATURE),
           DRIFT_LIMIT_PCT * type_sim_span(TYPE_PRESSURE),
           DRIFT_LIMIT_PCT * type_sim_span(TYPE_BATTERY));
    printf("\n");
    printf("  Seed used                       : %u\n", e->seed);
    printf("  Total registered devices        : %d\n", e->count);
    printf("  Readings processed              : %ld\n", e->processed);
    printf("  Normal readings                 : %ld\n", e->normal);
    printf("  Abnormal readings               : %ld\n", e->abnormal);
    printf("  Alerts per device type          : Temperature %ld, Pressure %ld, Battery %ld\n",
           e->alerts_by_type[TYPE_TEMPERATURE],
           e->alerts_by_type[TYPE_PRESSURE],
           e->alerts_by_type[TYPE_BATTERY]);
    printf("  Alerts logged (total)           : %d\n", e->alert_count);
    printf("  Drift checks                    : %ld\n", e->drift_checks);
    printf("  Drift alerts                    : %ld\n", e->drift_alerts);
    printf("  Device array                    : count %d, capacity %d\n",
           e->count, e->capacity);
    printf("  Alert log array                 : count %d, capacity %d\n",
           e->alert_count, e->alert_capacity);
    printf("  Check: normal + abnormal        : %ld (processed %ld -> %s)\n",
           e->normal + e->abnormal, e->processed,
           (e->normal + e->abnormal == e->processed) ? "OK" : "MISMATCH");
}

static void handle_summary(struct Engine *e)
{
    print_summary(e);
}

/* --- 8) Auto-simulate ----------------------------------------------- */
static void handle_autosim(struct Engine *e)
{
    char buf[INPUT_LEN];
    long rounds = 0;
    long r;
    struct Device *d;
    int generated = 0;
    int processed_now = 0;
    int skipped = 0;
    long processed_before;
    long normal_before;
    long abnormal_before;

    if (!read_line("  Number of rounds (1-1000): ", buf, sizeof buf)) return;
    if (!parse_long_strict(buf, &rounds) || rounds < 1 || rounds > 1000) {
        printf("  ERROR: rounds must be an integer between 1 and 1000.\n");
        return;
    }
    if (e->count == 0) {
        printf("  No devices registered, nothing to simulate.\n");
        return;
    }

    processed_before = e->processed;
    normal_before = e->normal;
    abnormal_before = e->abnormal;

    for (r = 1; r <= rounds; ++r) {
        printf("\n--- Auto-simulation round %ld/%ld ---------------------------------\n",
               r, rounds);

        /* generate one reading per device */
        generated = 0;
        for (d = e->devices; d < e->devices + e->count; ++d) {
            generate_reading(d);
            generated++;
            printf("[generate] ID=%d name=%s type=%s -> %.2f %s\n",
                   d->id, d->name, type_name(d->type),
                   device_value(d), type_unit(d->type));
        }

        /* process them through the callback table + drift detector */
        processed_now = 0;
        skipped = 0;
        for (d = e->devices; d < e->devices + e->count; ++d) {
            if (process_one_reading(e, d)) processed_now++;
            else                           skipped++;
        }
        printf("[round %ld] generated=%d processed=%d skipped=%d | totals processed=%ld normal=%ld abnormal=%ld alerts=%d drift_checks=%ld drift_alerts=%ld\n",
               r, generated, processed_now, skipped,
               e->processed, e->normal, e->abnormal, e->alert_count,
               e->drift_checks, e->drift_alerts);
    }
    printf("\nAuto-simulation finished: %ld round(s), %ld reading(s) processed in this run "
           "(%ld normal, %ld abnormal).\n",
           rounds, e->processed - processed_before,
           e->normal - normal_before, e->abnormal - abnormal_before);
}

/* --- 9) Set a reading manually (testing aid) ------------------------ */
static void handle_set_reading(struct Engine *e)
{
    char id_buf[INPUT_LEN];
    char value_buf[INPUT_LEN];
    long id = 0;
    double value = 0.0;
    struct Device *d;

    if (!read_line("  Device ID: ", id_buf, sizeof id_buf)) return;
    if (!parse_long_strict(id_buf, &id) || id <= 0) {
        printf("  ERROR: ID must be a positive integer (got '%s').\n", id_buf);
        return;
    }
    d = engine_find(e, (int)id);
    if (d == NULL) {
        printf("  ERROR: no device with ID %ld.\n", id);
        return;
    }

    printf("  Value for %s '%s': ", type_name(d->type), d->name);
    if (!read_line("", value_buf, sizeof value_buf)) return;
    if (!parse_double_strict(value_buf, &value)) {
        printf("  ERROR: '%s' is not a number.\n", value_buf);
        return;
    }

    if (d->type == TYPE_TEMPERATURE) {
        d->readings.temperature = (float)value;  /* union access: temperature (matches type) */
    } else if (d->type == TYPE_PRESSURE) {
        d->readings.pressure = (int)value;       /* union access: pressure (matches type)    */
    } else if (d->type == TYPE_BATTERY) {
        d->readings.voltage = (float)value;      /* union access: voltage (matches type)     */
    } else {
        printf("  ERROR: device has an invalid type.\n");
        return;
    }
    d->has_reading = 1;

    printf("  Set ID=%d %s type=%s -> %.2f %s (previous_value %s)\n",
           d->id, d->name, type_name(d->type),
           device_value(d), type_unit(d->type),
           d->has_previous ? "kept" : "not set yet");
    if (device_value(d) < type_sim_min(d->type) || device_value(d) > type_sim_max(d->type)) {
        printf("  note: value is outside the simulated range %.2f to %.2f %s\n",
               type_sim_min(d->type), type_sim_max(d->type), type_unit(d->type));
    }
    printf("  count=%d capacity=%d\n", e->count, e->capacity);
}

/* =====================================================================
 * MENU DISPATCH: array of function pointers (no big switch)
 * ==================================================================== */
typedef void (*MenuHandler)(struct Engine *);

static const MenuHandler MENU_HANDLERS[10] = {
    handle_exit,        /* 0 */
    handle_register,    /* 1 */
    handle_display,     /* 2 */
    handle_remove,      /* 3 */
    handle_generate,    /* 4 */
    handle_process,     /* 5 */
    handle_alerts,      /* 6 */
    handle_summary,     /* 7 */
    handle_autosim,     /* 8 */
    handle_set_reading  /* 9 */
};

static const char *const MENU_LABELS[10] = {
    "Exit",
    "Register device",
    "Display devices",
    "Remove device by ID",
    "Generate readings",
    "Process readings",
    "Display alerts",
    "Summary",
    "Auto-simulate N rounds",
    "Set a reading manually (testing aid)"
};

/* =====================================================================
 * MAIN
 * ==================================================================== */
int main(int argc, char *argv[])
{
    struct Engine engine;
    unsigned int seed;
    char buf[INPUT_LEN];
    long choice = -1;

    /* Seed: numeric command-line argument, otherwise time(NULL). */
    if (argc >= 2) {
        long parsed = 0;
        if (parse_long_strict(argv[1], &parsed)) {
            seed = (unsigned int)parsed;
        } else {
            seed = (unsigned int)time(NULL);
            printf("Note: '%s' is not a numeric seed, using time(NULL) instead.\n", argv[1]);
        }
    } else {
        seed = (unsigned int)time(NULL);
    }

    print_header();
    printf("Seed: %u\n", seed);
    srand(seed);                              /* called exactly once */

    if (!engine_init(&engine, seed)) {
        printf("FATAL: out of memory during initialisation.\n");
        engine_destroy(&engine);
        return EXIT_FAILURE;
    }
    printf("Initialised with 6 devices (2 per type): count=%d capacity=%d\n",
           engine.count, engine.capacity);

    while (engine.running) {
        print_menu();
        if (!read_line("Choice: ", buf, sizeof buf)) break;   /* EOF exits cleanly */

        if (!parse_long_strict(buf, &choice) || choice < 0 || choice > 9) {
            printf("Invalid choice '%s': enter a number from 0 to 9.\n", buf);
            continue;
        }
        printf("> %ld) %s\n", choice, MENU_LABELS[choice]);
        MENU_HANDLERS[choice](&engine);
    }

    printf("\nShutting down: freeing %d device(s) and %d alert record(s).\n",
           engine.count, engine.alert_count);
    engine_destroy(&engine);
    printf("Memory released. Goodbye.\n");
    return EXIT_SUCCESS;
}
