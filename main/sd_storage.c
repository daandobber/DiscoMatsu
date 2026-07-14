#include "sd_storage.h"

#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_storage";

static sdmmc_card_t *s_card = NULL;

#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL)
static sd_pwr_ctrl_handle_t s_pwr = NULL;

static sd_pwr_ctrl_handle_t initialize_sd_ldo(void) {
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SD LDO power control driver");
        return NULL;
    }
    return pwr;
}

static esp_err_t reset_sd_card(void) {
    if (s_pwr == NULL) return ESP_ERR_INVALID_STATE;

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = BIT64(GPIO_NUM_39) | BIT64(GPIO_NUM_40) | BIT64(GPIO_NUM_41) | BIT64(GPIO_NUM_42) |
                        BIT64(GPIO_NUM_43) | BIT64(GPIO_NUM_44),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gpio_cfg);
    gpio_set_level(GPIO_NUM_39, 0);
    gpio_set_level(GPIO_NUM_40, 0);
    gpio_set_level(GPIO_NUM_41, 0);
    gpio_set_level(GPIO_NUM_42, 0);
    gpio_set_level(GPIO_NUM_43, 0);
    gpio_set_level(GPIO_NUM_44, 0);

    sd_pwr_ctrl_set_io_voltage(s_pwr, 0);
    vTaskDelay(pdMS_TO_TICKS(150));

    gpio_cfg.mode = GPIO_MODE_INPUT;
    gpio_config(&gpio_cfg);
    sd_pwr_ctrl_set_io_voltage(s_pwr, 3300);
    vTaskDelay(pdMS_TO_TICKS(150));
    return ESP_OK;
}
#endif

esp_err_t sd_storage_mount(void) {
    if (s_card != NULL) return ESP_OK;

#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL)
    if (s_pwr == NULL) s_pwr = initialize_sd_ldo();
    if (s_pwr == NULL) return ESP_FAIL;

    esp_err_t res = reset_sd_card();
    if (res != ESP_OK) return res;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;

    static DRAM_DMA_ALIGNED_ATTR uint8_t dma_buf[512 * 4];
    host.dma_aligned_buffer = dma_buf;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = GPIO_NUM_43;
    slot_config.cmd = GPIO_NUM_44;
    slot_config.d0 = GPIO_NUM_39;
    slot_config.d1 = GPIO_NUM_40;
    slot_config.d2 = GPIO_NUM_41;
    slot_config.d3 = GPIO_NUM_42;
    slot_config.width = 4;

    res = esp_vfs_fat_sdmmc_mount(SD_STORAGE_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(res));
        s_card = NULL;
        return res;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "SD mount is only wired for Tanmatsu/Konsool targets");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool sd_storage_is_mounted(void) {
    return s_card != NULL;
}
