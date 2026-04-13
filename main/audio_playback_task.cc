/**
 * @file audio_playback_task.cc
 * @brief 独立音频播放任务
 *
 * 从 FreeRTOS 队列中消费音频块并调用 bsp_play_audio_stream()，
 * 与 WebSocket 回调任务解耦，避免阻塞导致的卡顿和断线。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

void AudioManager::playbackTaskFunc(void* param) {
    AudioManager* self = static_cast<AudioManager*>(param);

    ESP_LOGI(TAG, "播放任务启动");

    uint8_t* block;

    while (true) {
        // 等待队列中的音频块（超时100ms）
        BaseType_t ret = xQueueReceive(self->audio_queue, &block, pdMS_TO_TICKS(100));

        if (ret == pdTRUE) {
            // 前 sizeof(size_t) 字节是数据长度
            size_t size;
            memcpy(&size, block, sizeof(size_t));
            uint8_t* data = block + sizeof(size_t);

            // 流式播放（阻塞直到 DMA 接受数据，但不停止 I2S）
            esp_err_t err = bsp_play_audio_stream(data, size);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "流式播放失败: %s", esp_err_to_name(err));
            }

            free(block);
        } else {
            // 超时：若已标记完成且队列为空则退出
            if (self->streaming_finished &&
                uxQueueMessagesWaiting(self->audio_queue) == 0) {
                ESP_LOGI(TAG, "队列已空且流式传输完成，退出播放任务");
                break;
            }
        }
    }

    // 停止音频输出（发送静音帧 + 禁用 I2S TX）
    bsp_audio_stop();

    ESP_LOGI(TAG, "播放任务结束");
    self->playback_task_running = false;
    self->playback_task_handle = nullptr;
    vTaskDelete(NULL);
}
