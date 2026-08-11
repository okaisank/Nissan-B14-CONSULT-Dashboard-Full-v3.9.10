#include "rotary_encoder.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#define ENC_CLK 5
#define ENC_DT  6
#define ENC_SW  7

bool rotary_encoder_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ENC_CLK) | (1ULL << ENC_DT) | (1ULL << ENC_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg) == ESP_OK;
}

rotary_event_t rotary_encoder_poll(void)
{
    static const int8_t table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };
    static uint8_t prev_ab = 0x03;
    static int accum = 0;
    static int last_sw = 1;
    static int stable_sw = 1;
    static int64_t sw_changed_us = 0;
    static int64_t press_start_us = 0;
    static bool long_sent = false;
    static int64_t last_turn_event_us = 0;

    int a = gpio_get_level(ENC_CLK) ? 1 : 0;
    int b = gpio_get_level(ENC_DT) ? 1 : 0;
    uint8_t ab = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)((prev_ab << 2) | ab);
    prev_ab = ab;
    accum += table[idx & 0x0F];

    int64_t now = esp_timer_get_time();
    if (accum >= 4) {
        accum = 0;
        if ((now - last_turn_event_us) >= 50000) {
            last_turn_event_us = now;
            return ROTARY_EVENT_CW;
        }
    }
    if (accum <= -4) {
        accum = 0;
        if ((now - last_turn_event_us) >= 50000) {
            last_turn_event_us = now;
            return ROTARY_EVENT_CCW;
        }
    }

    int sw = gpio_get_level(ENC_SW) ? 1 : 0;
    if (sw != last_sw) {
        last_sw = sw;
        sw_changed_us = now;
    }
    if (sw != stable_sw && (now - sw_changed_us) > 40000) {
        stable_sw = sw;
        if (stable_sw == 0) {
            press_start_us = now;
            long_sent = false;
        } else if (press_start_us != 0) {
            int64_t held = now - press_start_us;
            press_start_us = 0;
            if (!long_sent && held >= 60000 && held < 1000000) {
                return ROTARY_EVENT_PRESS;
            }
        }
    }

    if (stable_sw == 0 && press_start_us != 0 && !long_sent &&
        (now - press_start_us) >= 1000000) {
        long_sent = true;
        return ROTARY_EVENT_LONG_PRESS;
    }

    return ROTARY_EVENT_NONE;
}
