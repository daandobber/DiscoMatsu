#pragma once

// MMC (Multi-Media Command set) layer for USB CD/DVD-ROM drives, built on
// top of usb_msc_scsi.h. Handles drive/disc presence polling, reading the
// table of contents, and pulling raw CD-DA (digital audio) sectors.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CDROM_AUDIO_MAX_TRACKS        99
#define CDROM_AUDIO_SECTOR_BYTES      2352
#define CDROM_AUDIO_FRAME_BYTES       4  // 16-bit stereo
#define CDROM_AUDIO_FRAMES_PER_SECTOR (CDROM_AUDIO_SECTOR_BYTES / CDROM_AUDIO_FRAME_BYTES)
#define CDROM_AUDIO_SECTORS_PER_SEC   75

typedef struct {
    uint8_t number;
    bool is_audio;
    uint32_t start_lba;
    uint32_t end_lba;  // exclusive; next track's start_lba, or lead-out for the last track
} cdrom_track_t;

typedef struct {
    bool drive_present;
    bool disc_present;
    int track_count;
    cdrom_track_t tracks[CDROM_AUDIO_MAX_TRACKS];
} cdrom_status_t;

// Powers the USB-A host boost, starts the USB MSC/SCSI transport, and spawns
// a background task that polls for drive/disc presence and reads the TOC.
esp_err_t cdrom_audio_init(void);

// Read-only snapshot of drive/disc/track state for the UI.
void cdrom_audio_get_status(cdrom_status_t *out_status);

// Returns true (and clears the flag) if status has changed since the last
// call - used by the UI main loop to decide whether to redraw.
bool cdrom_audio_consume_dirty(void);

// Reads sector_count contiguous CD-DA sectors starting at lba into out_buf
// (must be at least sector_count * CDROM_AUDIO_SECTOR_BYTES bytes). Intended
// to be called from the playback task, not the polling task.
esp_err_t cdrom_audio_read_sectors(uint32_t lba, uint16_t sector_count, uint8_t *out_buf);

// Opens the tray (or spins down/ejects the disc, drive-dependent). Safe to
// call whether or not a drive/disc is currently present.
esp_err_t cdrom_audio_eject(void);

#ifdef __cplusplus
}
#endif
