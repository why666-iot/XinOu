#ifndef BLE_PROVISIONING_H
#define BLE_PROVISIONING_H

#include "esp_err.h"

/**
 * 启动 BLE 配网服务
 * 初始化 NimBLE 栈，注册 GATT Service，开始广播
 * 设备名称: "XinOu-XXXX"（XXXX 为 MAC 后 4 位十六进制）
 */
esp_err_t ble_provisioning_start(void);

/**
 * 停止 BLE 配网服务
 */
esp_err_t ble_provisioning_stop(void);

/**
 * 查询 BLE 配网服务是否正在运行
 */
bool ble_provisioning_is_running(void);

#endif // BLE_PROVISIONING_H
