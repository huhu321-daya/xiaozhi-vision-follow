cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/managed_components")

set(COMPONENTS
    main
    espressif__esp32-camera
    esp_wifi
    esp_netif
    esp_event
    esp_http_server
    esp_timer
    nvs_flash
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aas3cam_node)