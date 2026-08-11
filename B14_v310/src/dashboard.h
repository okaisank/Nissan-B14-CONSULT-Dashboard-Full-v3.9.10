#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_PAGE_MAIN = 0,
    DASH_PAGE_ENGINE = 1,
    DASH_PAGE_TRIP = 2,
    DASH_PAGE_SETTINGS = 3,
    DASH_PAGE_COUNT = 4,
} dash_page_t;

typedef struct {
    bool display_ok;
    bool ftdi_connected;
    bool consult_session_active;
    bool sensor_valid;
    uint64_t rx_total;
    uint32_t accepted_frames;
    uint32_t rejected_frames;
    const char *parser_mode;

    float rpm;
    int speed_kmh;
    float injector_ms;
    float injector_duty;
    float maf_v;
    float maf_gps_est;
    bool maf_gps_valid;
    int ect_c;
    float o2_v;
    bool o2_rich;
    bool o2_lean;
    float battery_v;
    float tps_v;
    float tps_pct;
    int ignition_deg;
    float aac_pct;
    int af_alpha_pct;
    float fuel_corr_pct;
    bool ac_on;
    bool park_neutral;
    bool closed_throttle;
    float fuel_lph;
    float instant_km_l;

    bool trip_active;
    bool trip_showing_last;
    uint32_t trip_id;
    const char *trip_end_reason;
    double trip_distance_km;
    double trip_fuel_l;
    double trip_cost_baht;
    double trip_avg_km_l;
    double trip_engine_min;
    double trip_idle_min;
    double trip_ac_min;

    const char *daily_date;
    uint32_t daily_trips;
    double daily_distance_km;
    double daily_cost_baht;
    uint32_t lifetime_trips;
    double lifetime_distance_km;
    double lifetime_cost_baht;

    float fuel_price;
    float calibration;
    const char *current_date;
    bool ign_seen_on;
    bool ign_on;
    uint32_t visible_mask;

    int settings_index;
    bool settings_editing;
    float edit_value;
    bool just_saved;
} dashboard_view_t;

bool dashboard_init(void);
void dashboard_set_page(dash_page_t page);
void dashboard_render(const dashboard_view_t *v);
void dashboard_show_splash(const char *brand, uint32_t ms);
void dashboard_show_message(const char *title, const char *message, uint32_t ms);

#ifdef __cplusplus
}
#endif
