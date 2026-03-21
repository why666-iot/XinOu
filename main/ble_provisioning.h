#ifndef BLE_PROVISIONING_H
#define BLE_PROVISIONING_H

#include "esp_err.h"

// BLE 配网成功回调：收到 SSID + Password 并写入 NVS 后调用
typedef void (*ble_prov_wifi_cb_t)(const char* ssid, const char* password);

/**
 * 启动 BLE 配网服务
 * @param wifi_cb 收到新 WiFi 凭据后的回调（可为 NULL）
 */
esp_err_t ble_provisioning_start(ble_prov_wifi_cb_t wifi_cb);

/**
 * 停止 BLE 配网服务
 */
esp_err_t ble_provisioning_stop(void);

/**
 * 查询 BLE 配网服务是否正在运行
 */
bool ble_provisioning_is_running(void);

#endif // BLE_PROVISIONING_H
