/*
 * Copyright (c) 2023-2023, lihongquan
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2023-9-8      lihongquan   add license declaration
 */

#include "tinyusb.h"
#include "tinyusb_msc.h"
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "disk.h"
#include "esp_check.h"

static const char *TAG = "msc_disk";

// mount the partition and show all the files in path
bool msc_dick_mount(const char *path)
{
    const tinyusb_msc_driver_config_t driver_config = {};
    tinyusb_msc_storage_config_t storage_config = {
        .fat_fs.base_path = (char *)path,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };

    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&driver_config));

#ifdef CONFIG_MSC_STORAGE_MEDIA_SPIFLASH
    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    storage_config.medium.wl_handle = wl_handle;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&storage_config, NULL));
#else
    static sdmmc_card_t *card = NULL;

    if (storage_init_sdmmc(&card) != ESP_OK)
        return false;

    storage_config.medium.card = card;
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&storage_config, NULL));
#endif
    ESP_LOGI(TAG, "Mount storage...");

    return true;
}