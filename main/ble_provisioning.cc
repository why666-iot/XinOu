#include "ble_provisioning.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "BLE_Prov";

// GATT Characteristic 值缓冲区
static char ssid_buf[33] = {0};
static char password_buf[65] = {0};
static bool s_is_running = false;
static uint8_t own_addr_type;

// UUID 定义（静态变量，避免 C++ 取临时对象地址的编译错误）
static ble_uuid16_t prov_svc_uuid  = BLE_UUID16_INIT(0xFF01);
static ble_uuid16_t prov_chr_ssid_uuid = BLE_UUID16_INIT(0xFF02);
static ble_uuid16_t prov_chr_pass_uuid = BLE_UUID16_INIT(0xFF03);

// 前向声明
static void ble_advertise(void);
static int ssid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int pass_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

// GATT 服务定义
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &prov_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // SSID: 可读可写
                .uuid = &prov_chr_ssid_uuid.u,
                .access_cb = ssid_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                // Password: 仅可写（安全考虑，不可回读）
                .uuid = &prov_chr_pass_uuid.u,
                .access_cb = pass_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0}, // 终止符
        },
    },
    {0}, // 终止符
};

// SSID 读写回调
static int ssid_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        os_mbuf_append(ctxt->om, ssid_buf, strlen(ssid_buf));
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(ssid_buf)) len = sizeof(ssid_buf) - 1;
        memset(ssid_buf, 0, sizeof(ssid_buf));
        ble_hs_mbuf_to_flat(ctxt->om, ssid_buf, len, NULL);
        ESP_LOGI(TAG, "📶 BLE 收到 SSID: %s", ssid_buf);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// Password 写回调
static int pass_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(password_buf)) len = sizeof(password_buf) - 1;
        memset(password_buf, 0, sizeof(password_buf));
        ble_hs_mbuf_to_flat(ctxt->om, password_buf, len, NULL);
        ESP_LOGI(TAG, "🔑 BLE 收到 Password, 长度: %d", (int)len);
        // TODO: 产品化后取消注释，调用 nvs_config_save_wifi(ssid_buf, password_buf)
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// GAP 事件回调（处理连接/断开）
static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "📱 BLE 客户端已连接, conn_handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE 连接失败, status=%d", event->connect.status);
            ble_advertise();  // 连接失败，重新广播
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "📱 BLE 客户端已断开, reason=%d", event->disconnect.reason);
        ble_advertise();  // 断开后重新广播，允许再次连接
        break;
    default:
        break;
    }
    return 0;
}

// 开始 BLE 广播
static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 160;  // 100ms (160 * 0.625ms)
    adv_params.itvl_max = 320;  // 200ms

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "📡 BLE 广播已启动，设备名: %s", name);
}

// BLE 栈同步回调（栈初始化完成后调用）
static void on_ble_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "确保地址失败: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "推断地址类型失败: %d", rc);
        return;
    }
    ble_advertise();
}

// BLE 栈重置回调
static void on_ble_reset(int reason)
{
    ESP_LOGW(TAG, "BLE 栈重置, 原因: %d", reason);
}

// NimBLE host 任务
static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_provisioning_start(void)
{
    if (s_is_running) {
        ESP_LOGW(TAG, "BLE 配网服务已在运行");
        return ESP_OK;
    }

    // 构造设备名: "XinOu-XXXX"
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    char device_name[16];
    snprintf(device_name, sizeof(device_name), "XinOu-%02X%02X", mac[4], mac[5]);

    // 初始化 NimBLE
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 配置 host 回调
    ble_hs_cfg.reset_cb = on_ble_reset;
    ble_hs_cfg.sync_cb = on_ble_sync;

    // 初始化 GAP 和 GATT 服务
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 注册自定义 GATT 服务
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务计数失败: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务注册失败: %d", rc);
        return ESP_FAIL;
    }

    // 设置设备名
    ble_svc_gap_device_name_set(device_name);

    // 启动 NimBLE host 任务
    nimble_port_freertos_init(nimble_host_task);

    s_is_running = true;
    ESP_LOGI(TAG, "✅ BLE 配网服务已启动，设备名: %s", device_name);
    return ESP_OK;
}

esp_err_t ble_provisioning_stop(void)
{
    if (!s_is_running) return ESP_OK;

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "停止 NimBLE 失败: %d", rc);
        return ESP_FAIL;
    }
    nimble_port_deinit();
    s_is_running = false;
    ESP_LOGI(TAG, "BLE 配网服务已停止");
    return ESP_OK;
}

bool ble_provisioning_is_running(void)
{
    return s_is_running;
}
