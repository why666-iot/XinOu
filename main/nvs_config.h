#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include "esp_err.h"
#include <string>

#define NVS_CONFIG_NAMESPACE "app_config"
#define NVS_KEY_SSID         "wifi_ssid"
#define NVS_KEY_PASSWORD     "wifi_pass"
#define NVS_KEY_WS_URI       "ws_uri"

/**
 * 从 NVS 加载配置，若 NVS 为空则写入编译期默认值
 */
esp_err_t nvs_config_load(
    const char* default_ssid,
    const char* default_password,
    const char* default_ws_uri,
    std::string& ssid,
    std::string& password,
    std::string& ws_uri
);

/**
 * 将 SSID 和 Password 写入 NVS（供未来 BLE 配网实际写入时使用）
 */
esp_err_t nvs_config_save_wifi(const std::string& ssid, const std::string& password);

/**
 * 清除 NVS 中保存的配置（恢复出厂）
 */
esp_err_t nvs_config_clear(void);

#endif // NVS_CONFIG_H
