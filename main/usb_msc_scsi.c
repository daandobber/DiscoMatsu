#include "usb_msc_scsi.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_msc_scsi";

#define USB_CLASS_MASS_STORAGE   0x08
#define USB_MSC_PROTOCOL_BULK_ONLY 0x50

#define MSC_CBW_SIZE      31
#define MSC_CSW_SIZE      13
#define MSC_CBW_SIGNATURE 0x43425355u
#define MSC_CSW_SIGNATURE 0x53425355u

#define USB_MSC_REQ_BULK_ONLY_RESET 0xFF
#define USB_FEATURE_ENDPOINT_HALT   0x00

static usb_host_client_handle_t s_client_hdl = NULL;
static usb_device_handle_t s_dev_hdl         = NULL;
static uint8_t s_ep_in                       = 0;
static uint8_t s_ep_out                      = 0;
static uint16_t s_ep_in_mps                  = 64;
static uint8_t s_intf_num                    = 0;
static volatile bool s_ready                 = false;
static uint32_t s_next_tag                   = 1;

static SemaphoreHandle_t s_cmd_mutex   = NULL;
static SemaphoreHandle_t s_phase_sem   = NULL;

static usb_msc_scsi_ready_cb_t s_on_ready = NULL;
static usb_msc_scsi_gone_cb_t s_on_gone   = NULL;
static void *s_cb_arg                     = NULL;

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void transfer_done_cb(usb_transfer_t *transfer) {
    SemaphoreHandle_t sem = (SemaphoreHandle_t)transfer->context;
    xSemaphoreGive(sem);
}

// Sends a standard/class control request with no data stage, via EP0, and
// blocks (from the caller's task) for its completion.
static esp_err_t submit_control_no_data(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex) {
    usb_transfer_t *xfer = NULL;
    esp_err_t ret         = usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &xfer);
    if (ret != ESP_OK) return ret;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType       = bmRequestType;
    setup->bRequest            = bRequest;
    setup->wValue              = wValue;
    setup->wIndex              = wIndex;
    setup->wLength             = 0;

    xfer->device_handle    = s_dev_hdl;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t);
    xfer->callback         = transfer_done_cb;
    xfer->context          = s_phase_sem;
    xfer->timeout_ms       = 2000;

    xSemaphoreTake(s_phase_sem, 0);
    ret = usb_host_transfer_submit_control(s_client_hdl, xfer);
    if (ret == ESP_OK) {
        if (xSemaphoreTake(s_phase_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ret = ESP_ERR_TIMEOUT;
        } else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
            ret = ESP_FAIL;
        }
    }
    usb_host_transfer_free(xfer);
    return ret;
}

// Bulk-Only Mass Storage Reset Recovery (USB MSC BOT spec): a STALL or
// phase error on either bulk endpoint means the *device* considers that
// endpoint halted, and only it can clear that via a real CLEAR_FEATURE
// control request - resetting just the host-side pipe state
// (usb_host_endpoint_halt/clear) is not enough and leaves the device stuck,
// wedging every subsequent command. This performs the full spec-mandated
// recovery: class-specific Bulk-Only Mass Storage Reset, then
// CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints, then resets the
// host-side pipe state for both as well.
static void reset_recovery(void) {
    if (s_dev_hdl == NULL) return;
    ESP_LOGW(TAG, "Performing Bulk-Only Mass Storage Reset Recovery");

    submit_control_no_data(
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        USB_MSC_REQ_BULK_ONLY_RESET, 0, s_intf_num
    );
    submit_control_no_data(
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT,
        USB_B_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, s_ep_in
    );
    submit_control_no_data(
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_ENDPOINT,
        USB_B_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, s_ep_out
    );

    usb_host_endpoint_halt(s_dev_hdl, s_ep_in);
    usb_host_endpoint_clear(s_dev_hdl, s_ep_in);
    usb_host_endpoint_halt(s_dev_hdl, s_ep_out);
    usb_host_endpoint_clear(s_dev_hdl, s_ep_out);
}

static const char *transfer_status_name(usb_transfer_status_t status) {
    switch (status) {
        case USB_TRANSFER_STATUS_COMPLETED: return "completed";
        case USB_TRANSFER_STATUS_ERROR: return "error";
        case USB_TRANSFER_STATUS_TIMED_OUT: return "timed_out";
        case USB_TRANSFER_STATUS_CANCELED: return "canceled";
        case USB_TRANSFER_STATUS_STALL: return "stall";
        case USB_TRANSFER_STATUS_OVERFLOW: return "overflow";
        case USB_TRANSFER_STATUS_SKIPPED: return "skipped";
        case USB_TRANSFER_STATUS_NO_DEVICE: return "no_device";
        default: return "?";
    }
}

// Submits transfer and blocks (from the *caller's* task) until it completes
// or times out. Must never be called from the USB client task itself.
static esp_err_t submit_and_wait(usb_transfer_t *transfer, uint32_t timeout_ms, const char *phase_name) {
    xSemaphoreTake(s_phase_sem, 0);
    transfer->callback = transfer_done_cb;
    transfer->context  = s_phase_sem;
    transfer->timeout_ms = timeout_ms;

    esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: submit failed: %s", phase_name, esp_err_to_name(err));
        return err;
    }
    if (xSemaphoreTake(s_phase_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: no completion within %u ms, cancelling", phase_name, (unsigned)timeout_ms);
        // The transfer is still in-flight as far as the Host Library is
        // concerned - usb_host_transfer_free() must never be called on an
        // in-flight transfer. Halting + flushing the endpoint forces it to
        // complete (our callback still fires, giving the semaphore) before
        // the caller is allowed to free it.
        usb_host_endpoint_halt(s_dev_hdl, transfer->bEndpointAddress);
        usb_host_endpoint_flush(s_dev_hdl, transfer->bEndpointAddress);
        xSemaphoreTake(s_phase_sem, pdMS_TO_TICKS(1000));
        usb_host_endpoint_clear(s_dev_hdl, transfer->bEndpointAddress);
        return ESP_ERR_TIMEOUT;
    }
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(
            TAG, "%s: transfer status=%s actual_bytes=%d/%d", phase_name, transfer_status_name(transfer->status),
            transfer->actual_num_bytes, transfer->num_bytes
        );
        if (transfer->status == USB_TRANSFER_STATUS_STALL) {
            reset_recovery();
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t usb_msc_scsi_command(
    const uint8_t *cdb, uint8_t cdb_len, usb_msc_scsi_dir_t dir, uint8_t *data, uint32_t data_len,
    uint32_t timeout_ms
) {
    if (cdb == NULL || cdb_len == 0 || cdb_len > 16) return ESP_ERR_INVALID_ARG;
    if (dir != USB_MSC_SCSI_DIR_NONE && (data == NULL || data_len == 0)) return ESP_ERR_INVALID_ARG;
    if (s_cmd_mutex == NULL) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t ret            = ESP_FAIL;
    usb_transfer_t *cbw_xfer = NULL;
    usb_transfer_t *data_xfer = NULL;
    usb_transfer_t *csw_xfer  = NULL;

    if (!s_ready) {
        ret = ESP_ERR_INVALID_STATE;
        goto unlock;
    }

    uint32_t tag = s_next_tag++;

    // Bulk IN transfers must request an integer multiple of the endpoint's
    // max packet size; the device still ends the phase early with a short
    // packet once it has sent the real (possibly smaller) amount of data.
    uint32_t csw_alloc_len = usb_round_up_to_mps(MSC_CSW_SIZE, s_ep_in_mps);
    uint32_t data_alloc_len =
        (dir == USB_MSC_SCSI_DIR_IN) ? usb_round_up_to_mps(data_len, s_ep_in_mps) : data_len;

    ret = usb_host_transfer_alloc(MSC_CBW_SIZE, 0, &cbw_xfer);
    if (ret != ESP_OK) goto cleanup;
    ret = usb_host_transfer_alloc(csw_alloc_len, 0, &csw_xfer);
    if (ret != ESP_OK) goto cleanup;
    if (dir != USB_MSC_SCSI_DIR_NONE) {
        ret = usb_host_transfer_alloc(data_alloc_len, 0, &data_xfer);
        if (ret != ESP_OK) goto cleanup;
    }

    uint8_t *cbw = cbw_xfer->data_buffer;
    memset(cbw, 0, MSC_CBW_SIZE);
    put_le32(cbw + 0, MSC_CBW_SIGNATURE);
    put_le32(cbw + 4, tag);
    put_le32(cbw + 8, dir == USB_MSC_SCSI_DIR_NONE ? 0 : data_len);
    cbw[12] = (dir == USB_MSC_SCSI_DIR_IN) ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cdb_len;
    memcpy(cbw + 15, cdb, cdb_len);

    cbw_xfer->device_handle    = s_dev_hdl;
    cbw_xfer->bEndpointAddress = s_ep_out;
    cbw_xfer->num_bytes        = MSC_CBW_SIZE;
    ret = submit_and_wait(cbw_xfer, timeout_ms, "cbw");
    if (ret != ESP_OK || !s_ready) goto cleanup;

    if (dir == USB_MSC_SCSI_DIR_OUT) {
        memcpy(data_xfer->data_buffer, data, data_len);
    }
    if (dir != USB_MSC_SCSI_DIR_NONE) {
        data_xfer->device_handle    = s_dev_hdl;
        data_xfer->bEndpointAddress = (dir == USB_MSC_SCSI_DIR_IN) ? s_ep_in : s_ep_out;
        data_xfer->num_bytes        = data_alloc_len;
        ret = submit_and_wait(data_xfer, timeout_ms, "data");
        if (ret != ESP_OK || !s_ready) goto cleanup;
        if (dir == USB_MSC_SCSI_DIR_IN) {
            uint32_t copy_len = data_xfer->actual_num_bytes;
            if (copy_len > data_len) copy_len = data_len;
            memcpy(data, data_xfer->data_buffer, copy_len);
        }
    }

    csw_xfer->device_handle    = s_dev_hdl;
    csw_xfer->bEndpointAddress = s_ep_in;
    csw_xfer->num_bytes        = csw_alloc_len;
    ret = submit_and_wait(csw_xfer, timeout_ms, "csw");
    if (ret != ESP_OK || !s_ready) goto cleanup;

    {
        const uint8_t *csw    = csw_xfer->data_buffer;
        uint32_t signature    = get_le32(csw + 0);
        uint32_t returned_tag = get_le32(csw + 4);
        uint8_t status        = csw[12];
        if (signature != MSC_CSW_SIGNATURE || returned_tag != tag) {
            ESP_LOGW(TAG, "CSW signature/tag mismatch");
            ret = ESP_FAIL;
        } else if (status != 0) {
            ret = ESP_FAIL;
        } else {
            ret = ESP_OK;
        }
    }

cleanup:
    if (cbw_xfer) usb_host_transfer_free(cbw_xfer);
    if (data_xfer) usb_host_transfer_free(data_xfer);
    if (csw_xfer) usb_host_transfer_free(csw_xfer);
unlock:
    xSemaphoreGive(s_cmd_mutex);
    return ret;
}

bool usb_msc_scsi_is_ready(void) {
    return s_ready;
}

// Looks for a bulk-only mass-storage interface in the device's active
// configuration. Returns ESP_OK and fills *out_* on success.
static esp_err_t find_msc_interface(
    const usb_config_desc_t *config_desc, uint8_t *out_intf_num, uint8_t *out_ep_in, uint8_t *out_ep_out,
    uint16_t *out_ep_in_mps
) {
    for (uint8_t intf_num = 0; intf_num < config_desc->bNumInterfaces; intf_num++) {
        int offset = 0;
        const usb_intf_desc_t *intf =
            usb_parse_interface_descriptor(config_desc, intf_num, 0, &offset);
        if (intf == NULL) continue;
        if (intf->bInterfaceClass != USB_CLASS_MASS_STORAGE) continue;
        if (intf->bInterfaceProtocol != USB_MSC_PROTOCOL_BULK_ONLY) continue;

        uint8_t ep_in = 0, ep_out = 0;
        uint16_t ep_in_mps = 0;
        for (int i = 0; i < intf->bNumEndpoints; i++) {
            int ep_offset               = offset;
            const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(
                intf, i, config_desc->wTotalLength, &ep_offset
            );
            if (ep_desc == NULL) continue;
            if (USB_EP_DESC_GET_XFERTYPE(ep_desc) != USB_TRANSFER_TYPE_BULK) continue;
            if (USB_EP_DESC_GET_EP_DIR(ep_desc)) {
                ep_in     = ep_desc->bEndpointAddress;
                ep_in_mps = USB_EP_DESC_GET_MPS(ep_desc);
            } else {
                ep_out = ep_desc->bEndpointAddress;
            }
        }
        if (ep_in != 0 && ep_out != 0) {
            *out_intf_num  = intf->bInterfaceNumber;
            *out_ep_in     = ep_in;
            *out_ep_out    = ep_out;
            *out_ep_in_mps = ep_in_mps;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

static void handle_new_device(uint8_t address) {
    if (s_dev_hdl != NULL) {
        ESP_LOGW(TAG, "Ignoring extra USB device (only one mass-storage device is supported)");
        return;
    }

    usb_device_handle_t dev_hdl = NULL;
    esp_err_t ret                = usb_host_device_open(s_client_hdl, address, &dev_hdl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device_open failed: %s", esp_err_to_name(ret));
        return;
    }

    const usb_config_desc_t *config_desc = NULL;
    ret = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "get_active_config_descriptor failed: %s", esp_err_to_name(ret));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    usb_device_info_t dev_info = {0};
    if (usb_host_device_info(dev_hdl, &dev_info) == ESP_OK) {
        static const char *speed_names[] = {"low", "full", "high"};
        const char *speed_name           = (dev_info.speed <= USB_SPEED_HIGH) ? speed_names[dev_info.speed] : "?";
        ESP_LOGI(TAG, "Device at address %u negotiated USB speed: %s", address, speed_name);
    }

    uint8_t intf_num = 0, ep_in = 0, ep_out = 0;
    uint16_t ep_in_mps = 0;
    if (find_msc_interface(config_desc, &intf_num, &ep_in, &ep_out, &ep_in_mps) != ESP_OK) {
        ESP_LOGI(TAG, "Device at address %u has no bulk-only mass-storage interface, ignoring", address);
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }
    if (ep_in_mps == 0) {
        ESP_LOGW(TAG, "Bulk IN endpoint reported MPS of 0, ignoring device");
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    ret = usb_host_interface_claim(s_client_hdl, dev_hdl, intf_num, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "interface_claim failed: %s", esp_err_to_name(ret));
        usb_host_device_close(s_client_hdl, dev_hdl);
        return;
    }

    s_dev_hdl   = dev_hdl;
    s_intf_num  = intf_num;
    s_ep_in     = ep_in;
    s_ep_out    = ep_out;
    s_ep_in_mps = ep_in_mps;
    s_ready     = true;

    ESP_LOGI(
        TAG, "Mass-storage device ready (intf %u, ep_in 0x%02x mps %u, ep_out 0x%02x)", intf_num, ep_in, ep_in_mps,
        ep_out
    );
    if (s_on_ready) s_on_ready(s_cb_arg);
}

static void handle_device_gone(usb_device_handle_t dev_hdl) {
    if (dev_hdl != s_dev_hdl) return;

    s_ready = false;
    if (s_on_gone) s_on_gone(s_cb_arg);

    usb_host_interface_release(s_client_hdl, s_dev_hdl, s_intf_num);
    usb_host_device_close(s_client_hdl, s_dev_hdl);
    s_dev_hdl = NULL;
    s_ep_in = s_ep_out = s_intf_num = 0;
    ESP_LOGI(TAG, "Mass-storage device disconnected");
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg) {
    switch (event_msg->event) {
        case USB_HOST_CLIENT_EVENT_NEW_DEV: handle_new_device(event_msg->new_dev.address); break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE: handle_device_gone(event_msg->dev_gone.dev_hdl); break;
        default: break;
    }
}

// Both loops below normally block in their respective *_handle_events()
// call (portMAX_DELAY) and only spin again once real work is done. If the
// USB Host Library ever gets into a state where that call returns quickly
// with an error instead of blocking, these would busy-loop with no yield
// and starve the watchdog - the vTaskDelay on an unexpected error is a
// cheap, always-safe guard against that regardless of the root cause.
static void usbh_lib_task(void *arg) {
    while (1) {
        uint32_t event_flags = 0;
        esp_err_t ret         = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void usbh_client_task(void *arg) {
    while (1) {
        esp_err_t ret = usb_host_client_handle_events(s_client_hdl, portMAX_DELAY);
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "usb_host_client_handle_events: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t usb_msc_scsi_start(usb_msc_scsi_ready_cb_t on_ready, usb_msc_scsi_gone_cb_t on_gone, void *cb_arg) {
    s_on_ready = on_ready;
    s_on_gone  = on_gone;
    s_cb_arg   = cb_arg;

    s_cmd_mutex = xSemaphoreCreateMutex();
    s_phase_sem = xSemaphoreCreateBinary();
    if (s_cmd_mutex == NULL || s_phase_sem == NULL) return ESP_ERR_NO_MEM;

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async =
            {
                .client_event_callback = client_event_cb,
                .callback_arg          = NULL,
            },
    };
    ret = usb_host_client_register(&client_config, &s_client_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Priority 20: ESP-Hosted's own SDIO/RPC tasks run at priority 23 (see
    // "Hosted_Tasks: prio:23" in the boot log) - if these USB host tasks
    // sit at a much lower priority, ESP-Hosted's traffic can starve them of
    // CPU time, causing USB transfer completions to time out even though
    // the USB connection itself stays up. Keeping these close to (but still
    // below) ESP-Hosted's priority avoids that without starving ESP-Hosted
    // in turn.
    xTaskCreate(usbh_lib_task, "usbh_lib", 4096, NULL, 20, NULL);
    xTaskCreate(usbh_client_task, "usbh_client", 4096, NULL, 20, NULL);
    return ESP_OK;
}
