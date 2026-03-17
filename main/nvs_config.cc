#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "NVS_Config";

// 从 NVS 读取字符串，若不存在则写入默认值
static esp_err_t nvs_load_or_default(nvs_handle_t handle, const char* key,
                                      const char* default_val, std::string& out)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // 首次启动，写入默认值
        err = nvs_set_str(handle, key, default_val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入默认值失败 [%s]: %s", key, esp_err_to_name(err));
            return err;
        }
        out = default_val;
        ESP_LOGI(TAG, "  %s: 首次写入默认值", key);
        return ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取失败 [%s]: %s", key, esp_err_to_name(err));
        return err;
    }

    // 读取已有值
    char* buf = new char[required_size];
    err = nvs_get_str(handle, key, buf, &required_size);
    if (err == ESP_OK) {
        out = buf;
    }
    delete[] buf;
    return err;
}

esp_err_t nvs_config_load(
    const char* default_ssid,
    const char* default_password,
    const char* default_ws_uri,
    std::string& ssid,
    std::string& password,
    std::string& ws_uri)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "从 NVS 加载配置...");

    err = nvs_load_or_default(handle, NVS_KEY_SSID, default_ssid, ssid);
    if (err != ESP_OK) goto cleanup;

    err = nvs_load_or_default(handle, NVS_KEY_PASSWORD, default_password, password);
    if (err != ESP_OK) goto cleanup;

    err = nvs_load_or_default(handle, NVS_KEY_WS_URI, default_ws_uri, ws_uri);
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit 失败: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "NVS 配置加载完成: SSID=%s, Password长度=%d, WS_URI=%s",
             ssid.c_str(), (int)password.length(), ws_uri.c_str());

cleanup:
    nvs_close(handle);
    return err;
}

esp_err_t nvs_config_save_wifi(const std::string& ssid, const std::string& password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_SSID, ssid.c_str());
    if (err != ESP_OK) goto cleanup;

    err = nvs_set_str(handle, NVS_KEY_PASSWORD, password.c_str());
    if (err != ESP_OK) goto cleanup;

    err = nvs_commit(handle);
    ESP_LOGI(TAG, "WiFi 凭据已保存到 NVS: SSID=%s", ssid.c_str());

cleanup:
    nvs_close(handle);
    return err;
}

esp_err_t nvs_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI(TAG, "NVS 配置已清除");
    }

    nvs_close(handle);
    return err;
}
