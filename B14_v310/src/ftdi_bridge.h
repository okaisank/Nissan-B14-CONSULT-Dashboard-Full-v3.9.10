#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/cdc_acm_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *b14_ftdi_handle_t;

esp_err_t b14_ftdi_open(const cdc_acm_host_device_config_t *dev_config,
                        b14_ftdi_handle_t *out_handle);
esp_err_t b14_ftdi_line_coding_set(b14_ftdi_handle_t handle,
                                   cdc_acm_line_coding_t *line_coding);
esp_err_t b14_ftdi_set_control_line_state(b14_ftdi_handle_t handle,
                                          bool dtr,
                                          bool rts);
esp_err_t b14_ftdi_tx_blocking(b14_ftdi_handle_t handle,
                               const uint8_t *data,
                               size_t length,
                               uint32_t timeout_ms);
esp_err_t b14_ftdi_close(b14_ftdi_handle_t handle);

#ifdef __cplusplus
}
#endif
