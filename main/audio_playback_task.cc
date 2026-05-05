/**
 * @file audio_playback_task.cc
 * @brief 独立音频播放任务
 *
 * 从预分配的环形缓冲区中消费音频数据并调用 bsp_play_audio_stream()，
 * 与 WebSocket 回调任务解耦，避免阻塞导致的卡顿和断线。
 *
 * 工作流程：
 *   1. 每轮从环形缓冲区取最多 PLAYBACK_CHUNK 字节到栈上临时缓冲区（持锁）
 *   2. 释放锁后调用 bsp_play_audio_stream()（阻塞等待 I2S DMA 接受数据）
 *   3. 若缓冲区为空且 streaming_finished 已设置，则退出
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

void AudioManager::playbackTaskFunc(void* param) {
    AudioManager* self = static_cast<AudioManager*>(param);

    ESP_LOGI(TAG, "播放任务启动（环形缓冲区模式）");

    // 栈上临时缓冲区，用于在锁外调用 bsp_play_audio_stream()
    static uint8_t local_buf[PLAYBACK_CHUNK];

    while (true) {
        size_t to_play = 0;

        // ── 从环形缓冲区取一块数据（持锁，只做内存操作，速度极快）──
        if (self->ring_mutex) {
            xSemaphoreTake(self->ring_mutex, portMAX_DELAY);

            to_play = self->ring_data_len;
            if (to_play > PLAYBACK_CHUNK) to_play = PLAYBACK_CHUNK;

            if (to_play > 0) {
                // 读取数据（可能跨越缓冲区末尾，分两段拷贝）
                size_t bytes_to_end = self->ring_buf_total - self->ring_read_pos;
                if (to_play <= bytes_to_end) {
                    memcpy(local_buf, self->ring_buf + self->ring_read_pos, to_play);
                } else {
                    memcpy(local_buf, self->ring_buf + self->ring_read_pos, bytes_to_end);
                    memcpy(local_buf + bytes_to_end, self->ring_buf, to_play - bytes_to_end);
                }
                self->ring_read_pos  = (self->ring_read_pos + to_play) % self->ring_buf_total;
                self->ring_data_len -= to_play;
            }

            xSemaphoreGive(self->ring_mutex);
        }

        if (to_play > 0) {
            // ── 锁已释放，安全地阻塞调用 I2S 写入 ──
            esp_err_t err = bsp_play_audio_stream(local_buf, to_play);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "流式播放失败: %s", esp_err_to_name(err));
            }
        } else {
            // 缓冲区为空
            if (self->streaming_finished) {
                // 服务端已发完，缓冲区也清空，安全退出
                ESP_LOGI(TAG, "缓冲区已空且流式传输完成，退出播放任务");
                break;
            }
            // 等待更多数据（短暂让出 CPU，避免空转）
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    // 停止音频输出
    bsp_audio_stop();

    ESP_LOGI(TAG, "播放任务结束");
    self->playback_task_running = false;
    self->playback_task_handle  = nullptr;
    vTaskDelete(NULL);
}
