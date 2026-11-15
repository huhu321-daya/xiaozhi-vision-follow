#include "cam_config.h"
#include "cam_camera.h"
#include "cam_http.h"
#include "cam_net.h"
#include "cam_vlm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#define TAG "main"

void app_main(void)
{
    ESP_LOGI(TAG, "AAS3-CAM node boot");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    cam_camera_init();

    cam_net_wifi_start();
    cam_http_start();
    cam_net_udp_start();
    cam_vlm_start();

    ESP_LOGI(TAG, "node ready (brain+nervelet follow decision here)");
}