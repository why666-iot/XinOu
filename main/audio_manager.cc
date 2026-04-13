/**
 * @file audio_manager.cc
 * @brief 🎧 音频管理器实现文件
 * 
 * 这里实现了audio_manager.h中声明的所有功能。
 * 主要包括录音缓冲区管理、音频播放控制和流式播放。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_buffer_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(sample_rate * recording_buffer_sec)
    , recording_length(0)
    , recording_write_pos(0)
    , recording_wrapped(false)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
    , playback_task_handle(nullptr)
    , audio_queue(nullptr)
    , playback_task_running(false)
    , streaming_finished(false)
{
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器...");
    
    // 分配录音缓冲区
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 录音环形缓冲区分配成功，大小: %zu 字节 (%zu 秒)",
             recording_buffer_size * sizeof(int16_t), recording_buffer_size / sample_rate);
    
    // 分配响应缓冲区
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 响应缓冲区分配成功，大小: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    // 创建音频播放队列（每个槽存一个 malloc 块的指针）
    audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(uint8_t*));
    if (audio_queue == nullptr) {
        ESP_LOGE(TAG, "音频播放队列创建失败");
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 音频播放队列创建成功，容量: %zu 块", AUDIO_QUEUE_SIZE);

    return ESP_OK;
}

void AudioManager::deinit() {
    // 停止播放任务（如果还在运行）
    if (playback_task_running) {
        streaming_finished = true;
        if (audio_queue) {
            uint8_t* ptr;
            while (xQueueReceive(audio_queue, &ptr, 0) == pdTRUE) {
                free(ptr);
            }
        }
        uint32_t timeout = 200;
        while (playback_task_running && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (audio_queue != nullptr) {
        vQueueDelete(audio_queue);
        audio_queue = nullptr;
    }

    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }

    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }

    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

// 🎙️ ========== 录音功能实现 ==========

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    recording_write_pos = 0;
    recording_wrapped = false;
    ESP_LOGI(TAG, "开始录音...");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)",
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }

    for (size_t i = 0; i < samples; i++) {
        recording_buffer[recording_write_pos] = data[i];
        recording_write_pos++;
        if (recording_write_pos >= recording_buffer_size) {
            recording_write_pos = 0;
            recording_wrapped = true;
        }
    }

    // 更新有效数据长度
    if (recording_wrapped) {
        recording_length = recording_buffer_size;
    } else {
        recording_length = recording_write_pos;
    }

    return true;
}

void AudioManager::exportRecordingData(int16_t* out_buf, size_t& out_samples) const {
    if (recording_buffer == nullptr || recording_length == 0) {
        out_samples = 0;
        return;
    }

    if (!recording_wrapped) {
        // 没绕回，直接拷贝 0 ~ write_pos
        memcpy(out_buf, recording_buffer, recording_write_pos * sizeof(int16_t));
        out_samples = recording_write_pos;
    } else {
        // 绕回了，先拷贝 write_pos ~ end，再拷贝 0 ~ write_pos
        size_t tail = recording_buffer_size - recording_write_pos;
        memcpy(out_buf, &recording_buffer[recording_write_pos], tail * sizeof(int16_t));
        memcpy(&out_buf[tail], recording_buffer, recording_write_pos * sizeof(int16_t));
        out_samples = recording_buffer_size;
    }
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
    recording_write_pos = 0;
    recording_wrapped = false;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

// 🔊 ========== 音频播放功能实现 ==========

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "📦 接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "📢 播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 🔁 添加重试机制，确保音频可靠播放
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ 响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "❌ 音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s...", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ %s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}


// 🌊 ========== 流式播放功能实现 ==========

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放");
    is_streaming = true;
    streaming_finished = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;

    // 清空队列中的残留数据
    if (audio_queue) {
        uint8_t* ptr;
        while (xQueueReceive(audio_queue, &ptr, 0) == pdTRUE) {
            free(ptr);
        }
    }

    // 启动独立播放任务，避免阻塞 WebSocket 回调
    playback_task_running = true;
    BaseType_t ret = xTaskCreate(
        playbackTaskFunc, "audio_play",
        4096, this,
        configMAX_PRIORITIES - 2,
        &playback_task_handle
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        playback_task_running = false;
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !data || size == 0 || !audio_queue) {
        return false;
    }

    // 分配内存块：前 sizeof(size_t) 字节存长度，后跟音频数据
    uint8_t* block = (uint8_t*)malloc(sizeof(size_t) + size);
    if (!block) {
        ESP_LOGE(TAG, "音频块内存分配失败: %zu 字节", sizeof(size_t) + size);
        return false;
    }

    memcpy(block, &size, sizeof(size_t));
    memcpy(block + sizeof(size_t), data, size);

    // 非阻塞投递（立即返回，不阻塞 WebSocket 回调任务）
    if (xQueueSend(audio_queue, &block, 0) != pdTRUE) {
        ESP_LOGW(TAG, "音频队列已满，丢弃 %zu 字节", size);
        free(block);
        return false;
    }

    ESP_LOGD(TAG, "入队音频块: %zu 字节", size);
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }

    ESP_LOGI(TAG, "结束流式播放，等待播放任务排空队列...");

    // 通知播放任务：数据已全部入队，队列排空后可退出
    streaming_finished = true;

    // 等待播放任务自然退出（最多10秒）
    uint32_t timeout = 10000 / 50;
    while (playback_task_running && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (playback_task_running) {
        ESP_LOGW(TAG, "播放任务超时，强制终止");
        if (playback_task_handle) {
            vTaskDelete(playback_task_handle);
            playback_task_handle = nullptr;
        }
        bsp_audio_stop();
        playback_task_running = false;
    }

    is_streaming = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    ESP_LOGI(TAG, "流式播放完成");
}