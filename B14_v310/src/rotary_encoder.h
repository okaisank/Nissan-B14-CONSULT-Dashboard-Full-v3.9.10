#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROTARY_EVENT_NONE = 0,
    ROTARY_EVENT_CW,
    ROTARY_EVENT_CCW,
    ROTARY_EVENT_PRESS,
    ROTARY_EVENT_LONG_PRESS,
} rotary_event_t;

bool rotary_encoder_init(void);
rotary_event_t rotary_encoder_poll(void);

#ifdef __cplusplus
}
#endif
