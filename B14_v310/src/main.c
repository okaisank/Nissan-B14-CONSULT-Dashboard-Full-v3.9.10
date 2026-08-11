#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <inttypes.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "ftdi_bridge.h"

#include "dashboard.h"
#include "rotary_encoder.h"

static const char *TAG = "B14_TRIP_FULL";

/* --------------------------------------------------------------------------
 * Safety and operating limits
 * -------------------------------------------------------------------------- */
#define CONSULT_FRAME_LENGTH          18U
#define RX_STREAM_BUFFER_SIZE       4096U
#define PRINT_INTERVAL_MS            1000U
#define STREAM_TIMEOUT_MS            2500U
#define GOOD_FRAMES_AFTER_RECONNECT     5U
#define ENGINE_START_CONFIRM_MS       1000U
#define ENGINE_STOP_CONFIRM_MS        8000U
#define PARKED_PREVIEW_DELAY_MS      15000U
#define SNAPSHOT_INTERVAL_MS         30000U
#define SNAPSHOT_DISTANCE_STEP_KM      0.5

/* Optional IGN/ACC sense input. Feed this GPIO only through an automotive-safe
 * divider/opto/input conditioner. NEVER connect vehicle 12 V directly.
 * If the pin is not wired, the firmware still works with RPM-based trip stop
 * and power-loss recovery; IGN safe-shutdown simply remains inactive. */
#define IGN_SENSE_GPIO               GPIO_NUM_17
#define IGN_ACTIVE_LEVEL                       1
#define IGN_DEBOUNCE_MS                      300U
#define TRIP_SAVED_SCREEN_MS                15000U

#define WARN_ECT_C                           105
#define WARN_LOW_BAT_V                       12.5f
#define WARN_HIGH_BAT_V                      15.0f
#define WARN_BAT_MIN_RPM                   1000.0f
#define INVALID_FRAME_EVENT_STREAK             5U

#define FIRMWARE_VERSION             "3.9.10-CONFIG-GUARD-FIX-FULL"

#define DEFAULT_FUEL_PRICE_BAHT_L      36.70f
#define DEFAULT_INJECTOR_FLOW_CC_MIN  180.00f
#define DEFAULT_FUEL_CALIBRATION        1.000f
#define NUMBER_OF_INJECTORS              4.0f

/* v3.9.10 engineering-unit model.
 * Nissan CONSULT supplies the MAF channel as sensor voltage on this ECU.
 * MAF_EST is therefore explicitly an estimate, not a direct ECU g/s value.
 * The raw voltage remains available for traceability and later calibration. */
#define ENGINE_DISPLACEMENT_L            1.60f
#define AIR_DENSITY_G_PER_L               1.18f
#define DEFAULT_TPS_CLOSED_V              0.56f
#define DEFAULT_TPS_WOT_V                 4.00f
#define MAF_RAW_MIN_VALID_V               0.20f
#define MAF_RAW_MAX_VALID_V               4.80f
#define O2_LEAN_THRESHOLD_V               0.35f
#define O2_RICH_THRESHOLD_V               0.55f

#define EVENT_INIT_ACK     BIT0
#define EVENT_VALID_FRAME  BIT1

#define STORE_MAGIC       0x42313446U  /* B14F */
#define STORE_VERSION             3U
#define TRIP_HISTORY_CAPACITY          100U
#define EVENT_HISTORY_CAPACITY         128U
#define HISTORY_PRINT_LINES             10U
#define HISTORY_STORE_MAGIC      0x42313948U  /* B19H */
#define EVENT_STORE_MAGIC        0x42313945U  /* B19E */
#define RING_STORE_VERSION                1U
#define NVS_PARTITION_LABEL  "tripnvs"
#define NVS_NAMESPACE        "b14store"
#define UI_STORE_MAGIC       0x42313455U  /* B14U */
#define UI_STORE_VERSION             1U
#define UI_VISIBLE_O2          (1U << 0)
#define UI_VISIBLE_AAC         (1U << 1)
#define UI_VISIBLE_AF          (1U << 2)
#define UI_VISIBLE_DEFAULT     (UI_VISIBLE_O2 | UI_VISIBLE_AAC | UI_VISIBLE_AF)
#define RAW_LOCK_FRAMES               3U
#define RAW_SIGNATURE_REPEAT         true
#define SESSION_RX_MIN_BYTES         72U
#define SESSION_RX_TIMEOUT_MS      3000U

#define EVENT_QUEUE_DEPTH 24

/* --------------------------------------------------------------------------
 * Persistent models
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;
    uint32_t version;
    float fuel_price_baht_l;
    float fuel_calibration;
    float injector_flow_cc_min;
    char current_date[11];          /* YYYY-MM-DD */
    uint32_t next_trip_id;
} settings_t;

typedef struct {
    uint32_t trip_id;
    char date[11];
    uint64_t start_uptime_ms;
    uint64_t end_uptime_ms;

    double distance_km;
    double fuel_l_est;
    double cost_baht_est;
    double engine_seconds;
    double moving_seconds;
    double idle_seconds;
    double ac_seconds;

    int32_t max_coolant_c;
    float min_battery_v;
    float fuel_price_baht_l;
    float fuel_calibration;

    /* v3.7 engineering statistics. Integrals are persisted so a recovered
     * power-loss trip still retains usable averages. */
    float max_rpm;
    int32_t max_speed_kmh;
    float max_fuel_lph;
    float max_tps_v;
    float max_aac_pct;
    double rpm_time_integral;
    double maf_time_integral;
    double af_alpha_time_integral;
    uint32_t o2_switch_count;
    uint32_t usb_disconnect_count;
    uint32_t overheat_event_count;
    uint32_t low_battery_event_count;
    uint32_t high_battery_event_count;
    uint32_t sensor_warning_count;
    float injector_flow_cc_min;
    uint8_t parser_mode_end;
    char firmware_version[20];

    uint32_t accepted_frames;
    uint32_t rejected_frames;
    uint8_t recovered_after_power_loss;
    uint8_t committed;
    char end_reason[16];
} trip_record_t;

/* Legacy v3.6.x record shape for one-time NVS migration. */
typedef struct {
    uint32_t trip_id;
    char date[11];
    uint64_t start_uptime_ms;
    uint64_t end_uptime_ms;
    double distance_km;
    double fuel_l_est;
    double cost_baht_est;
    double engine_seconds;
    double moving_seconds;
    double idle_seconds;
    double ac_seconds;
    int32_t max_coolant_c;
    float min_battery_v;
    float fuel_price_baht_l;
    float fuel_calibration;
    uint32_t accepted_frames;
    uint32_t rejected_frames;
    uint8_t recovered_after_power_loss;
    uint8_t committed;
    char end_reason[16];
} trip_record_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t active;
    trip_record_v2_t trip;
} active_snapshot_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t valid;
    trip_record_v2_t trip;
} last_trip_store_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    char daily_date[11];
    uint32_t daily_trip_count;
    double daily_distance_km;
    double daily_fuel_l;
    double daily_cost_baht;
    double daily_engine_seconds;
    double daily_moving_seconds;
    double daily_idle_seconds;
    double daily_ac_seconds;

    uint32_t lifetime_trip_count;
    double lifetime_distance_km;
    double lifetime_fuel_l;
    double lifetime_cost_baht;
    double lifetime_engine_seconds;
    double lifetime_moving_seconds;
    double lifetime_idle_seconds;
    double lifetime_ac_seconds;

    uint32_t last_committed_trip_id;
} totals_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t active;
    trip_record_t trip;
} active_snapshot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t valid;
    trip_record_t trip;
} last_trip_store_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t last_page;
    uint8_t reserved[3];
    uint32_t visible_mask;
} ui_settings_t;

typedef enum {
    PARSER_SEARCH = 0,
    PARSER_HEADER_FF12,
    PARSER_RAW18,
} parser_mode_t;

/* --------------------------------------------------------------------------
 * Runtime models
 * -------------------------------------------------------------------------- */
typedef struct {
    bool valid;
    uint32_t accepted_frame_count;
    uint32_t rejected_frame_count;

    float rpm;
    float injector_pulse_ms;
    float injector_duty_pct;
    float maf_voltage_v;
    int coolant_temp_c;
    float o2_voltage_v;
    int vehicle_speed_kmh;
    float battery_voltage_v;
    float throttle_voltage_v;
    float throttle_position_pct_est;
    float maf_gps_est;
    bool maf_gps_est_valid;
    bool o2_rich;
    bool o2_lean;
    float fuel_correction_delta_pct;
    int ignition_timing_btdc_deg;
    float iacv_aac_pct;
    uint8_t digital_13;
    uint8_t control_1e;
    uint8_t control_1f;
    int af_alpha_pct;

    bool closed_throttle;
    bool park_neutral;
    bool ac_on;

    float fuel_lph_est;
    float instant_km_per_l_est;
} live_sensor_t;

typedef enum {
    TRIP_WAIT_ENGINE = 0,
    TRIP_RUNNING,
    TRIP_STOP_CONFIRM,
    TRIP_FINALIZING,
} trip_state_t;

typedef struct {
    trip_state_t state;
    bool active;
    bool preview_emitted;
    bool preview_event_pending;
    int64_t parked_since_us;
    int64_t engine_off_since_us;
    int64_t previous_integration_frame_us;
    bool o2_state_known;
    bool last_o2_rich;
    bool warn_ect_active;
    bool warn_low_bat_active;
    bool warn_high_bat_active;
    trip_record_t record;
} trip_runtime_t;

typedef struct {
    live_sensor_t live;
    trip_runtime_t trip;

    bool saved_event_pending;
    trip_record_t saved_event;
    totals_t saved_totals_event;
} system_state_t;

typedef struct {
    uint32_t trip_id;
    uint64_t uptime_ms;
    char date[11];
    char code[24];
    float value;
    char unit[8];
} event_log_record_t;

/* Internal Flash/NVS ring metadata. Trip/event records are stored in individual
 * NVS keys so a new record does not rewrite the entire history blob. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint16_t head;      /* next slot to write */
    uint16_t count;     /* valid records, capped by capacity */
    uint32_t writes;
} ring_meta_t;

/* --------------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------------- */
static b14_ftdi_handle_t s_ftdi = NULL;
static volatile bool s_ftdi_connected = false;
static volatile bool s_ftdi_disconnected = false;
static volatile bool s_waiting_init_ack = false;
static volatile bool s_ecu_streaming = false;
static volatile bool s_parser_reset_requested = false;
static volatile bool s_snapshot_requested = false;
static volatile bool s_consult_session_active = false;
static volatile int64_t s_last_rx_us = 0;
static volatile parser_mode_t s_parser_mode = PARSER_SEARCH;

static StreamBufferHandle_t s_rx_stream = NULL;
static EventGroupHandle_t s_events = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static SemaphoreHandle_t s_storage_mutex = NULL;
static QueueHandle_t s_event_queue = NULL;

static uint64_t s_rx_total = 0;
static uint64_t s_rx_dropped = 0;
static int64_t s_last_valid_frame_us = 0;
static int64_t s_engine_start_candidate_us = 0;
static volatile uint32_t s_good_frame_streak = 0;

static system_state_t s_system = {0};
static settings_t s_settings = {0};
static totals_t s_totals = {0};
static last_trip_store_t s_last_trip = {0};
static active_snapshot_t s_active_snapshot = {0};
static ui_settings_t s_ui_settings = {0};
static SemaphoreHandle_t s_ui_mutex = NULL;
static volatile bool s_tft_ready = false;
static volatile int s_ui_page = DASH_PAGE_MAIN;
static volatile int s_settings_index = 0;
static volatile bool s_settings_editing = false;
static volatile float s_settings_edit_value = 0.0f;
static volatile bool s_ui_saved_flash = false;

static nvs_handle_t s_nvs_handle = 0;
static bool s_nvs_ready = false;
static ring_meta_t s_trip_history_meta = {0};
static ring_meta_t s_event_history_meta = {0};

static int64_t s_last_snapshot_us = 0;
static double s_last_snapshot_distance_km = 0.0;
static volatile bool s_ign_seen_on = false;
static volatile bool s_ign_stable_on = false;
static volatile bool s_ign_off_requested = false;
static volatile int64_t s_trip_saved_screen_until_us = 0;
static uint32_t s_invalid_frame_streak = 0;

/* --------------------------------------------------------------------------
 * Utility functions
 * -------------------------------------------------------------------------- */
static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void build_date_iso(char out[11])
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    char mon[4] = {0};
    int day = 1;
    int year = 2026;
    int month = 1;

    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3) {
        for (int i = 0; i < 12; ++i) {
            if (strcmp(mon, months[i]) == 0) {
                month = i + 1;
                break;
            }
        }
    }

    snprintf(out, 11, "%04d-%02d-%02d", year, month, day);
}

static bool valid_iso_date(const char *date)
{
    if (date == NULL || strlen(date) != 10) {
        return false;
    }
    if (date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (!isdigit((unsigned char)date[i])) {
            return false;
        }
    }

    int y = 0, m = 0, d = 0;
    if (sscanf(date, "%4d-%2d-%2d", &y, &m, &d) != 3) {
        return false;
    }
    if (y < 2020 || y > 2099 || m < 1 || m > 12 || d < 1) return false;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[m - 1];
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    if (m == 2 && leap) dim = 29;
    return d <= dim;
}

static const char *trip_state_name(trip_state_t state)
{
    switch (state) {
        case TRIP_WAIT_ENGINE: return "WAIT_ENGINE";
        case TRIP_RUNNING: return "RUNNING";
        case TRIP_STOP_CONFIRM: return "STOP_CONFIRM";
        case TRIP_FINALIZING: return "FINALIZING";
        default: return "UNKNOWN";
    }
}

static const char *parser_mode_name(parser_mode_t mode)
{
    switch (mode) {
        case PARSER_HEADER_FF12: return "FF12";
        case PARSER_RAW18: return "RAW18";
        case PARSER_SEARCH:
        default: return "SEARCH";
    }
}

static double safe_average_km_l(double distance_km, double fuel_l)
{
    return fuel_l > 0.0001 ? distance_km / fuel_l : 0.0;
}

static double safe_average_speed_kmh(const trip_record_t *trip)
{
    return (trip && trip->moving_seconds > 0.1) ?
        trip->distance_km * 3600.0 / trip->moving_seconds : 0.0;
}

static double safe_average_rpm(const trip_record_t *trip)
{
    return (trip && trip->engine_seconds > 0.1) ?
        trip->rpm_time_integral / trip->engine_seconds : 0.0;
}

static double safe_average_maf_v(const trip_record_t *trip)
{
    return (trip && trip->engine_seconds > 0.1) ?
        trip->maf_time_integral / trip->engine_seconds : 0.0;
}

static double safe_average_af_alpha(const trip_record_t *trip)
{
    return (trip && trip->engine_seconds > 0.1) ?
        trip->af_alpha_time_integral / trip->engine_seconds : 0.0;
}

static double safe_average_fuel_lph(const trip_record_t *trip)
{
    return (trip && trip->engine_seconds > 0.1) ?
        trip->fuel_l_est * 3600.0 / trip->engine_seconds : 0.0;
}

static bool is_leap_year(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static bool increment_iso_date(char date[11])
{
    if (!valid_iso_date(date)) return false;
    int y = 0, m = 0, d = 0;
    if (sscanf(date, "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[m - 1];
    if (m == 2 && is_leap_year(y)) dim = 29;
    if (d < 1 || d > dim) return false;
    d++;
    if (d > dim) {
        d = 1;
        m++;
        if (m > 12) {
            m = 1;
            y++;
            if (y > 2099) y = 2020;
        }
    }
    snprintf(date, 11, "%04d-%02d-%02d", y, m, d);
    return true;
}

static void migrate_trip_v2(const trip_record_v2_t *old, trip_record_t *out)
{
    if (!old || !out) return;
    memset(out, 0, sizeof(*out));
    out->trip_id = old->trip_id;
    copy_string(out->date, sizeof(out->date), old->date);
    out->start_uptime_ms = old->start_uptime_ms;
    out->end_uptime_ms = old->end_uptime_ms;
    out->distance_km = old->distance_km;
    out->fuel_l_est = old->fuel_l_est;
    out->cost_baht_est = old->cost_baht_est;
    out->engine_seconds = old->engine_seconds;
    out->moving_seconds = old->moving_seconds;
    out->idle_seconds = old->idle_seconds;
    out->ac_seconds = old->ac_seconds;
    out->max_coolant_c = old->max_coolant_c;
    out->min_battery_v = old->min_battery_v;
    out->fuel_price_baht_l = old->fuel_price_baht_l;
    out->fuel_calibration = old->fuel_calibration;
    out->injector_flow_cc_min = DEFAULT_INJECTOR_FLOW_CC_MIN;
    copy_string(out->firmware_version, sizeof(out->firmware_version), "v3.6.x-migrated");
    out->accepted_frames = old->accepted_frames;
    out->rejected_frames = old->rejected_frames;
    out->recovered_after_power_loss = old->recovered_after_power_loss;
    out->committed = old->committed;
    copy_string(out->end_reason, sizeof(out->end_reason), old->end_reason);
}

static void queue_trip_event_locked(const char *code, float value, const char *unit)
{
    if (!s_event_queue || !code || !s_system.trip.active) return;
    event_log_record_t ev = {0};
    ev.trip_id = s_system.trip.record.trip_id;
    ev.uptime_ms = uptime_ms();
    copy_string(ev.date, sizeof(ev.date), s_system.trip.record.date);
    copy_string(ev.code, sizeof(ev.code), code);
    ev.value = value;
    copy_string(ev.unit, sizeof(ev.unit), unit ? unit : "");
    (void)xQueueSend(s_event_queue, &ev, 0);
}

static void reset_daily_totals_locked(const char *date)
{
    copy_string(s_totals.daily_date, sizeof(s_totals.daily_date), date);
    s_totals.daily_trip_count = 0;
    s_totals.daily_distance_km = 0.0;
    s_totals.daily_fuel_l = 0.0;
    s_totals.daily_cost_baht = 0.0;
    s_totals.daily_engine_seconds = 0.0;
    s_totals.daily_moving_seconds = 0.0;
    s_totals.daily_idle_seconds = 0.0;
    s_totals.daily_ac_seconds = 0.0;
}

/* --------------------------------------------------------------------------
 * Internal Flash / NVS storage
 * -------------------------------------------------------------------------- */
static esp_err_t nvs_set_blob_checked(const char *key, const void *value, size_t size)
{
    if (!s_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_set_blob(s_nvs_handle, key, value, size);
}

static esp_err_t commit_base_store_locked(void)
{
    esp_err_t err;

    err = nvs_set_blob_checked("settings", &s_settings, sizeof(s_settings));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("totals", &s_totals, sizeof(s_totals));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("lasttrip", &s_last_trip, sizeof(s_last_trip));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("active", &s_active_snapshot, sizeof(s_active_snapshot));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("ui", &s_ui_settings, sizeof(s_ui_settings));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("histmeta", &s_trip_history_meta, sizeof(s_trip_history_meta));
    if (err != ESP_OK) return err;

    err = nvs_set_blob_checked("evtmeta", &s_event_history_meta, sizeof(s_event_history_meta));
    if (err != ESP_OK) return err;

    return nvs_commit(s_nvs_handle);
}

static bool load_blob_exact(const char *key, void *dst, size_t expected_size)
{
    size_t size = expected_size;
    esp_err_t err = nvs_get_blob(s_nvs_handle, key, dst, &size);
    return err == ESP_OK && size == expected_size;
}

static esp_err_t init_persistent_storage(void)
{
    esp_err_t err = nvs_flash_init_partition(NVS_PARTITION_LABEL);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing incompatible trip NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_PARTITION_LABEL));
        err = nvs_flash_init_partition(NVS_PARTITION_LABEL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trip NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_open_from_partition(
        NVS_PARTITION_LABEL,
        NVS_NAMESPACE,
        NVS_READWRITE,
        &s_nvs_handle
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trip NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    s_nvs_ready = true;

    bool settings_ok = load_blob_exact("settings", &s_settings, sizeof(s_settings));
    if (settings_ok && s_settings.magic == STORE_MAGIC && s_settings.version == 2U) {
        ESP_LOGI(TAG, "Migrating v3.6 settings -> v3.7 without resetting user values");
        s_settings.version = STORE_VERSION;
    } else if (!settings_ok || s_settings.magic != STORE_MAGIC || s_settings.version != STORE_VERSION) {
        memset(&s_settings, 0, sizeof(s_settings));
        s_settings.magic = STORE_MAGIC;
        s_settings.version = STORE_VERSION;
        s_settings.fuel_price_baht_l = DEFAULT_FUEL_PRICE_BAHT_L;
        s_settings.fuel_calibration = DEFAULT_FUEL_CALIBRATION;
        s_settings.injector_flow_cc_min = DEFAULT_INJECTOR_FLOW_CC_MIN;
        build_date_iso(s_settings.current_date);
        s_settings.next_trip_id = 1;
    }

    bool totals_ok = load_blob_exact("totals", &s_totals, sizeof(s_totals));
    if (totals_ok && s_totals.magic == STORE_MAGIC && s_totals.version == 2U) {
        ESP_LOGI(TAG, "Migrating v3.6 daily/lifetime totals -> v3.7");
        s_totals.version = STORE_VERSION;
    } else if (!totals_ok || s_totals.magic != STORE_MAGIC || s_totals.version != STORE_VERSION) {
        memset(&s_totals, 0, sizeof(s_totals));
        s_totals.magic = STORE_MAGIC;
        s_totals.version = STORE_VERSION;
        reset_daily_totals_locked(s_settings.current_date);
    }

    bool last_ok = load_blob_exact("lasttrip", &s_last_trip, sizeof(s_last_trip));
    if (!last_ok || s_last_trip.magic != STORE_MAGIC || s_last_trip.version != STORE_VERSION) {
        last_trip_store_v2_t legacy_last = {0};
        bool legacy_ok = load_blob_exact("lasttrip", &legacy_last, sizeof(legacy_last));
        memset(&s_last_trip, 0, sizeof(s_last_trip));
        s_last_trip.magic = STORE_MAGIC;
        s_last_trip.version = STORE_VERSION;
        if (legacy_ok && legacy_last.magic == STORE_MAGIC && legacy_last.version == 2U && legacy_last.valid) {
            s_last_trip.valid = 1;
            migrate_trip_v2(&legacy_last.trip, &s_last_trip.trip);
            ESP_LOGI(TAG, "Migrated last saved trip %lu from v3.6",
                     (unsigned long)s_last_trip.trip.trip_id);
        }
    }

    bool active_ok = load_blob_exact("active", &s_active_snapshot, sizeof(s_active_snapshot));
    if (!active_ok || s_active_snapshot.magic != STORE_MAGIC || s_active_snapshot.version != STORE_VERSION) {
        active_snapshot_v2_t legacy_active = {0};
        bool legacy_ok = load_blob_exact("active", &legacy_active, sizeof(legacy_active));
        memset(&s_active_snapshot, 0, sizeof(s_active_snapshot));
        s_active_snapshot.magic = STORE_MAGIC;
        s_active_snapshot.version = STORE_VERSION;
        if (legacy_ok && legacy_active.magic == STORE_MAGIC && legacy_active.version == 2U && legacy_active.active) {
            s_active_snapshot.active = 1;
            migrate_trip_v2(&legacy_active.trip, &s_active_snapshot.trip);
            ESP_LOGW(TAG, "Migrated unfinished v3.6 trip %lu for power-loss recovery",
                     (unsigned long)s_active_snapshot.trip.trip_id);
        }
    }

    bool ui_ok = load_blob_exact("ui", &s_ui_settings, sizeof(s_ui_settings));
    if (!ui_ok || s_ui_settings.magic != UI_STORE_MAGIC || s_ui_settings.version != UI_STORE_VERSION) {
        memset(&s_ui_settings, 0, sizeof(s_ui_settings));
        s_ui_settings.magic = UI_STORE_MAGIC;
        s_ui_settings.version = UI_STORE_VERSION;
        s_ui_settings.last_page = DASH_PAGE_MAIN;
        s_ui_settings.visible_mask = UI_VISIBLE_DEFAULT;
    }
    if (s_ui_settings.last_page >= DASH_PAGE_COUNT) s_ui_settings.last_page = DASH_PAGE_MAIN;
    if ((s_ui_settings.visible_mask & UI_VISIBLE_DEFAULT) == 0) s_ui_settings.visible_mask = UI_VISIBLE_DEFAULT;

    bool hist_ok = load_blob_exact("histmeta", &s_trip_history_meta, sizeof(s_trip_history_meta));
    if (!hist_ok || s_trip_history_meta.magic != HISTORY_STORE_MAGIC ||
        s_trip_history_meta.version != RING_STORE_VERSION ||
        s_trip_history_meta.head >= TRIP_HISTORY_CAPACITY ||
        s_trip_history_meta.count > TRIP_HISTORY_CAPACITY) {
        memset(&s_trip_history_meta, 0, sizeof(s_trip_history_meta));
        s_trip_history_meta.magic = HISTORY_STORE_MAGIC;
        s_trip_history_meta.version = RING_STORE_VERSION;
    }

    bool evt_ok = load_blob_exact("evtmeta", &s_event_history_meta, sizeof(s_event_history_meta));
    if (!evt_ok || s_event_history_meta.magic != EVENT_STORE_MAGIC ||
        s_event_history_meta.version != RING_STORE_VERSION ||
        s_event_history_meta.head >= EVENT_HISTORY_CAPACITY ||
        s_event_history_meta.count > EVENT_HISTORY_CAPACITY) {
        memset(&s_event_history_meta, 0, sizeof(s_event_history_meta));
        s_event_history_meta.magic = EVENT_STORE_MAGIC;
        s_event_history_meta.version = RING_STORE_VERSION;
    }

    if (strcmp(s_totals.daily_date, s_settings.current_date) != 0) {
        reset_daily_totals_locked(s_settings.current_date);
    }

    err = commit_base_store_locked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial NVS commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "NVS ready | date=%s | next trip=%lu | fuel=%.2f Baht/L | cal=%.3f",
             s_settings.current_date,
             (unsigned long)s_settings.next_trip_id,
             s_settings.fuel_price_baht_l,
             s_settings.fuel_calibration);
    return ESP_OK;
}

static void trip_history_key(uint16_t slot, char key[16])
{
    snprintf(key, 16, "h%03u", (unsigned)slot);
}

static void event_history_key(uint16_t slot, char key[16])
{
    snprintf(key, 16, "e%03u", (unsigned)slot);
}

static esp_err_t stage_trip_history_locked(const trip_record_t *trip)
{
    if (!s_nvs_ready || trip == NULL) return ESP_ERR_INVALID_STATE;

    char key[16];
    const uint16_t slot = s_trip_history_meta.head;
    trip_history_key(slot, key);
    esp_err_t err = nvs_set_blob_checked(key, trip, sizeof(*trip));
    if (err != ESP_OK) return err;

    s_trip_history_meta.head = (uint16_t)((slot + 1U) % TRIP_HISTORY_CAPACITY);
    if (s_trip_history_meta.count < TRIP_HISTORY_CAPACITY) s_trip_history_meta.count++;
    s_trip_history_meta.writes++;
    return nvs_set_blob_checked("histmeta", &s_trip_history_meta, sizeof(s_trip_history_meta));
}

static esp_err_t stage_event_history_locked(const event_log_record_t *ev)
{
    if (!s_nvs_ready || ev == NULL) return ESP_ERR_INVALID_STATE;

    char key[16];
    const uint16_t slot = s_event_history_meta.head;
    event_history_key(slot, key);
    esp_err_t err = nvs_set_blob_checked(key, ev, sizeof(*ev));
    if (err != ESP_OK) return err;

    s_event_history_meta.head = (uint16_t)((slot + 1U) % EVENT_HISTORY_CAPACITY);
    if (s_event_history_meta.count < EVENT_HISTORY_CAPACITY) s_event_history_meta.count++;
    s_event_history_meta.writes++;
    return nvs_set_blob_checked("evtmeta", &s_event_history_meta, sizeof(s_event_history_meta));
}

static bool read_trip_history_slot_locked(uint16_t slot, trip_record_t *out)
{
    if (!out || slot >= TRIP_HISTORY_CAPACITY) return false;
    char key[16];
    trip_history_key(slot, key);
    size_t size = sizeof(*out);
    return nvs_get_blob(s_nvs_handle, key, out, &size) == ESP_OK && size == sizeof(*out);
}

static bool read_event_history_slot_locked(uint16_t slot, event_log_record_t *out)
{
    if (!out || slot >= EVENT_HISTORY_CAPACITY) return false;
    char key[16];
    event_history_key(slot, key);
    size_t size = sizeof(*out);
    return nvs_get_blob(s_nvs_handle, key, out, &size) == ESP_OK && size == sizeof(*out);
}

static esp_err_t clear_trip_history_locked(void)
{
    for (uint16_t i = 0; i < TRIP_HISTORY_CAPACITY; ++i) {
        char key[16];
        trip_history_key(i, key);
        esp_err_t err = nvs_erase_key(s_nvs_handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    }
    memset(&s_trip_history_meta, 0, sizeof(s_trip_history_meta));
    s_trip_history_meta.magic = HISTORY_STORE_MAGIC;
    s_trip_history_meta.version = RING_STORE_VERSION;
    esp_err_t err = nvs_set_blob_checked("histmeta", &s_trip_history_meta, sizeof(s_trip_history_meta));
    return err == ESP_OK ? nvs_commit(s_nvs_handle) : err;
}

static esp_err_t clear_event_history_locked(void)
{
    for (uint16_t i = 0; i < EVENT_HISTORY_CAPACITY; ++i) {
        char key[16];
        event_history_key(i, key);
        esp_err_t err = nvs_erase_key(s_nvs_handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    }
    memset(&s_event_history_meta, 0, sizeof(s_event_history_meta));
    s_event_history_meta.magic = EVENT_STORE_MAGIC;
    s_event_history_meta.version = RING_STORE_VERSION;
    esp_err_t err = nvs_set_blob_checked("evtmeta", &s_event_history_meta, sizeof(s_event_history_meta));
    return err == ESP_OK ? nvs_commit(s_nvs_handle) : err;
}

static void event_storage_task(void *arg)
{
    (void)arg;
    event_log_record_t ev;
    while (true) {
        if (xQueueReceive(s_event_queue, &ev, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGW(TAG, "Event NVS ring lock timeout: %s", ev.code);
                continue;
            }
            esp_err_t err = stage_event_history_locked(&ev);
            if (err == ESP_OK) err = nvs_commit(s_nvs_handle);
            xSemaphoreGive(s_storage_mutex);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Event NVS ring write failed: %s | %s", ev.code, esp_err_to_name(err));
            }
        }
    }
}

static void add_trip_to_totals_locked(const trip_record_t *trip)
{
    if (strcmp(s_totals.daily_date, trip->date) != 0) {
        reset_daily_totals_locked(trip->date);
    }

    s_totals.daily_trip_count++;
    s_totals.daily_distance_km += trip->distance_km;
    s_totals.daily_fuel_l += trip->fuel_l_est;
    s_totals.daily_cost_baht += trip->cost_baht_est;
    s_totals.daily_engine_seconds += trip->engine_seconds;
    s_totals.daily_moving_seconds += trip->moving_seconds;
    s_totals.daily_idle_seconds += trip->idle_seconds;
    s_totals.daily_ac_seconds += trip->ac_seconds;

    s_totals.lifetime_trip_count++;
    s_totals.lifetime_distance_km += trip->distance_km;
    s_totals.lifetime_fuel_l += trip->fuel_l_est;
    s_totals.lifetime_cost_baht += trip->cost_baht_est;
    s_totals.lifetime_engine_seconds += trip->engine_seconds;
    s_totals.lifetime_moving_seconds += trip->moving_seconds;
    s_totals.lifetime_idle_seconds += trip->idle_seconds;
    s_totals.lifetime_ac_seconds += trip->ac_seconds;
    s_totals.last_committed_trip_id = trip->trip_id;
}

static esp_err_t commit_trip_record(const trip_record_t *input_trip)
{
    if (input_trip == NULL || !s_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    trip_record_t trip = *input_trip;
    trip.cost_baht_est = trip.fuel_l_est * trip.fuel_price_baht_l;
    trip.committed = 1;

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const bool is_new_trip = trip.trip_id > s_totals.last_committed_trip_id;
    if (is_new_trip) {
        add_trip_to_totals_locked(&trip);
    } else {
        ESP_LOGW(TAG, "Trip %lu already included in totals/history; not adding twice",
                 (unsigned long)trip.trip_id);
    }

    s_last_trip.magic = STORE_MAGIC;
    s_last_trip.version = STORE_VERSION;
    s_last_trip.valid = 1;
    s_last_trip.trip = trip;

    s_active_snapshot.magic = STORE_MAGIC;
    s_active_snapshot.version = STORE_VERSION;
    s_active_snapshot.active = 0;
    memset(&s_active_snapshot.trip, 0, sizeof(s_active_snapshot.trip));

    esp_err_t err = ESP_OK;
    if (is_new_trip) err = stage_trip_history_locked(&trip);
    if (err == ESP_OK) err = commit_base_store_locked();
    totals_t totals_copy = s_totals;
    xSemaphoreGive(s_storage_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Trip Internal Flash commit failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_system.saved_event_pending = true;
        s_system.saved_event = trip;
        s_system.saved_totals_event = totals_copy;
        xSemaphoreGive(s_state_mutex);
    }

    s_trip_saved_screen_until_us = esp_timer_get_time() +
        ((int64_t)TRIP_SAVED_SCREEN_MS * 1000LL);

    ESP_LOGI(TAG, "Trip %lu committed once | %.3f km | %.3f L EST | %.2f Baht | reason=%s",
             (unsigned long)trip.trip_id,
             trip.distance_km,
             trip.fuel_l_est,
             trip.cost_baht_est,
             trip.end_reason);
    return ESP_OK;
}

static esp_err_t save_active_snapshot(void)
{
    active_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.magic = STORE_MAGIC;
    snapshot.version = STORE_VERSION;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_system.trip.active) {
        snapshot.active = 1;
        snapshot.trip = s_system.trip.record;
    }
    xSemaphoreGive(s_state_mutex);

    if (!snapshot.active) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_active_snapshot = snapshot;
    esp_err_t err = nvs_set_blob_checked("active", &s_active_snapshot, sizeof(s_active_snapshot));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs_handle);
    }
    xSemaphoreGive(s_storage_mutex);

    if (err == ESP_OK) {
        s_last_snapshot_us = esp_timer_get_time();
        s_last_snapshot_distance_km = snapshot.trip.distance_km;
        ESP_LOGI(TAG, "Trip snapshot saved | id=%lu | %.3f km | %.3f L",
                 (unsigned long)snapshot.trip.trip_id,
                 snapshot.trip.distance_km,
                 snapshot.trip.fuel_l_est);
    } else {
        ESP_LOGE(TAG, "Snapshot failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void recover_unfinished_trip(void)
{
    if (!s_active_snapshot.active) {
        return;
    }

    trip_record_t recovered = s_active_snapshot.trip;

    if (recovered.trip_id == 0 || recovered.trip_id <= s_totals.last_committed_trip_id) {
        ESP_LOGW(TAG, "Clearing stale active snapshot");
        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_active_snapshot.active = 0;
            memset(&s_active_snapshot.trip, 0, sizeof(s_active_snapshot.trip));
            commit_base_store_locked();
            xSemaphoreGive(s_storage_mutex);
        }
        return;
    }

    recovered.recovered_after_power_loss = 1;
    recovered.end_uptime_ms = recovered.start_uptime_ms +
                              (uint64_t)(recovered.engine_seconds * 1000.0);
    copy_string(recovered.end_reason, sizeof(recovered.end_reason), "POWER_LOSS");
    recovered.cost_baht_est = recovered.fuel_l_est * recovered.fuel_price_baht_l;

    ESP_LOGW(TAG, "Recovering unfinished trip %lu from last snapshot",
             (unsigned long)recovered.trip_id);
    commit_trip_record(&recovered);
}

/* --------------------------------------------------------------------------
 * USB / FTDI callbacks
 * -------------------------------------------------------------------------- */
static bool handle_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    (void)user_arg;

    if (data == NULL || data_len == 0) {
        return true;
    }

    s_rx_total += data_len;
    s_last_rx_us = esp_timer_get_time();

    if (s_waiting_init_ack && s_events != NULL) {
        for (size_t i = 0; i < data_len; ++i) {
            if (data[i] == 0x10) {
                xEventGroupSetBits(s_events, EVENT_INIT_ACK);
                break;
            }
        }
    }

    if (s_rx_stream != NULL) {
        size_t accepted = xStreamBufferSend(s_rx_stream, data, data_len, 0);
        if (accepted < data_len) {
            s_rx_dropped += (data_len - accepted);
        }
    }

    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    if (event == NULL) {
        return;
    }

    switch (event->type) {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "FTDI/CDC error: %d", event->data.error);
            break;

        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "FTDI disconnected; active trip remains in memory/NVS");
            s_ftdi_connected = false;
            s_ftdi_disconnected = true;
            s_ecu_streaming = false;
            s_consult_session_active = false;
            s_snapshot_requested = true;
            if (s_state_mutex && xSemaphoreTake(s_state_mutex, 0) == pdTRUE) {
                if (s_system.trip.active) {
                    s_system.trip.record.usb_disconnect_count++;
                    queue_trip_event_locked("USB_LOST",
                                            (float)s_system.trip.record.usb_disconnect_count,
                                            "count");
                }
                xSemaphoreGive(s_state_mutex);
            }
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGI(TAG, "FTDI serial state: 0x%04X", event->data.serial_state.val);
            break;

        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
            break;
    }
}

static void usb_host_daemon_task(void *arg)
{
    TaskHandle_t parent = (TaskHandle_t)arg;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_LOGI(TAG, "Installing USB Host Library...");
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host Library installed");
    xTaskNotifyGive(parent);

    while (true) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* --------------------------------------------------------------------------
 * CONSULT decoding and trip state machine
 * -------------------------------------------------------------------------- */
static float clampf_local(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* TPS engineering display conversion.
 * 0.56 V / 4.00 V are conservative GA16 starting calibration points.
 * The original CONSULT voltage is always retained as TPS RAW. */
static float estimate_tps_pct(float tps_v, bool closed_throttle)
{
    const float span = DEFAULT_TPS_WOT_V - DEFAULT_TPS_CLOSED_V;
    if (span <= 0.10f || !isfinite(tps_v)) return 0.0f;
    float pct = ((tps_v - DEFAULT_TPS_CLOSED_V) / span) * 100.0f;
    pct = clampf_local(pct, 0.0f, 100.0f);
    /* The ECU closed-throttle switch is useful near the bottom end where
       sensor tolerance can otherwise display 1-3 % with the plate shut. */
    if (closed_throttle && pct < 8.0f) pct = 0.0f;
    return pct;
}

/* Simple VE model for an engineering MAF estimate.
 * It deliberately uses RPM + calibrated TPS rather than pretending that the
 * Nissan MAF voltage has a known linear V->g/s transfer curve. */
static float estimate_ve(float rpm, float tps_pct, bool closed_throttle)
{
    float ve;
    if (closed_throttle || tps_pct < 3.0f) {
        if (rpm < 1200.0f) ve = 0.28f;
        else if (rpm < 3000.0f) ve = 0.20f;
        else ve = 0.18f;
    } else if (tps_pct < 10.0f) {
        ve = 0.30f;
    } else if (tps_pct < 20.0f) {
        ve = 0.35f;
    } else if (tps_pct < 40.0f) {
        ve = 0.50f;
    } else if (tps_pct < 70.0f) {
        ve = 0.65f;
    } else {
        ve = 0.80f;
    }
    if (rpm > 4500.0f && tps_pct > 40.0f) ve += 0.05f;
    return clampf_local(ve, 0.15f, 0.90f);
}

static bool estimate_maf_gps(float rpm, float tps_pct, bool closed_throttle,
                             float maf_raw_v, float *out_gps)
{
    if (out_gps == NULL || !isfinite(rpm) || rpm < 300.0f ||
        !isfinite(maf_raw_v) || maf_raw_v < MAF_RAW_MIN_VALID_V ||
        maf_raw_v > MAF_RAW_MAX_VALID_V) {
        if (out_gps) *out_gps = 0.0f;
        return false;
    }
    const float ve = estimate_ve(rpm, tps_pct, closed_throttle);
    const float gps = ENGINE_DISPLACEMENT_L * (rpm / 120.0f) * ve * AIR_DENSITY_G_PER_L;
    if (!isfinite(gps) || gps < 0.0f || gps > 250.0f) {
        *out_gps = 0.0f;
        return false;
    }
    *out_gps = gps;
    return true;
}

static const char *o2_state_name(const live_sensor_t *live)
{
    if (live == NULL) return "--";
    if (live->o2_rich) return "RICH";
    if (live->o2_lean) return "LEAN";
    return "MID";
}

static void format_ignition(char *dst, size_t dst_len, int deg_btdc)
{
    if (dst == NULL || dst_len == 0) return;
    if (deg_btdc >= 0) snprintf(dst, dst_len, "%d BTDC", deg_btdc);
    else snprintf(dst, dst_len, "%d ATDC", -deg_btdc);
}

static bool decode_and_validate_frame(const uint8_t p[CONSULT_FRAME_LENGTH],
                                      live_sensor_t *out)
{
    if (p == NULL || out == NULL) {
        return false;
    }

    const uint16_t rpm_raw = ((uint16_t)p[0] << 8) | p[1];
    const uint16_t injector_raw = ((uint16_t)p[2] << 8) | p[3];
    const uint16_t maf_raw = ((uint16_t)p[4] << 8) | p[5];

    live_sensor_t next = {0};
    next.rpm = rpm_raw * 12.5f;
    next.injector_pulse_ms = injector_raw / 100.0f;
    next.maf_voltage_v = maf_raw * 0.005f;
    next.coolant_temp_c = (int)p[6] - 50;
    next.o2_voltage_v = p[7] * 0.01f;
    next.vehicle_speed_kmh = p[8] * 2;
    next.battery_voltage_v = p[9] * 0.08f;
    next.throttle_voltage_v = p[10] * 0.02f;
    next.ignition_timing_btdc_deg = 110 - (int)p[11];
    next.iacv_aac_pct = p[12] / 2.0f;
    next.digital_13 = p[13];
    next.control_1e = p[14];
    next.control_1f = p[15];
    next.af_alpha_pct = p[16];

    next.closed_throttle = (p[13] & 0x01U) != 0;
    next.park_neutral = (p[13] & 0x04U) != 0;
    next.ac_on = (p[13] & 0x20U) != 0;

    next.throttle_position_pct_est =
        estimate_tps_pct(next.throttle_voltage_v, next.closed_throttle);
    next.maf_gps_est_valid = estimate_maf_gps(
        next.rpm, next.throttle_position_pct_est, next.closed_throttle,
        next.maf_voltage_v, &next.maf_gps_est);
    next.o2_rich = next.o2_voltage_v >= O2_RICH_THRESHOLD_V;
    next.o2_lean = next.o2_voltage_v <= O2_LEAN_THRESHOLD_V;
    next.fuel_correction_delta_pct = (float)next.af_alpha_pct - 100.0f;

    next.injector_duty_pct =
        (next.injector_pulse_ms * next.rpm) / 1200.0f;

    float fuel_cal = DEFAULT_FUEL_CALIBRATION;
    float injector_flow = DEFAULT_INJECTOR_FLOW_CC_MIN;

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        fuel_cal = s_settings.fuel_calibration;
        injector_flow = s_settings.injector_flow_cc_min;
        xSemaphoreGive(s_storage_mutex);
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_system.trip.active) {
            fuel_cal = s_system.trip.record.fuel_calibration;
        }
        xSemaphoreGive(s_state_mutex);
    }

    const float full_flow_lph =
        injector_flow * NUMBER_OF_INJECTORS * 60.0f / 1000.0f;
    next.fuel_lph_est =
        full_flow_lph * (next.injector_duty_pct / 100.0f) * fuel_cal;

    if (next.vehicle_speed_kmh >= 5 && next.fuel_lph_est > 0.05f) {
        next.instant_km_per_l_est =
            next.vehicle_speed_kmh / next.fuel_lph_est;
    }

    /* Strict limits prevent first-frame false locks such as 414 km/h,
       157 C, O2 1.65 V or A/F 4% from entering the trip totals. */
    const bool plausible =
        isfinite(next.rpm) && next.rpm >= 0.0f && next.rpm <= 7500.0f &&
        isfinite(next.injector_pulse_ms) && next.injector_pulse_ms >= 0.0f &&
            next.injector_pulse_ms <= 30.0f &&
        isfinite(next.maf_voltage_v) && next.maf_voltage_v >= 0.20f &&
            next.maf_voltage_v <= 5.50f &&
        next.coolant_temp_c >= -20 && next.coolant_temp_c <= 125 &&
        isfinite(next.o2_voltage_v) && next.o2_voltage_v >= 0.0f &&
            next.o2_voltage_v <= 1.20f &&
        next.vehicle_speed_kmh >= 0 && next.vehicle_speed_kmh <= 240 &&
        isfinite(next.battery_voltage_v) && next.battery_voltage_v >= 7.0f &&
            next.battery_voltage_v <= 16.5f &&
        isfinite(next.throttle_voltage_v) && next.throttle_voltage_v >= 0.20f &&
            next.throttle_voltage_v <= 5.20f &&
        next.ignition_timing_btdc_deg >= -20 &&
            next.ignition_timing_btdc_deg <= 60 &&
        isfinite(next.iacv_aac_pct) && next.iacv_aac_pct >= 0.0f &&
            next.iacv_aac_pct <= 100.0f &&
        next.af_alpha_pct >= 50 && next.af_alpha_pct <= 150;

    if (!plausible) {
        return false;
    }

    *out = next;
    return true;
}

static void start_new_trip(const live_sensor_t *live)
{
    settings_t settings_copy;

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Cannot lock settings to start trip");
        return;
    }

    settings_copy = s_settings;
    uint32_t trip_id = s_settings.next_trip_id++;
    esp_err_t settings_err = nvs_set_blob_checked("settings", &s_settings, sizeof(s_settings));
    if (settings_err == ESP_OK) {
        settings_err = nvs_commit(s_nvs_handle);
    }
    xSemaphoreGive(s_storage_mutex);

    if (settings_err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot reserve trip id in NVS: %s", esp_err_to_name(settings_err));
        return;
    }

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }

    if (s_system.trip.active) {
        xSemaphoreGive(s_state_mutex);
        return;
    }

    memset(&s_system.trip, 0, sizeof(s_system.trip));
    s_system.trip.active = true;
    s_system.trip.state = TRIP_RUNNING;
    s_system.trip.record.trip_id = trip_id;
    copy_string(s_system.trip.record.date,
                sizeof(s_system.trip.record.date),
                settings_copy.current_date);
    s_system.trip.record.start_uptime_ms = uptime_ms();
    s_system.trip.record.fuel_price_baht_l = settings_copy.fuel_price_baht_l;
    s_system.trip.record.fuel_calibration = settings_copy.fuel_calibration;
    s_system.trip.record.injector_flow_cc_min = settings_copy.injector_flow_cc_min;
    copy_string(s_system.trip.record.firmware_version,
                sizeof(s_system.trip.record.firmware_version),
                FIRMWARE_VERSION);
    s_system.trip.record.parser_mode_end = (uint8_t)s_parser_mode;
    s_system.trip.record.max_coolant_c = live ? live->coolant_temp_c : -20;
    s_system.trip.record.min_battery_v = live ? live->battery_voltage_v : 99.0f;
    s_system.trip.record.max_rpm = live ? live->rpm : 0.0f;
    s_system.trip.record.max_speed_kmh = live ? live->vehicle_speed_kmh : 0;
    s_system.trip.record.max_fuel_lph = live ? live->fuel_lph_est : 0.0f;
    s_system.trip.record.max_tps_v = live ? live->throttle_voltage_v : 0.0f;
    s_system.trip.record.max_aac_pct = live ? live->iacv_aac_pct : 0.0f;
    if (live) {
        s_system.trip.o2_state_known = true;
        s_system.trip.last_o2_rich = live->o2_voltage_v >= 0.45f;
    }
    copy_string(s_system.trip.record.end_reason,
                sizeof(s_system.trip.record.end_reason),
                "RUNNING");
    s_system.trip.previous_integration_frame_us = esp_timer_get_time();
    xSemaphoreGive(s_state_mutex);

    s_last_snapshot_us = 0;
    s_last_snapshot_distance_km = 0.0;
    s_engine_start_candidate_us = 0;

    ESP_LOGI(TAG, "========== TRIP %lu STARTED | date=%s | price=%.2f | cal=%.3f =========",
             (unsigned long)trip_id,
             settings_copy.current_date,
             settings_copy.fuel_price_baht_l,
             settings_copy.fuel_calibration);

    save_active_snapshot();
}

static void finalize_active_trip(const char *reason, bool recovered)
{
    trip_record_t trip;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    if (!s_system.trip.active || s_system.trip.state == TRIP_FINALIZING) {
        xSemaphoreGive(s_state_mutex);
        return;
    }

    s_system.trip.state = TRIP_FINALIZING;
    s_system.trip.record.parser_mode_end = (uint8_t)s_parser_mode;
    trip = s_system.trip.record;
    trip.end_uptime_ms = uptime_ms();
    trip.recovered_after_power_loss = recovered ? 1 : 0;
    trip.cost_baht_est = trip.fuel_l_est * trip.fuel_price_baht_l;
    copy_string(trip.end_reason, sizeof(trip.end_reason), reason);
    xSemaphoreGive(s_state_mutex);

    esp_err_t err = commit_trip_record(&trip);

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (err == ESP_OK) {
            memset(&s_system.trip, 0, sizeof(s_system.trip));
            s_system.trip.state = TRIP_WAIT_ENGINE;
        } else {
            s_system.trip.state = TRIP_STOP_CONFIRM;
        }
        xSemaphoreGive(s_state_mutex);
    }

    s_engine_start_candidate_us = 0;
}

static void update_trip_state(const live_sensor_t *live, int64_t now_us)
{
    bool should_start = false;
    bool should_finalize = false;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    const uint32_t prior_accepted = s_system.live.accepted_frame_count;
    const uint32_t prior_rejected = s_system.live.rejected_frame_count;
    s_system.live = *live;
    s_system.live.valid = true;
    s_system.live.accepted_frame_count = prior_accepted + 1U;
    s_system.live.rejected_frame_count = prior_rejected;

    trip_runtime_t *trip = &s_system.trip;

    if (!trip->active) {
        if (live->rpm > 300.0f) {
            if (s_engine_start_candidate_us == 0) {
                s_engine_start_candidate_us = now_us;
            }
            if ((now_us - s_engine_start_candidate_us) >=
                ((int64_t)ENGINE_START_CONFIRM_MS * 1000LL)) {
                should_start = true;
            }
        } else {
            s_engine_start_candidate_us = 0;
        }
    } else {
        trip->record.accepted_frames++;

        double dt_s = 0.0;
        if (trip->previous_integration_frame_us != 0) {
            dt_s = (now_us - trip->previous_integration_frame_us) / 1000000.0;
        }
        trip->previous_integration_frame_us = now_us;

        if (live->rpm > 300.0f) {
            trip->state = TRIP_RUNNING;
            trip->engine_off_since_us = 0;

            if (dt_s > 0.0 && dt_s <= 0.5) {
                trip->record.engine_seconds += dt_s;
                trip->record.distance_km +=
                    live->vehicle_speed_kmh * dt_s / 3600.0;
                trip->record.fuel_l_est +=
                    live->fuel_lph_est * dt_s / 3600.0;
                trip->record.rpm_time_integral += live->rpm * dt_s;
                trip->record.maf_time_integral += live->maf_voltage_v * dt_s;
                trip->record.af_alpha_time_integral += live->af_alpha_pct * dt_s;

                if (live->vehicle_speed_kmh > 0) {
                    trip->record.moving_seconds += dt_s;
                } else {
                    trip->record.idle_seconds += dt_s;
                }
                if (live->ac_on) {
                    trip->record.ac_seconds += dt_s;
                }
            }

            trip->record.cost_baht_est =
                trip->record.fuel_l_est * trip->record.fuel_price_baht_l;

            if (live->rpm > trip->record.max_rpm) trip->record.max_rpm = live->rpm;
            if (live->vehicle_speed_kmh > trip->record.max_speed_kmh)
                trip->record.max_speed_kmh = live->vehicle_speed_kmh;
            if (live->fuel_lph_est > trip->record.max_fuel_lph)
                trip->record.max_fuel_lph = live->fuel_lph_est;
            if (live->throttle_voltage_v > trip->record.max_tps_v)
                trip->record.max_tps_v = live->throttle_voltage_v;
            if (live->iacv_aac_pct > trip->record.max_aac_pct)
                trip->record.max_aac_pct = live->iacv_aac_pct;

            if (live->coolant_temp_c > trip->record.max_coolant_c) {
                trip->record.max_coolant_c = live->coolant_temp_c;
            }
            if (live->battery_voltage_v > 1.0f &&
                live->battery_voltage_v < trip->record.min_battery_v) {
                trip->record.min_battery_v = live->battery_voltage_v;
            }

            /* Narrowband O2 switch counter with hysteresis to reduce chatter.
             * This is a diagnostic switching count, NOT an AFR measurement. */
            if (!trip->o2_state_known) {
                trip->o2_state_known = true;
                trip->last_o2_rich = live->o2_voltage_v >= 0.45f;
            } else if (!trip->last_o2_rich && live->o2_voltage_v >= 0.55f) {
                trip->record.o2_switch_count++;
                trip->last_o2_rich = true;
            } else if (trip->last_o2_rich && live->o2_voltage_v <= 0.35f) {
                trip->record.o2_switch_count++;
                trip->last_o2_rich = false;
            }

            const bool ect_warn = live->coolant_temp_c >= WARN_ECT_C;
            if (ect_warn && !trip->warn_ect_active) {
                trip->warn_ect_active = true;
                trip->record.overheat_event_count++;
                queue_trip_event_locked("ECT_HIGH", (float)live->coolant_temp_c, "C");
            } else if (!ect_warn && live->coolant_temp_c <= WARN_ECT_C - 3) {
                trip->warn_ect_active = false;
            }

            const bool low_bat_warn = live->rpm >= WARN_BAT_MIN_RPM &&
                                      live->battery_voltage_v < WARN_LOW_BAT_V;
            if (low_bat_warn && !trip->warn_low_bat_active) {
                trip->warn_low_bat_active = true;
                trip->record.low_battery_event_count++;
                queue_trip_event_locked("BAT_LOW", live->battery_voltage_v, "V");
            } else if (!low_bat_warn && live->battery_voltage_v >= WARN_LOW_BAT_V + 0.3f) {
                trip->warn_low_bat_active = false;
            }

            const bool high_bat_warn = live->battery_voltage_v > WARN_HIGH_BAT_V;
            if (high_bat_warn && !trip->warn_high_bat_active) {
                trip->warn_high_bat_active = true;
                trip->record.high_battery_event_count++;
                queue_trip_event_locked("BAT_HIGH", live->battery_voltage_v, "V");
            } else if (!high_bat_warn && live->battery_voltage_v <= WARN_HIGH_BAT_V - 0.3f) {
                trip->warn_high_bat_active = false;
            }

            const bool safely_parked =
                live->vehicle_speed_kmh == 0 &&
                live->park_neutral &&
                live->closed_throttle;

            if (safely_parked) {
                if (trip->parked_since_us == 0) {
                    trip->parked_since_us = now_us;
                }
                if (!trip->preview_emitted &&
                    (now_us - trip->parked_since_us) >=
                    ((int64_t)PARKED_PREVIEW_DELAY_MS * 1000LL)) {
                    trip->preview_emitted = true;
                    trip->preview_event_pending = true;
                }
            } else {
                trip->parked_since_us = 0;
                trip->preview_emitted = false;
                trip->preview_event_pending = false;
            }
        } else if (live->rpm < 50.0f) {
            trip->state = TRIP_STOP_CONFIRM;
            trip->parked_since_us = 0;
            trip->preview_event_pending = false;

            if (trip->engine_off_since_us == 0) {
                trip->engine_off_since_us = now_us;
            }

            /* Do not add trip time, distance or fuel while RPM is zero. */
            if ((now_us - trip->engine_off_since_us) >=
                ((int64_t)ENGINE_STOP_CONFIRM_MS * 1000LL)) {
                should_finalize = true;
            }
        } else {
            /* Cranking or a brief stall: keep the same trip but do not integrate. */
            trip->engine_off_since_us = 0;
        }
    }

    xSemaphoreGive(s_state_mutex);

    if (should_start) {
        start_new_trip(live);
    }
    if (should_finalize) {
        finalize_active_trip("ENGINE_OFF", false);
    }
}

static bool frame_basic_plausible(const uint8_t p[CONSULT_FRAME_LENGTH], bool require_repeat)
{
    if (p == NULL) return false;
    const uint16_t rpm_raw = ((uint16_t)p[0] << 8) | p[1];
    const uint16_t inj_raw = ((uint16_t)p[2] << 8) | p[3];
    const uint16_t maf_raw = ((uint16_t)p[4] << 8) | p[5];
    const float rpm = rpm_raw * 12.5f;
    const float inj = inj_raw / 100.0f;
    const float maf = maf_raw * 0.005f;
    const int ect = (int)p[6] - 50;
    const float o2 = p[7] * 0.01f;
    const int speed = p[8] * 2;
    const float bat = p[9] * 0.08f;
    const float tps = p[10] * 0.02f;
    const int ign = 110 - (int)p[11];
    const float aac = p[12] / 2.0f;
    const int af = p[16];
    if (require_repeat && RAW_SIGNATURE_REPEAT && p[17] != p[3]) return false;
    return rpm >= 0.0f && rpm <= 7500.0f &&
           inj >= 0.0f && inj <= 30.0f &&
           maf >= 0.20f && maf <= 5.50f &&
           ect >= -20 && ect <= 125 &&
           o2 >= 0.0f && o2 <= 1.20f &&
           speed >= 0 && speed <= 240 &&
           bat >= 7.0f && bat <= 16.5f &&
           tps >= 0.20f && tps <= 5.20f &&
           ign >= -20 && ign <= 60 &&
           aac >= 0.0f && aac <= 100.0f &&
           af >= 50 && af <= 150;
}

static bool raw_frame_signature(const uint8_t p[CONSULT_FRAME_LENGTH])
{
    /* Captures from this B14 show byte 17 repeats injector LSB byte 3. */
    return frame_basic_plausible(p, true);
}

static void update_sensor_data(const uint8_t p[CONSULT_FRAME_LENGTH])
{
    if (s_parser_reset_requested) {
        s_parser_reset_requested = false;
        s_good_frame_streak = 0;
    }

    live_sensor_t decoded;
    if (!decode_and_validate_frame(p, &decoded)) {
        s_good_frame_streak = 0;
        s_invalid_frame_streak++;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_system.live.rejected_frame_count++;
            if (s_system.trip.active) {
                s_system.trip.record.rejected_frames++;
                if (s_invalid_frame_streak == INVALID_FRAME_EVENT_STREAK) {
                    s_system.trip.record.sensor_warning_count++;
                    queue_trip_event_locked("INVALID_FRAMES",
                                            (float)s_invalid_frame_streak,
                                            "count");
                }
            }
            xSemaphoreGive(s_state_mutex);
        }
        return;
    }

    s_invalid_frame_streak = 0;

    if (s_good_frame_streak < GOOD_FRAMES_AFTER_RECONNECT) {
        s_good_frame_streak++;
        if (s_good_frame_streak < GOOD_FRAMES_AFTER_RECONNECT) {
            return;
        }
        ESP_LOGI(TAG, "Frame filter locked after %u consecutive plausible frames",
                 GOOD_FRAMES_AFTER_RECONNECT);
    }

    const int64_t now_us = esp_timer_get_time();
    update_trip_state(&decoded, now_us);

    s_last_valid_frame_us = now_us;
    s_ecu_streaming = true;
    xEventGroupSetBits(s_events, EVENT_VALID_FRAME);
}

static void consult_parser_task(void *arg)
{
    (void)arg;

    uint8_t scan[768];
    size_t scan_len = 0;
    uint8_t input[192];
    bool raw_locked = false;

    while (true) {
        size_t received = xStreamBufferReceive(
            s_rx_stream,
            input,
            sizeof(input),
            portMAX_DELAY
        );

        if (s_parser_reset_requested) {
            s_parser_reset_requested = false;
            scan_len = 0;
            raw_locked = false;
            s_parser_mode = PARSER_SEARCH;
            s_good_frame_streak = 0;
        }

        if (received == 0) continue;

        if (scan_len + received > sizeof(scan)) {
            size_t drop = (scan_len + received) - sizeof(scan);
            if (drop >= scan_len) scan_len = 0;
            else {
                memmove(scan, scan + drop, scan_len - drop);
                scan_len -= drop;
            }
        }
        memcpy(scan + scan_len, input, received);
        scan_len += received;

        bool progressed = true;
        while (progressed) {
            progressed = false;

            /* Mode 1: canonical CONSULT framing FF 12 + 18-byte payload. */
            for (size_t i = 0; i + 2U + CONSULT_FRAME_LENGTH <= scan_len; ++i) {
                if (scan[i] == 0xFF && scan[i + 1] == CONSULT_FRAME_LENGTH) {
                    const uint8_t *payload = &scan[i + 2];
                    if (frame_basic_plausible(payload, false)) {
                        if (s_parser_mode != PARSER_HEADER_FF12) {
                            ESP_LOGI(TAG, "Parser lock: FF 12 headered frame");
                        }
                        s_parser_mode = PARSER_HEADER_FF12;
                        raw_locked = false;
                        update_sensor_data(payload);
                        size_t used = i + 2U + CONSULT_FRAME_LENGTH;
                        memmove(scan, scan + used, scan_len - used);
                        scan_len -= used;
                        progressed = true;
                        break;
                    } else {
                        /* Not a plausible headered sensor frame. Do not destructively
                           discard here: the same bytes may belong to a RAW18 stream.
                           Continue scanning and let RAW18 alignment decide below. */
                        continue;
                    }
                }
            }
            if (progressed) continue;

            /* Mode 2: some FTDI/driver paths deliver only the 18-byte payloads.
               Auto-align using 3 consecutive plausible frames and the B14 capture
               signature payload[17] == payload[3]. */
            if (raw_locked && scan_len >= CONSULT_FRAME_LENGTH) {
                if (raw_frame_signature(scan)) {
                    s_parser_mode = PARSER_RAW18;
                    update_sensor_data(scan);
                    memmove(scan, scan + CONSULT_FRAME_LENGTH,
                            scan_len - CONSULT_FRAME_LENGTH);
                    scan_len -= CONSULT_FRAME_LENGTH;
                    progressed = true;
                    continue;
                }
                raw_locked = false;
                s_parser_mode = PARSER_SEARCH;
                if (scan_len > 0) {
                    memmove(scan, scan + 1, scan_len - 1);
                    scan_len--;
                    progressed = true;
                }
                continue;
            }

            const size_t need = CONSULT_FRAME_LENGTH * RAW_LOCK_FRAMES;
            if (!raw_locked && scan_len >= need) {
                bool found = false;
                for (size_t off = 0; off + need <= scan_len; ++off) {
                    bool ok = true;
                    for (size_t k = 0; k < RAW_LOCK_FRAMES; ++k) {
                        if (!raw_frame_signature(scan + off + k * CONSULT_FRAME_LENGTH)) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        if (off > 0) {
                            memmove(scan, scan + off, scan_len - off);
                            scan_len -= off;
                        }
                        raw_locked = true;
                        s_parser_mode = PARSER_RAW18;
                        ESP_LOGI(TAG, "Parser lock: RAW18 payload stream (%u consecutive frames)", RAW_LOCK_FRAMES);
                        found = true;
                        progressed = true;
                        break;
                    }
                }
                if (!found && scan_len > need + CONSULT_FRAME_LENGTH) {
                    /* Keep enough tail for a future alignment crossing USB packets. */
                    size_t keep = need + CONSULT_FRAME_LENGTH - 1U;
                    memmove(scan, scan + (scan_len - keep), keep);
                    scan_len = keep;
                }
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * FTDI and CONSULT commands
 * -------------------------------------------------------------------------- */
static esp_err_t open_ftdi(void)
{
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 256,
        .in_buffer_size = 0,
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    ESP_LOGI(TAG, "Opening FTDI 0403:6001...");

    esp_err_t err = b14_ftdi_open(&dev_config, &s_ftdi);
    if (err != ESP_OK) {
        return err;
    }

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 9600,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };

    err = b14_ftdi_line_coding_set(s_ftdi, &line_coding);
    if (err != ESP_OK) {
        b14_ftdi_close(s_ftdi);
        s_ftdi = NULL;
        return err;
    }

    err = b14_ftdi_set_control_line_state(s_ftdi, false, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTR/RTS setup: %s", esp_err_to_name(err));
    }

    s_ftdi_connected = true;
    s_ftdi_disconnected = false;
    s_ecu_streaming = false;
    s_consult_session_active = false;
    s_last_rx_us = 0;
    s_parser_mode = PARSER_SEARCH;
    s_rx_total = 0;
    s_rx_dropped = 0;
    s_last_valid_frame_us = 0;
    s_good_frame_streak = 0;
    s_parser_reset_requested = true;

    ESP_LOGI(TAG, "FTDI OPENED | 9600 baud | 8-N-1 | SENSOR REQUESTS ONLY");
    return ESP_OK;
}

static esp_err_t send_consult_bytes(const uint8_t *data,
                                    size_t length,
                                    const char *label)
{
    if (s_ftdi == NULL || !s_ftdi_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    printf("TX %-14s | ", label);
    for (size_t i = 0; i < length; ++i) {
        printf("%02X%s", data[i], (i + 1 < length) ? " " : "");
    }
    putchar('\n');
    fflush(stdout);

    return b14_ftdi_tx_blocking(s_ftdi, data, length, 1000);
}

static bool start_consult_stream(void)
{
    static const uint8_t init_command[] = {0xFF, 0xFF, 0xEF};

    static const uint8_t sensor_request[] = {
        0x30,
        0x5A, 0x00, 0x5A, 0x01, 0x5A, 0x14, 0x5A, 0x15,
        0x5A, 0x04, 0x5A, 0x05, 0x5A, 0x08, 0x5A, 0x09,
        0x5A, 0x0B, 0x5A, 0x0C, 0x5A, 0x0D, 0x5A, 0x16,
        0x5A, 0x17, 0x5A, 0x13, 0x5A, 0x1E, 0x5A, 0x1F,
        0x5A, 0x1A, 0x5A, 0x15,
        0xF0
    };

    xEventGroupClearBits(s_events, EVENT_INIT_ACK | EVENT_VALID_FRAME);
    xStreamBufferReset(s_rx_stream);
    s_parser_reset_requested = true;
    s_good_frame_streak = 0;
    s_last_valid_frame_us = 0;
    s_parser_mode = PARSER_SEARCH;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_system.trip.active) s_system.trip.previous_integration_frame_us = 0;
        xSemaphoreGive(s_state_mutex);
    }

    for (int attempt = 1; attempt <= 3; ++attempt) {
        ESP_LOGI(TAG, "CONSULT initialization attempt %d/3", attempt);
        s_waiting_init_ack = true;
        esp_err_t err = send_consult_bytes(init_command, sizeof(init_command), "INIT");
        if (err != ESP_OK) {
            s_waiting_init_ack = false;
            ESP_LOGE(TAG, "INIT transmit failed: %s", esp_err_to_name(err));
            return false;
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_events, EVENT_INIT_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(1200));
        s_waiting_init_ack = false;

        if (bits & EVENT_INIT_ACK) {
            ESP_LOGI(TAG, "ECU initialization ACK 0x10 received");
            vTaskDelay(pdMS_TO_TICKS(80));
            xEventGroupClearBits(s_events, EVENT_VALID_FRAME);

            uint64_t rx_before = s_rx_total;
            err = send_consult_bytes(sensor_request, sizeof(sensor_request), "SENSOR REQUEST");
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Sensor request failed: %s", esp_err_to_name(err));
                return false;
            }

            s_consult_session_active = true;
            bits = xEventGroupWaitBits(
                s_events, EVENT_VALID_FRAME, pdTRUE, pdFALSE, pdMS_TO_TICKS(3500));

            if (bits & EVENT_VALID_FRAME) {
                ESP_LOGI(TAG, "CONSULT live sensor stream locked | parser=%s", parser_mode_name(s_parser_mode));
                return true;
            }

            uint64_t growth = s_rx_total - rx_before;
            if (growth >= SESSION_RX_MIN_BYTES) {
                ESP_LOGW(TAG,
                    "RX stream is active (%llu bytes) but parser is still auto-syncing; keeping session alive",
                    (unsigned long long)growth);
                return true;
            }

            ESP_LOGW(TAG, "ACK received but sensor RX is not active enough yet");
            s_consult_session_active = false;
        } else {
            ESP_LOGW(TAG, "No ECU ACK. Check plug, ignition ON, clock and power");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "Trying exact captured sensor request once...");
    uint64_t rx_before = s_rx_total;
    esp_err_t err = send_consult_bytes(sensor_request, sizeof(sensor_request), "DIRECT REQUEST");
    if (err != ESP_OK) return false;
    s_consult_session_active = true;

    EventBits_t bits = xEventGroupWaitBits(
        s_events, EVENT_VALID_FRAME, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
    if (bits & EVENT_VALID_FRAME) {
        ESP_LOGI(TAG, "Direct request produced live frames | parser=%s", parser_mode_name(s_parser_mode));
        return true;
    }

    uint64_t growth = s_rx_total - rx_before;
    if (growth >= SESSION_RX_MIN_BYTES) {
        ESP_LOGW(TAG, "Direct request produced %llu RX bytes; parser continues searching without re-init",
                 (unsigned long long)growth);
        return true;
    }

    s_consult_session_active = false;
    ESP_LOGW(TAG, "ECU did not start sensor RX");
    return false;
}

/* --------------------------------------------------------------------------
 * Human-readable output
 * -------------------------------------------------------------------------- */
static void print_totals_block(const totals_t *t)
{
    if (t == NULL) return;

    printf(
        "DAILY %s | trips=%lu | DIST=%.3f km | FUEL_EST=%.3f L | COST_EST=%.2f Baht | AVG=%.2f km/L\n",
        t->daily_date,
        (unsigned long)t->daily_trip_count,
        t->daily_distance_km,
        t->daily_fuel_l,
        t->daily_cost_baht,
        safe_average_km_l(t->daily_distance_km, t->daily_fuel_l)
    );
    printf(
        "LIFETIME | trips=%lu | DIST=%.3f km | FUEL_EST=%.3f L | COST_EST=%.2f Baht | AVG=%.2f km/L\n",
        (unsigned long)t->lifetime_trip_count,
        t->lifetime_distance_km,
        t->lifetime_fuel_l,
        t->lifetime_cost_baht,
        safe_average_km_l(t->lifetime_distance_km, t->lifetime_fuel_l)
    );
}

static void print_trip_summary(const char *title,
                               const trip_record_t *trip,
                               bool saved,
                               const totals_t *totals)
{
    if (trip == NULL) return;

    const uint32_t total_frames = trip->accepted_frames + trip->rejected_frames;
    const double invalid_pct = total_frames > 0 ?
        100.0 * (double)trip->rejected_frames / (double)total_frames : 0.0;

    printf("========== %s ==========\n", title);
    printf("Trip ID        : %lu\n", (unsigned long)trip->trip_id);
    printf("Date           : %s (manual / NO RTC)\n", trip->date);
    printf("Status         : %s\n", saved ? "SAVED" : "PREVIEW - NOT SAVED YET");
    printf("End reason     : %s\n", trip->end_reason);
    printf("Firmware       : %s\n", trip->firmware_version[0] ? trip->firmware_version : "legacy/unknown");
    printf("Parser end     : %s\n", parser_mode_name((parser_mode_t)trip->parser_mode_end));
    printf("Distance       : %.3f km\n", trip->distance_km);
    printf("Fuel used EST  : %.3f L\n", trip->fuel_l_est);
    printf("Average EST    : %.2f km/L\n",
           safe_average_km_l(trip->distance_km, trip->fuel_l_est));
    printf("Fuel cost EST  : %.2f Baht @ %.2f Baht/L\n",
           trip->fuel_l_est * trip->fuel_price_baht_l,
           trip->fuel_price_baht_l);
    printf("Engine time    : %.1f min | Moving %.1f | Idle %.1f | A/C %.1f\n",
           trip->engine_seconds / 60.0,
           trip->moving_seconds / 60.0,
           trip->idle_seconds / 60.0,
           trip->ac_seconds / 60.0);
    printf("Speed          : avg %.1f km/h | max %ld km/h\n",
           safe_average_speed_kmh(trip), (long)trip->max_speed_kmh);
    printf("RPM            : avg %.0f | max %.0f rpm\n",
           safe_average_rpm(trip), trip->max_rpm);
    printf("Fuel rate      : avg %.2f | max %.2f L/h\n",
           safe_average_fuel_lph(trip), trip->max_fuel_lph);
    printf("MAF RAW avg    : %.3f V | TPS max %.1f%% (raw %.2f V) | AAC max %.1f%%\n",
           safe_average_maf_v(trip), estimate_tps_pct(trip->max_tps_v, false),
           trip->max_tps_v, trip->max_aac_pct);
    printf("Fuel corr avg  : %+.1f%% (Alpha %.1f%%) | O2 switches %lu\n",
           safe_average_af_alpha(trip) - 100.0, safe_average_af_alpha(trip),
           (unsigned long)trip->o2_switch_count);
    printf("Max coolant    : %ld C | Min battery %.2f V\n",
           (long)trip->max_coolant_c, trip->min_battery_v);
    printf("Events         : USB=%lu | ECT=%lu | BAT_LOW=%lu | BAT_HIGH=%lu | SENSOR=%lu\n",
           (unsigned long)trip->usb_disconnect_count,
           (unsigned long)trip->overheat_event_count,
           (unsigned long)trip->low_battery_event_count,
           (unsigned long)trip->high_battery_event_count,
           (unsigned long)trip->sensor_warning_count);
    printf("Data quality   : good=%lu bad=%lu invalid=%.3f%%\n",
           (unsigned long)trip->accepted_frames,
           (unsigned long)trip->rejected_frames,
           invalid_pct);
    printf("Fuel model     : injector %.1f cc/min | calibration %.4f\n",
           trip->injector_flow_cc_min, trip->fuel_calibration);
    if (saved && totals != NULL) {
        print_totals_block(totals);
        printf("Trip history   : Internal Flash NVS ring (%u/%u)\n",
               (unsigned)s_trip_history_meta.count, (unsigned)TRIP_HISTORY_CAPACITY);
        printf("Event history  : Internal Flash NVS ring (%u/%u)\n",
               (unsigned)s_event_history_meta.count, (unsigned)EVENT_HISTORY_CAPACITY);
    }
    printf("================================================\n");
}

static void print_live_data(void)
{
    system_state_t state;
    settings_t settings;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    state = s_system;
    s_system.trip.preview_event_pending = false;
    s_system.saved_event_pending = false;
    xSemaphoreGive(s_state_mutex);

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    settings = s_settings;
    xSemaphoreGive(s_storage_mutex);

    if (!state.live.valid) {
        ESP_LOGI(TAG,
                 "Waiting sensor frame | parser=%s | session=%s | IGN=%s | RX=%llu | dropped=%llu | rejected=%lu",
                 parser_mode_name(s_parser_mode),
                 s_consult_session_active ? "ACTIVE" : "OFF",
                 s_ign_seen_on ? (s_ign_stable_on ? "ON" : "OFF") : "--",
                 (unsigned long long)s_rx_total,
                 (unsigned long long)s_rx_dropped,
                 (unsigned long)state.live.rejected_frame_count);
        return;
    }

    char ign_text[20];
    format_ignition(ign_text, sizeof(ign_text), state.live.ignition_timing_btdc_deg);
    if (state.live.maf_gps_est_valid) {
        printf(
            "LIVE | RPM=%4.0f rpm | SPD=%3d km/h | INJ=%4.2f ms | DUTY=%4.2f%% | "
            "MAF_EST=%5.2f g/s (RAW=%4.3f V) | ECT=%3d C | O2=%4.2f V %s | BAT=%4.2f V | "
            "TPS=%5.1f%% (RAW=%4.2f V) | IGN=%s | AAC=%4.1f%% | FUEL_CORR=%+5.1f%% (ALPHA=%3d%%) | "
            "A/C=%s | P/N=%s | THR=%s | FUEL_EST=%4.2f L/h | INST=%5.2f km/L\n",
            state.live.rpm, state.live.vehicle_speed_kmh,
            state.live.injector_pulse_ms, state.live.injector_duty_pct,
            state.live.maf_gps_est, state.live.maf_voltage_v,
            state.live.coolant_temp_c, state.live.o2_voltage_v, o2_state_name(&state.live),
            state.live.battery_voltage_v,
            state.live.throttle_position_pct_est, state.live.throttle_voltage_v,
            ign_text, state.live.iacv_aac_pct, state.live.fuel_correction_delta_pct,
            state.live.af_alpha_pct,
            state.live.ac_on ? "ON " : "OFF", state.live.park_neutral ? "ON " : "OFF",
            state.live.closed_throttle ? "CLOSED" : "OPEN  ",
            state.live.fuel_lph_est, state.live.instant_km_per_l_est);
    } else {
        printf(
            "LIVE | RPM=%4.0f rpm | SPD=%3d km/h | INJ=%4.2f ms | DUTY=%4.2f%% | "
            "MAF_EST=-- g/s (RAW=%4.3f V) | ECT=%3d C | O2=%4.2f V %s | BAT=%4.2f V | "
            "TPS=%5.1f%% (RAW=%4.2f V) | IGN=%s | AAC=%4.1f%% | FUEL_CORR=%+5.1f%% (ALPHA=%3d%%) | "
            "A/C=%s | P/N=%s | THR=%s | FUEL_EST=%4.2f L/h | INST=%5.2f km/L\n",
            state.live.rpm, state.live.vehicle_speed_kmh,
            state.live.injector_pulse_ms, state.live.injector_duty_pct,
            state.live.maf_voltage_v, state.live.coolant_temp_c,
            state.live.o2_voltage_v, o2_state_name(&state.live), state.live.battery_voltage_v,
            state.live.throttle_position_pct_est, state.live.throttle_voltage_v,
            ign_text, state.live.iacv_aac_pct, state.live.fuel_correction_delta_pct,
            state.live.af_alpha_pct,
            state.live.ac_on ? "ON " : "OFF", state.live.park_neutral ? "ON " : "OFF",
            state.live.closed_throttle ? "CLOSED" : "OPEN  ",
            state.live.fuel_lph_est, state.live.instant_km_per_l_est);
    }

    if (state.trip.active) {
        const trip_record_t *t = &state.trip.record;
        printf(
            "TRIP %lu | STATE=%s | DIST=%.3f km | FUEL_EST=%.3f L | AVG_EST=%.2f km/L | "
            "COST_EST=%.2f Baht | ENGINE=%.1f min | IDLE=%.1f min | "
            "DATE=%s | PRICE=%.2f | CAL=%.3f | good=%lu bad=%lu\n",
            (unsigned long)t->trip_id,
            trip_state_name(state.trip.state),
            t->distance_km,
            t->fuel_l_est,
            safe_average_km_l(t->distance_km, t->fuel_l_est),
            t->fuel_l_est * t->fuel_price_baht_l,
            t->engine_seconds / 60.0,
            t->idle_seconds / 60.0,
            t->date,
            t->fuel_price_baht_l,
            t->fuel_calibration,
            (unsigned long)t->accepted_frames,
            (unsigned long)t->rejected_frames
        );
    } else {
        printf(
            "TRIP | STATE=WAIT_ENGINE | next=%lu | date=%s | price=%.2f Baht/L | cal=%.3f\n",
            (unsigned long)settings.next_trip_id,
            settings.current_date,
            settings.fuel_price_baht_l,
            settings.fuel_calibration
        );
    }

    if (state.trip.preview_event_pending && state.trip.active) {
        trip_record_t preview = state.trip.record;
        copy_string(preview.end_reason, sizeof(preview.end_reason), "PARKED");
        print_trip_summary("PARKED PREVIEW", &preview, false, NULL);
    }

    if (state.saved_event_pending) {
        print_trip_summary("TRIP SAVED", &state.saved_event, true,
                           &state.saved_totals_event);
    }

    fflush(stdout);
}


static bool trip_is_active(void);

/* --------------------------------------------------------------------------
 * TFT dashboard + rotary encoder
 * -------------------------------------------------------------------------- */
static esp_err_t save_ui_settings_locked(void)
{
    esp_err_t err = nvs_set_blob_checked("ui", &s_ui_settings, sizeof(s_ui_settings));
    if (err == ESP_OK) err = nvs_commit(s_nvs_handle);
    return err;
}

static void ui_set_page(int page, bool persist)
{
    if (page < 0) page = DASH_PAGE_COUNT - 1;
    if (page >= DASH_PAGE_COUNT) page = 0;
    s_ui_page = page;
    dashboard_set_page((dash_page_t)page);
    if (persist && xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        s_ui_settings.last_page = (uint8_t)page;
        save_ui_settings_locked();
        xSemaphoreGive(s_storage_mutex);
    }
}

static void ui_save_price_or_cal(void)
{
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (s_settings_index == 0) {
        float v = s_settings_edit_value;
        if (v < 1.0f) v = 1.0f;
        if (v > 100.0f) v = 100.0f;
        s_settings.fuel_price_baht_l = roundf(v * 100.0f) / 100.0f;
    } else if (s_settings_index == 1) {
        float v = s_settings_edit_value;
        if (v < 0.20f) v = 0.20f;
        if (v > 3.00f) v = 3.00f;
        s_settings.fuel_calibration = roundf(v * 1000.0f) / 1000.0f;
    }
    esp_err_t err = commit_base_store_locked();
    xSemaphoreGive(s_storage_mutex);
    if (err == ESP_OK) {
        s_ui_saved_flash = true;
        ESP_LOGI(TAG, "Rotary setting saved to NVS");
    } else {
        ESP_LOGE(TAG, "Rotary setting save failed: %s", esp_err_to_name(err));
    }
}

static void ui_toggle_visible(uint32_t bit)
{
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    s_ui_settings.visible_mask ^= bit;
    esp_err_t err = save_ui_settings_locked();
    xSemaphoreGive(s_storage_mutex);
    if (err == ESP_OK) s_ui_saved_flash = true;
}

static void ui_advance_day(void)
{
    if (trip_is_active()) {
        ESP_LOGW(TAG, "NEXT DAY blocked while trip is active");
        return;
    }
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    if (!increment_iso_date(s_settings.current_date)) {
        ESP_LOGW(TAG, "Current manual date is invalid; use DATE YYYY-MM-DD on Serial Monitor");
        xSemaphoreGive(s_storage_mutex);
        return;
    }
    reset_daily_totals_locked(s_settings.current_date);
    esp_err_t err = commit_base_store_locked();
    char date_copy[11];
    copy_string(date_copy, sizeof(date_copy), s_settings.current_date);
    xSemaphoreGive(s_storage_mutex);
    if (err == ESP_OK) {
        s_ui_saved_flash = true;
        ESP_LOGI(TAG, "Manual NO-RTC day advanced -> %s | DAILY reset", date_copy);
    } else {
        ESP_LOGE(TAG, "NEXT DAY save failed: %s", esp_err_to_name(err));
    }
}

static void ui_rotary_event(rotary_event_t ev)
{
    if (ev == ROTARY_EVENT_NONE) return;

    if (s_ui_page != DASH_PAGE_SETTINGS) {
        if (ev == ROTARY_EVENT_CW) ui_set_page(s_ui_page + 1, false);
        else if (ev == ROTARY_EVENT_CCW) ui_set_page(s_ui_page - 1, false);
        else if (ev == ROTARY_EVENT_PRESS) {
            s_settings_index = 0;
            s_settings_editing = false;
            ui_set_page(DASH_PAGE_SETTINGS, false);
        } else if (ev == ROTARY_EVENT_LONG_PRESS) {
            ui_set_page(DASH_PAGE_MAIN, true);
        }
        return;
    }

    if (s_settings_editing) {
        if (ev == ROTARY_EVENT_CW || ev == ROTARY_EVENT_CCW) {
            float step = (s_settings_index == 0) ? 0.10f : 0.001f;
            if (ev == ROTARY_EVENT_CCW) step = -step;
            s_settings_edit_value += step;
            if (s_settings_index == 0) {
                if (s_settings_edit_value < 1.0f) s_settings_edit_value = 1.0f;
                if (s_settings_edit_value > 100.0f) s_settings_edit_value = 100.0f;
            } else {
                if (s_settings_edit_value < 0.20f) s_settings_edit_value = 0.20f;
                if (s_settings_edit_value > 3.00f) s_settings_edit_value = 3.00f;
            }
        } else if (ev == ROTARY_EVENT_PRESS) {
            ui_save_price_or_cal();
            s_settings_editing = false;
        } else if (ev == ROTARY_EVENT_LONG_PRESS) {
            s_settings_editing = false; /* cancel */
        }
        return;
    }

    if (ev == ROTARY_EVENT_CW) {
        s_settings_index = (s_settings_index + 1) % 7;
    } else if (ev == ROTARY_EVENT_CCW) {
        s_settings_index = (s_settings_index + 6) % 7;
    } else if (ev == ROTARY_EVENT_PRESS) {
        if (s_settings_index == 0 || s_settings_index == 1) {
            if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                s_settings_edit_value = (s_settings_index == 0) ?
                    s_settings.fuel_price_baht_l : s_settings.fuel_calibration;
                xSemaphoreGive(s_storage_mutex);
                s_settings_editing = true;
            }
        } else if (s_settings_index == 2) ui_toggle_visible(UI_VISIBLE_O2);
        else if (s_settings_index == 3) ui_toggle_visible(UI_VISIBLE_AAC);
        else if (s_settings_index == 4) ui_toggle_visible(UI_VISIBLE_AF);
        else if (s_settings_index == 5) ui_advance_day();
        else if (s_settings_index == 6) ui_set_page(DASH_PAGE_MAIN, true);
    } else if (ev == ROTARY_EVENT_LONG_PRESS) {
        ui_set_page(DASH_PAGE_MAIN, true);
    }
}

static void rotary_task(void *arg)
{
    (void)arg;
    if (!rotary_encoder_init()) {
        ESP_LOGW(TAG, "Rotary encoder GPIO init failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Rotary ready | CLK=GPIO5 DT=GPIO6 SW=GPIO7 | + must be 3.3V");
    while (true) {
        rotary_event_t ev = rotary_encoder_poll();
        if (ev != ROTARY_EVENT_NONE) ui_rotary_event(ev);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void dashboard_task(void *arg)
{
    (void)arg;
    s_tft_ready = dashboard_init();
    if (s_tft_ready) {
        dashboard_show_splash("JOEVOHAN@261", 2600);
    }
    if (!s_tft_ready) {
        ESP_LOGW(TAG, "TFT disabled; Serial Monitor remains fully functional");
        vTaskDelete(NULL);
        return;
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_ui_page = s_ui_settings.last_page;
        xSemaphoreGive(s_storage_mutex);
    }
    ui_set_page(s_ui_page, false);

    int64_t last_saved_screen_seen = 0;
    while (true) {
        system_state_t state;
        settings_t settings;
        totals_t totals;
        last_trip_store_t last_trip;
        uint32_t visible = UI_VISIBLE_DEFAULT;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            state = s_system;
            xSemaphoreGive(s_state_mutex);
        } else {
            memset(&state, 0, sizeof(state));
        }
        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            settings = s_settings;
            totals = s_totals;
            last_trip = s_last_trip;
            visible = s_ui_settings.visible_mask;
            xSemaphoreGive(s_storage_mutex);
        } else {
            memset(&settings, 0, sizeof(settings));
            memset(&totals, 0, sizeof(totals));
            memset(&last_trip, 0, sizeof(last_trip));
        }

        const int64_t now_us = esp_timer_get_time();
        if (s_trip_saved_screen_until_us > now_us &&
            s_trip_saved_screen_until_us != last_saved_screen_seen) {
            last_saved_screen_seen = s_trip_saved_screen_until_us;
            ui_set_page(DASH_PAGE_TRIP, false);
        }

        dashboard_view_t v = {0};
        v.display_ok = s_tft_ready;
        v.ftdi_connected = s_ftdi_connected;
        v.consult_session_active = s_consult_session_active;
        v.sensor_valid = state.live.valid;
        v.rx_total = s_rx_total;
        v.accepted_frames = state.live.accepted_frame_count;
        v.rejected_frames = state.live.rejected_frame_count;
        v.parser_mode = parser_mode_name(s_parser_mode);
        v.ign_seen_on = s_ign_seen_on;
        v.ign_on = s_ign_stable_on;
        v.rpm = state.live.rpm;
        v.speed_kmh = state.live.vehicle_speed_kmh;
        v.injector_ms = state.live.injector_pulse_ms;
        v.injector_duty = state.live.injector_duty_pct;
        v.maf_v = state.live.maf_voltage_v;
        v.maf_gps_est = state.live.maf_gps_est;
        v.maf_gps_valid = state.live.maf_gps_est_valid;
        v.ect_c = state.live.coolant_temp_c;
        v.o2_v = state.live.o2_voltage_v;
        v.o2_rich = state.live.o2_rich;
        v.o2_lean = state.live.o2_lean;
        v.battery_v = state.live.battery_voltage_v;
        v.tps_v = state.live.throttle_voltage_v;
        v.tps_pct = state.live.throttle_position_pct_est;
        v.ignition_deg = state.live.ignition_timing_btdc_deg;
        v.aac_pct = state.live.iacv_aac_pct;
        v.af_alpha_pct = state.live.af_alpha_pct;
        v.fuel_corr_pct = state.live.fuel_correction_delta_pct;
        v.ac_on = state.live.ac_on;
        v.park_neutral = state.live.park_neutral;
        v.closed_throttle = state.live.closed_throttle;
        v.fuel_lph = state.live.fuel_lph_est;
        v.instant_km_l = state.live.instant_km_per_l_est;

        v.trip_active = state.trip.active;
        if (state.trip.active) {
            v.trip_showing_last = false;
            v.trip_id = state.trip.record.trip_id;
            v.trip_end_reason = "RUNNING";
            v.trip_distance_km = state.trip.record.distance_km;
            v.trip_fuel_l = state.trip.record.fuel_l_est;
            v.trip_cost_baht = state.trip.record.fuel_l_est * state.trip.record.fuel_price_baht_l;
            v.trip_avg_km_l = safe_average_km_l(state.trip.record.distance_km, state.trip.record.fuel_l_est);
            v.trip_engine_min = state.trip.record.engine_seconds / 60.0;
            v.trip_idle_min = state.trip.record.idle_seconds / 60.0;
            v.trip_ac_min = state.trip.record.ac_seconds / 60.0;
        } else if (last_trip.valid) {
            const trip_record_t *lt = &last_trip.trip;
            v.trip_showing_last = true;
            v.trip_id = lt->trip_id;
            v.trip_end_reason = lt->end_reason;
            v.trip_distance_km = lt->distance_km;
            v.trip_fuel_l = lt->fuel_l_est;
            v.trip_cost_baht = lt->cost_baht_est;
            v.trip_avg_km_l = safe_average_km_l(lt->distance_km, lt->fuel_l_est);
            v.trip_engine_min = lt->engine_seconds / 60.0;
            v.trip_idle_min = lt->idle_seconds / 60.0;
            v.trip_ac_min = lt->ac_seconds / 60.0;
        }
        v.daily_date = totals.daily_date;
        v.daily_trips = totals.daily_trip_count;
        v.daily_distance_km = totals.daily_distance_km;
        v.daily_cost_baht = totals.daily_cost_baht;
        v.lifetime_trips = totals.lifetime_trip_count;
        v.lifetime_distance_km = totals.lifetime_distance_km;
        v.lifetime_cost_baht = totals.lifetime_cost_baht;
        v.fuel_price = settings.fuel_price_baht_l;
        v.calibration = settings.fuel_calibration;
        v.current_date = settings.current_date;
        v.visible_mask = visible;
        v.settings_index = s_settings_index;
        v.settings_editing = s_settings_editing;
        v.edit_value = s_settings_edit_value;
        v.just_saved = s_ui_saved_flash;
        dashboard_render(&v);
        s_ui_saved_flash = false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* --------------------------------------------------------------------------
 * Serial Monitor commands on COM4
 * -------------------------------------------------------------------------- */
static void print_help(void)
{
    printf(
        "\nCOMMANDS (type then press Enter)\n"
        "  HELP                    show commands\n"
        "  STATUS                  current settings and state\n"
        "  DISPLAYSTATUS           TFT power-on/reset/backlight configuration\n"
        "  UNITS                   engineering-unit model and raw/derived rules\n"
        "  DATE YYYY-MM-DD         set manual trip date; NO RTC; resets DAILY if changed\n"
        "  NEXTDAY                 advance manual date by one day; resets DAILY\n"
        "  PRICE 36.70             fuel price for NEXT trip\n"
        "  CAL 1.000               fuel calibration for NEXT trip\n"
        "  TOTALS                  daily and lifetime totals\n"
        "  LAST                    last saved trip\n"
        "  HISTORY                 last 10 trips from Internal Flash\n"
        "  EVENTS                  last 10 warning/events from Internal Flash\n"
        "  FLASHSTATUS             Internal Flash/NVS usage and ring status\n"
        "  NVSSTATUS               same storage health report\n"
        "  SAVE                    force active-trip NVS snapshot\n"
        "  IGNOFF                  simulate IGN-OFF save for bench test\n"
        "  RESETDAILY CONFIRM      clear daily only; lifetime remains\n"
        "  CLEARHISTORY CONFIRM    erase trip history ring only\n"
        "  CLEAREVENTS CONFIRM     erase event history ring only\n\n"
    );
    fflush(stdout);
}

static void print_display_status_command(void)
{
    printf("\nDISPLAY STATUS v3.9.10 POWER-ON FIX\n");
    printf("  TFT            : %s\n", s_tft_ready ? "READY" : "NOT_READY");
    printf("  Panel          : ILI9488 SPI 480x320 @ 10MHz\n");
    printf("  Backlight      : GPIO18 active-high | OFF during boot, ON after splash drawn\n");
    printf("  Hardware reset : GPIO8 | held LOW 350ms, release wait 180ms\n");
    printf("  CS boot state  : GPIO10 HIGH before SPI initialization\n");
    printf("  Init retry     : 3 attempts | reset wait 180ms | init settle 120ms\n");
    printf("  Boot symptom   : simultaneous TFT+ESP32 power white-screen mitigation ENABLED\n");
    fflush(stdout);
}

static void print_units_command(void)
{
    printf("\nENGINEERING UNITS v3.9.10\n");
    printf("  MAF EST : g/s ESTIMATE | model=1.60L*(RPM/120)*VE*1.18g/L | RAW V retained\n");
    printf("  TPS     : 0-100%% EST | CLOSED=%.2fV | WOT=%.2fV | RAW V retained\n",
           DEFAULT_TPS_CLOSED_V, DEFAULT_TPS_WOT_V);
    printf("  O2      : V + state | LEAN<=%.2fV | RICH>=%.2fV | MID between | narrowband, NOT AFR\n",
           O2_LEAN_THRESHOLD_V, O2_RICH_THRESHOLD_V);
    printf("  IGN     : degrees | positive=BTDC | negative=ATDC\n");
    printf("  FUEL CORR: Alpha-100%% | e.g. Alpha 97%% => -3%%, Alpha 105%% => +5%%\n");
    printf("  Standard: RPM rpm | SPD km/h | INJ ms | DUTY %% | ECT C | BAT V | AAC %% | FUEL L/h | ECON km/L\n");
    fflush(stdout);
}

static void print_status_command(void)
{
    system_state_t st;
    settings_t set;
    totals_t tot;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        st = s_system;
        xSemaphoreGive(s_state_mutex);
    } else {
        memset(&st, 0, sizeof(st));
    }

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        set = s_settings;
        tot = s_totals;
        xSemaphoreGive(s_storage_mutex);
    } else {
        memset(&set, 0, sizeof(set));
        memset(&tot, 0, sizeof(tot));
    }

    printf("\nSTATUS | FW=%s | FTDI=%s | SESSION=%s | ECU_STREAM=%s | PARSER=%s | IGN=%s | TRIP=%s | ID=%lu\n",
           FIRMWARE_VERSION,
           s_ftdi_connected ? "CONNECTED" : "DISCONNECTED",
           s_consult_session_active ? "ACTIVE" : "OFF",
           s_ecu_streaming ? "ON" : "OFF",
           parser_mode_name(s_parser_mode),
           s_ign_seen_on ? (s_ign_stable_on ? "ON" : "OFF") : "NOT_WIRED",
           st.trip.active ? trip_state_name(st.trip.state) : "WAIT_ENGINE",
           st.trip.active ? (unsigned long)st.trip.record.trip_id : 0UL);
    printf("SETTINGS | DATE=%s MANUAL/NO-RTC | PRICE=%.2f | CAL=%.4f | INJECTOR=%.1f cc/min | NEXT=%lu\n",
           set.current_date,
           set.fuel_price_baht_l,
           set.fuel_calibration,
           set.injector_flow_cc_min,
           (unsigned long)set.next_trip_id);
    print_totals_block(&tot);
    printf("STORAGE | INTERNAL_FLASH=NVS | TRIPS=%u/%u | EVENTS=%u/%u | SD=NOT_USED | RTC=NOT_USED\n",
           (unsigned)s_trip_history_meta.count, (unsigned)TRIP_HISTORY_CAPACITY,
           (unsigned)s_event_history_meta.count, (unsigned)EVENT_HISTORY_CAPACITY);
    fflush(stdout);
}

static void print_last_trip_command(void)
{
    last_trip_store_t last;
    totals_t totals;

    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    last = s_last_trip;
    totals = s_totals;
    xSemaphoreGive(s_storage_mutex);

    if (!last.valid) {
        printf("No saved trip yet.\n");
    } else {
        print_trip_summary("LAST SAVED TRIP", &last.trip, true, &totals);
    }
}

static void print_history_tail(void)
{
    if (!s_nvs_ready) {
        printf("Internal Flash/NVS is not ready.\n");
        return;
    }
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    const uint16_t count = s_trip_history_meta.count;
    const uint16_t n = count < HISTORY_PRINT_LINES ? count : HISTORY_PRINT_LINES;
    printf("\nLAST %u TRIPS | INTERNAL FLASH NVS RING %u/%u\n",
           (unsigned)n, (unsigned)count, (unsigned)TRIP_HISTORY_CAPACITY);
    printf(" ID     DATE        KM       FUEL(L)   AVG(km/L)  MAXSPD  MAXRPM  END\n");
    printf("-----------------------------------------------------------------------\n");
    for (uint16_t i = 0; i < n; ++i) {
        int slot = (int)s_trip_history_meta.head - 1 - (int)i;
        while (slot < 0) slot += (int)TRIP_HISTORY_CAPACITY;
        trip_record_t trip = {0};
        if (read_trip_history_slot_locked((uint16_t)slot, &trip)) {
            printf(" %-6lu %-10s %8.2f %8.3f %10.2f %7ld %7.0f  %s\n",
                   (unsigned long)trip.trip_id, trip.date, trip.distance_km,
                   trip.fuel_l_est, safe_average_km_l(trip.distance_km, trip.fuel_l_est),
                   (long)trip.max_speed_kmh, trip.max_rpm, trip.end_reason);
        }
    }
    xSemaphoreGive(s_storage_mutex);
    fflush(stdout);
}

static void print_event_tail(void)
{
    if (!s_nvs_ready) {
        printf("Internal Flash/NVS is not ready.\n");
        return;
    }
    if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;

    const uint16_t count = s_event_history_meta.count;
    const uint16_t n = count < HISTORY_PRINT_LINES ? count : HISTORY_PRINT_LINES;
    printf("\nLAST %u EVENTS | INTERNAL FLASH NVS RING %u/%u\n",
           (unsigned)n, (unsigned)count, (unsigned)EVENT_HISTORY_CAPACITY);
    printf(" TRIP   DATE        UPTIME(ms)    EVENT                   VALUE UNIT\n");
    printf("-----------------------------------------------------------------------\n");
    for (uint16_t i = 0; i < n; ++i) {
        int slot = (int)s_event_history_meta.head - 1 - (int)i;
        while (slot < 0) slot += (int)EVENT_HISTORY_CAPACITY;
        event_log_record_t ev = {0};
        if (read_event_history_slot_locked((uint16_t)slot, &ev)) {
            printf(" %-6lu %-10s %-12" PRIu64 " %-22s %7.3f %s\n",
                   (unsigned long)ev.trip_id, ev.date, ev.uptime_ms,
                   ev.code, ev.value, ev.unit);
        }
    }
    xSemaphoreGive(s_storage_mutex);
    fflush(stdout);
}

static void print_flash_status(void)
{
    nvs_stats_t stats = {0};
    esp_err_t err = nvs_get_stats(NVS_PARTITION_LABEL, &stats);
    printf("\nINTERNAL FLASH / NVS STATUS\n");
    printf("  state        : %s\n", s_nvs_ready ? "READY" : "ERR");
    printf("  SD card      : NOT USED\n");
    printf("  RTC          : NOT USED\n");
    printf("  partition    : %s\n", NVS_PARTITION_LABEL);
    printf("  NVS stats    : %s\n", esp_err_to_name(err));
    if (err == ESP_OK) {
        printf("  used entries : %lu\n", (unsigned long)stats.used_entries);
        printf("  free entries : %lu\n", (unsigned long)stats.free_entries);
        printf("  namespaces   : %lu\n", (unsigned long)stats.namespace_count);
    }
    printf("  trip ring    : %u/%u | head=%u | writes=%lu\n",
           (unsigned)s_trip_history_meta.count, (unsigned)TRIP_HISTORY_CAPACITY,
           (unsigned)s_trip_history_meta.head, (unsigned long)s_trip_history_meta.writes);
    printf("  event ring   : %u/%u | head=%u | writes=%lu\n",
           (unsigned)s_event_history_meta.count, (unsigned)EVENT_HISTORY_CAPACITY,
           (unsigned)s_event_history_meta.head, (unsigned long)s_event_history_meta.writes);
    printf("  active snap  : %s\n", s_active_snapshot.active ? "PRESENT" : "NONE");
    printf("  last trip    : %s\n", s_last_trip.valid ? "PRESENT" : "NONE");
    fflush(stdout);
}

static bool trip_is_active(void)
{
    bool active = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = s_system.trip.active;
        xSemaphoreGive(s_state_mutex);
    }
    return active;
}

static void process_command(char *line)
{
    if (line == NULL) return;

    while (isspace((unsigned char)*line)) line++;
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char cmd[24] = {0};
    char arg[48] = {0};
    sscanf(line, "%23s %47[^\r\n]", cmd, arg);

    if (strcasecmp(cmd, "HELP") == 0) {
        print_help();
        return;
    }
    if (strcasecmp(cmd, "STATUS") == 0) {
        print_status_command();
        return;
    }
    if (strcasecmp(cmd, "DISPLAYSTATUS") == 0) {
        print_display_status_command();
        return;
    }
    if (strcasecmp(cmd, "UNITS") == 0) {
        print_units_command();
        return;
    }
    if (strcasecmp(cmd, "TOTALS") == 0) {
        totals_t totals;
        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            totals = s_totals;
            xSemaphoreGive(s_storage_mutex);
            print_totals_block(&totals);
        }
        return;
    }
    if (strcasecmp(cmd, "LAST") == 0) {
        print_last_trip_command();
        return;
    }
    if (strcasecmp(cmd, "HISTORY") == 0) {
        print_history_tail();
        return;
    }
    if (strcasecmp(cmd, "EVENTS") == 0) {
        print_event_tail();
        return;
    }
    if (strcasecmp(cmd, "FLASHSTATUS") == 0 || strcasecmp(cmd, "NVSSTATUS") == 0) {
        print_flash_status();
        return;
    }
    if (strcasecmp(cmd, "CLEARHISTORY") == 0) {
        if (strcasecmp(arg, "CONFIRM") != 0) {
            printf("Use: CLEARHISTORY CONFIRM\n");
            return;
        }
        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_err_t err = clear_trip_history_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("CLEARHISTORY: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "CLEAREVENTS") == 0) {
        if (strcasecmp(arg, "CONFIRM") != 0) {
            printf("Use: CLEAREVENTS CONFIRM\n");
            return;
        }
        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            esp_err_t err = clear_event_history_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("CLEAREVENTS: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "SAVE") == 0) {
        if (!trip_is_active()) {
            printf("No active trip to snapshot.\n");
        } else {
            esp_err_t err = save_active_snapshot();
            printf("Snapshot: %s\n", esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "NEXTDAY") == 0) {
        ui_advance_day();
        return;
    }
    if (strcasecmp(cmd, "IGNOFF") == 0) {
        if (!trip_is_active()) {
            printf("No active trip; IGNOFF test did nothing.\n");
        } else {
            printf("Simulating IGN OFF -> snapshot + final commit.\n");
            s_ign_off_requested = true;
            s_snapshot_requested = true;
        }
        return;
    }
    if (strcasecmp(cmd, "DATE") == 0) {
        if (!valid_iso_date(arg)) {
            printf("Use: DATE YYYY-MM-DD\n");
            return;
        }
        if (trip_is_active()) {
            printf("DATE rejected while a trip is active. Set it after engine OFF and TRIP SAVED.\n");
            return;
        }

        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            copy_string(s_settings.current_date, sizeof(s_settings.current_date), arg);
            if (strcmp(s_totals.daily_date, arg) != 0) {
                reset_daily_totals_locked(arg);
            }
            esp_err_t err = commit_base_store_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("DATE set to %s | NVS=%s\n", arg, esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "PRICE") == 0) {
        char *end = NULL;
        float price = strtof(arg, &end);
        if (end == arg || !isfinite(price) || price < 1.0f || price > 100.0f) {
            printf("Use: PRICE 36.70\n");
            return;
        }

        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_settings.fuel_price_baht_l = price;
            esp_err_t err = commit_base_store_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("Fuel price set %.2f Baht/L for NEXT trip | NVS=%s\n",
                   price, esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "CAL") == 0) {
        char *end = NULL;
        float cal = strtof(arg, &end);
        if (end == arg || !isfinite(cal) || cal < 0.20f || cal > 3.00f) {
            printf("Use: CAL 1.000 (allowed 0.20 to 3.00)\n");
            return;
        }

        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            s_settings.fuel_calibration = cal;
            esp_err_t err = commit_base_store_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("Fuel calibration set %.4f for NEXT trip | NVS=%s\n",
                   cal, esp_err_to_name(err));
        }
        return;
    }
    if (strcasecmp(cmd, "RESETDAILY") == 0) {
        if (strcasecmp(arg, "CONFIRM") != 0) {
            printf("Use: RESETDAILY CONFIRM\n");
            return;
        }
        if (trip_is_active()) {
            printf("Cannot reset daily while a trip is active.\n");
            return;
        }

        if (xSemaphoreTake(s_storage_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            reset_daily_totals_locked(s_settings.current_date);
            esp_err_t err = commit_base_store_locked();
            xSemaphoreGive(s_storage_mutex);
            printf("Daily totals reset; lifetime unchanged | NVS=%s\n", esp_err_to_name(err));
        }
        return;
    }

    printf("Unknown command: %s | type HELP\n", cmd);
    fflush(stdout);
}

static void console_command_task(void *arg)
{
    (void)arg;

    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t err = uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UART command input disabled: %s", esp_err_to_name(err));
            vTaskDelete(NULL);
            return;
        }
    }

    char line[96];
    size_t pos = 0;
    ESP_LOGI(TAG, "Serial commands ready; type HELP then Enter");

    while (true) {
        uint8_t ch = 0;
        int n = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                printf("\nCMD> %s\n", line);
                process_command(line);
                pos = 0;
            }
            continue;
        }

        if (ch == 0x08 || ch == 0x7F) {
            if (pos > 0) pos--;
            continue;
        }

        if (isprint(ch) && pos < sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}


static void ignition_sense_task(void *arg)
{
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << IGN_SENSE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IGN sense GPIO init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    bool candidate = gpio_get_level(IGN_SENSE_GPIO) == IGN_ACTIVE_LEVEL;
    bool stable = candidate;
    int64_t candidate_since_us = esp_timer_get_time();
    s_ign_stable_on = stable;
    if (stable) s_ign_seen_on = true;

    ESP_LOGI(TAG,
             "IGN sense ready | GPIO%d active-%s | NO RTC | protected 12V interface required",
             (int)IGN_SENSE_GPIO,
             IGN_ACTIVE_LEVEL ? "HIGH" : "LOW");

    while (true) {
        const bool raw = gpio_get_level(IGN_SENSE_GPIO) == IGN_ACTIVE_LEVEL;
        const int64_t now_us = esp_timer_get_time();
        if (raw != candidate) {
            candidate = raw;
            candidate_since_us = now_us;
        }
        if (candidate != stable &&
            (now_us - candidate_since_us) >= ((int64_t)IGN_DEBOUNCE_MS * 1000LL)) {
            const bool was_on = stable;
            stable = candidate;
            s_ign_stable_on = stable;
            if (stable) {
                s_ign_seen_on = true;
                ESP_LOGI(TAG, "IGN ON detected");
            } else if (was_on && s_ign_seen_on) {
                ESP_LOGI(TAG, "IGN OFF detected -> graceful trip save requested");
                s_ign_off_requested = true;
                s_snapshot_requested = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void service_ignition_off_if_needed(void)
{
    if (!s_ign_off_requested) return;
    s_ign_off_requested = false;

    bool active = false;
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        active = s_system.trip.active;
        if (active) {
            queue_trip_event_locked("IGN_OFF", 0.0f, "");
        }
        xSemaphoreGive(s_state_mutex);
    }

    if (!active) {
        ESP_LOGI(TAG, "IGN OFF: no active trip to finalize");
        return;
    }

    /* The board must be powered from fused B+ through an automotive buck.
     * IGN/ACC is only a sense input, so NVS + FAT can complete after key-off. */
    esp_err_t snap = save_active_snapshot();
    if (snap != ESP_OK) {
        ESP_LOGW(TAG, "Final IGN snapshot failed (%s); attempting final trip commit anyway",
                 esp_err_to_name(snap));
    }
    finalize_active_trip("IGN_OFF", false);
    if (trip_is_active()) {
        ESP_LOGE(TAG, "IGN OFF final commit did not complete; retrying while B+ power remains");
        vTaskDelay(pdMS_TO_TICKS(500));
        s_ign_off_requested = true;
    }
}

static void service_snapshot_if_needed(void)
{
    bool active = false;
    double distance = 0.0;

    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = s_system.trip.active;
        distance = s_system.trip.record.distance_km;
        xSemaphoreGive(s_state_mutex);
    }

    if (!active) {
        s_snapshot_requested = false;
        return;
    }

    const int64_t now_us = esp_timer_get_time();
    const bool timed_snapshot =
        s_last_snapshot_us == 0 ||
        (now_us - s_last_snapshot_us) >=
        ((int64_t)SNAPSHOT_INTERVAL_MS * 1000LL);
    const bool distance_snapshot =
        (distance - s_last_snapshot_distance_km) >= SNAPSHOT_DISTANCE_STEP_KM;

    if (s_snapshot_requested || timed_snapshot || distance_snapshot) {
        s_snapshot_requested = false;
        save_active_snapshot();
    }
}

/* --------------------------------------------------------------------------
 * Main application
 * -------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "Nissan B14 CONSULT Dashboard %s | FULL INTERNAL FLASH | NO-SD | NO-RTC", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Storage: NVS settings + snapshot + 100-trip ring + 128-event ring");
    ESP_LOGI(TAG, "USB Host GPIO19 D- / GPIO20 D+");
    ESP_LOGW(TAG, "Only captured DATA MONITOR requests are transmitted");
    ESP_LOGW(TAG, "Fuel, km/L and money are estimates until calibrated");

    s_rx_stream = xStreamBufferCreate(RX_STREAM_BUFFER_SIZE, 1);
    s_events = xEventGroupCreate();
    s_state_mutex = xSemaphoreCreateMutex();
    s_storage_mutex = xSemaphoreCreateMutex();
    s_ui_mutex = xSemaphoreCreateMutex();
    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_log_record_t));

    if (s_rx_stream == NULL || s_events == NULL ||
        s_state_mutex == NULL || s_storage_mutex == NULL || s_ui_mutex == NULL ||
        s_event_queue == NULL) {
        ESP_LOGE(TAG, "Cannot allocate RTOS objects");
        return;
    }

    s_system.trip.state = TRIP_WAIT_ENGINE;

    if (init_persistent_storage() != ESP_OK) {
        ESP_LOGE(TAG, "Persistent NVS is required; stopping");
        return;
    }

    recover_unfinished_trip();

    BaseType_t ok = xTaskCreatePinnedToCore(
        usb_host_daemon_task,
        "usb_host_daemon",
        4096,
        xTaskGetCurrentTaskHandle(),
        2,
        NULL,
        0
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Cannot create USB host daemon task");
        return;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "Installing CDC-ACM host driver...");
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    ESP_LOGI(TAG, "CDC-ACM host driver installed");

    ok = xTaskCreatePinnedToCore(
        consult_parser_task,
        "consult_parser",
        6144,
        NULL,
        3,
        NULL,
        1
    );
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Cannot create CONSULT parser task");
        return;
    }

    ok = xTaskCreatePinnedToCore(
        console_command_task,
        "console_commands",
        4096,
        NULL,
        1,
        NULL,
        0
    );
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Cannot create command task; monitor output still works");
    }

    ok = xTaskCreatePinnedToCore(
        dashboard_task, "dashboard", 8192, NULL, 1, NULL, 1);
    if (ok != pdPASS) ESP_LOGW(TAG, "Cannot create TFT dashboard task");

    ok = xTaskCreatePinnedToCore(
        rotary_task, "rotary", 3072, NULL, 1, NULL, 1);
    if (ok != pdPASS) ESP_LOGW(TAG, "Cannot create rotary task");

    ok = xTaskCreatePinnedToCore(
        event_storage_task, "event_storage", 4096, NULL, 1, NULL, 0);
    if (ok != pdPASS) ESP_LOGW(TAG, "Cannot create event storage task");

    ok = xTaskCreatePinnedToCore(
        ignition_sense_task, "ign_sense", 3072, NULL, 1, NULL, 0);
    if (ok != pdPASS) ESP_LOGW(TAG, "Cannot create IGN sense task");

    print_help();

    uint32_t last_print_ms = 0;

    while (true) {
        service_ignition_off_if_needed();
        service_snapshot_if_needed();

        const int64_t loop_now_us = esp_timer_get_time();
        const uint32_t loop_now_ms = (uint32_t)(loop_now_us / 1000LL);
        if ((loop_now_ms - last_print_ms) >= PRINT_INTERVAL_MS) {
            last_print_ms = loop_now_ms;
            print_live_data();
        }

        /* After IGN has been observed ON at least once, key-OFF becomes a real
         * standby state. Do not keep hammering ECU init while the car is off.
         * If IGN sense is not wired, s_ign_seen_on stays false and legacy
         * RPM/CONSULT behavior remains unchanged. */
        if (s_ign_seen_on && !s_ign_stable_on) {
            s_consult_session_active = false;
            s_ecu_streaming = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        if (s_ftdi == NULL) {
            ESP_LOGI(TAG, "Waiting for direct FTDI 0403:6001 connection...");
            esp_err_t err = open_ftdi();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "FTDI open failed: %s; retry in 2 seconds",
                         esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            ESP_LOGI(TAG, "Connection order: ignition OFF -> plug CONSULT -> ignition ON");
            vTaskDelay(pdMS_TO_TICKS(1500));
        }

        if (s_ftdi_disconnected && s_ftdi != NULL) {
            s_ftdi_disconnected = false;
            s_waiting_init_ack = false;
            s_ecu_streaming = false;
            s_consult_session_active = false;
            s_parser_mode = PARSER_SEARCH;
            s_parser_reset_requested = true;
            s_good_frame_streak = 0;

            esp_err_t err = b14_ftdi_close(s_ftdi);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "cdc_acm_host_close: %s", esp_err_to_name(err));
            }

            s_ftdi = NULL;
            s_ftdi_connected = false;
            ESP_LOGI(TAG, "Ready for FTDI reconnect; active trip not reset");
            continue;
        }

        if (!s_consult_session_active && s_ftdi_connected) {
            if (!start_consult_stream()) {
                ESP_LOGW(TAG, "No CONSULT session yet; retry in 3 seconds");
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
        }

        if (s_consult_session_active) {
            const int64_t now_us = esp_timer_get_time();
            if (s_last_rx_us > 0) {
                const int64_t rx_age_ms = (now_us - s_last_rx_us) / 1000;
                if (rx_age_ms > SESSION_RX_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "CONSULT RX timeout (%lld ms); restarting session", (long long)rx_age_ms);
                    s_consult_session_active = false;
                    s_ecu_streaming = false;
                    s_parser_mode = PARSER_SEARCH;
                    s_parser_reset_requested = true;
                    s_good_frame_streak = 0;
                    continue;
                }
            }

            if (s_ecu_streaming && s_last_valid_frame_us > 0) {
                const int64_t valid_age_ms = (now_us - s_last_valid_frame_us) / 1000;
                if (valid_age_ms > STREAM_TIMEOUT_MS && s_last_rx_us > 0 &&
                    ((now_us - s_last_rx_us) / 1000) < SESSION_RX_TIMEOUT_MS) {
                    /* RX is still flowing. Keep session alive and let dual parser re-lock. */
                    s_ecu_streaming = false;
                    s_parser_mode = PARSER_SEARCH;
                    s_parser_reset_requested = true;
                    s_good_frame_streak = 0;
                    ESP_LOGW(TAG, "Valid sensor frame timeout but RX continues; parser re-sync only");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
