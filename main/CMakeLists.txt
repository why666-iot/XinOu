set(requires
    driver
    esp_driver_i2s
    esp_timer
    nvs_flash
    esp_wifi
    esp_netif
    esp_websocket_client
    mbedtls
    bt
    )

idf_component_register(SRCS
                       main.cc
                       bsp_board.cc
                       audio_manager.cc
                       wifi_manager.cc
                       websocket_client.cc
                       nvs_config.cc
                       ble_provisioning.cc
                       INCLUDE_DIRS
                       "."
                       REQUIRES ${requires}
                       )

