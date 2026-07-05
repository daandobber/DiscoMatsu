#pragma once

// Minimal USB Mass Storage Bulk-Only Transport (BOT) driver for exactly one
// connected device, built directly on the ESP-IDF USB Host Library. Unlike
// the espressif/usb_host_msc component this does not mount a filesystem: it
// exposes a raw SCSI command-block primitive so callers can send arbitrary
// CDBs (used here to send CD-ROM/MMC commands like READ TOC and READ CD).
//
// Threading: usb_msc_scsi_start() spawns background tasks that own the USB
// Host Library and its client. usb_msc_scsi_command() must be called from a
// *different* task (e.g. the cdrom_audio poll/playback tasks) - it blocks
// waiting for a completion signal that is raised from within those
// background tasks' own event-handling loop.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_MSC_SCSI_DIR_NONE = 0,
    USB_MSC_SCSI_DIR_IN,
    USB_MSC_SCSI_DIR_OUT,
} usb_msc_scsi_dir_t;

// Called (from the internal USB client task) when a mass-storage bulk-only
// device has been opened and its interface claimed. Must not block and must
// not call usb_msc_scsi_command() itself - just record readiness and let
// another task act on it.
typedef void (*usb_msc_scsi_ready_cb_t)(void *arg);

// Called (from the internal USB client task) when the previously ready
// device has disconnected.
typedef void (*usb_msc_scsi_gone_cb_t)(void *arg);

// Starts the USB Host Library + a single bulk-only-transport client. Safe to
// call once at application startup.
esp_err_t usb_msc_scsi_start(usb_msc_scsi_ready_cb_t on_ready, usb_msc_scsi_gone_cb_t on_gone, void *cb_arg);

// True once a mass-storage bulk-only device is open and its interface
// claimed (i.e. after on_ready has fired and before on_gone).
bool usb_msc_scsi_is_ready(void);

// Sends a single SCSI/MMC command block (6/10/12/16 bytes) with an optional
// data-in or data-out phase, then reads the status wrapper.
//
// data/data_len describe the caller's buffer for the data phase (ignored if
// dir is USB_MSC_SCSI_DIR_NONE). Callers that want sense data after a
// failure can issue a follow-up REQUEST SENSE (CDB 0x03) command themselves.
//
// Returns ESP_OK only if the command was accepted by the device (CSW status
// "Command Passed"). ESP_ERR_INVALID_STATE if no device is ready,
// ESP_ERR_TIMEOUT on a transport timeout, ESP_FAIL for "Command Failed" or
// a phase/transport error.
esp_err_t usb_msc_scsi_command(
    const uint8_t *cdb, uint8_t cdb_len, usb_msc_scsi_dir_t dir, uint8_t *data, uint32_t data_len,
    uint32_t timeout_ms
);

#ifdef __cplusplus
}
#endif
