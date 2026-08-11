#include "ftdi_bridge.h"

#include <new>

#include "usb/vcp.hpp"
#include "usb/vcp_ftdi.hpp"

using namespace esp_usb;

namespace {
bool s_driver_registered = false;

static CdcAcmDevice *as_device(b14_ftdi_handle_t handle)
{
    return static_cast<CdcAcmDevice *>(handle);
}
}

extern "C" esp_err_t b14_ftdi_open(const cdc_acm_host_device_config_t *dev_config,
                                    b14_ftdi_handle_t *out_handle)
{
    if (dev_config == nullptr || out_handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = nullptr;

    try {
        if (!s_driver_registered) {
            /* Register only the FTDI FT23x family. */
            VCP::register_driver<FT23x>();
            s_driver_registered = true;
        }

        CdcAcmDevice *dev = VCP::open(dev_config, 0);
        if (dev == nullptr) {
            return ESP_ERR_NOT_FOUND;
        }
        *out_handle = static_cast<b14_ftdi_handle_t>(dev);
        return ESP_OK;
    } catch (const std::bad_alloc &) {
        return ESP_ERR_NO_MEM;
    } catch (...) {
        return ESP_FAIL;
    }
}

extern "C" esp_err_t b14_ftdi_line_coding_set(b14_ftdi_handle_t handle,
                                                cdc_acm_line_coding_t *line_coding)
{
    if (handle == nullptr || line_coding == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return as_device(handle)->line_coding_set(line_coding);
}

extern "C" esp_err_t b14_ftdi_set_control_line_state(b14_ftdi_handle_t handle,
                                                       bool dtr,
                                                       bool rts)
{
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return as_device(handle)->set_control_line_state(dtr, rts);
}

extern "C" esp_err_t b14_ftdi_tx_blocking(b14_ftdi_handle_t handle,
                                            const uint8_t *data,
                                            size_t length,
                                            uint32_t timeout_ms)
{
    if (handle == nullptr || data == nullptr || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return as_device(handle)->tx_blocking(const_cast<uint8_t *>(data), length, timeout_ms);
}

extern "C" esp_err_t b14_ftdi_close(b14_ftdi_handle_t handle)
{
    if (handle == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    CdcAcmDevice *dev = as_device(handle);
    const esp_err_t err = dev->close();
    delete dev;
    return err;
}
