idf_component_register(
    SRCS "main.c" "cam_camera.c" "cam_detect.cpp" "cam_http.c" "cam_net.c" "cam_vlm.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_http_client esp_timer esp32-camera esp_jpeg nvs_flash pedestrian_detect json mbedtls
)