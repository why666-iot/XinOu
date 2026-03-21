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
    
    // 分配流式播放缓冲区
    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "✓ 流式播放缓冲区分配成功，大小: %zu 字节", streaming_buffer_size);
    
    return ESP_OK;
}

void AudioManager::deinit() {
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
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    // 清空缓冲区
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 📏 计算环形缓冲区的剩余空间
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }

    // 🔄 缓冲区满时先播放已有数据腾出空间，而不是丢弃新数据
    if (size > available_space) {
        ESP_LOGD(TAG, "缓冲区空间不足(需要%zu,可用%zu)，先播放腾出空间", size, available_space);

        // 计算当前可播放的数据量
        size_t available_data;
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }

        // 播放已有数据直到有足够空间
        while (available_data >= STREAMING_CHUNK_SIZE && size > available_space) {
            uint8_t chunk[STREAMING_CHUNK_SIZE];
            size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
            if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
                memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
                streaming_read_pos += STREAMING_CHUNK_SIZE;
            } else {
                memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
                memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
                streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
            }
            if (streaming_read_pos >= streaming_buffer_size) {
                streaming_read_pos = 0;
            }

            esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "腾空间播放失败: %s", esp_err_to_name(ret));
                break;
            }

            // 重新计算空间
            if (streaming_write_pos >= streaming_read_pos) {
                available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
                available_data = streaming_write_pos - streaming_read_pos;
            } else {
                available_space = streaming_read_pos - streaming_write_pos - 1;
                available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
            }
        }

        // 如果播放后仍然空间不足，才真正丢弃
        if (size > available_space) {
            ESP_LOGW(TAG, "流式缓冲区空间不足: 需要 %zu, 可用 %zu", size, available_space);
            return false;
        }
    }
    
    // 📝 将数据写入环形缓冲区
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        // 简单情况：数据不跨越缓冲区末尾
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        // 复杂情况：数据跨越末尾，需要分两段写入
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    // 如果写位置到达缓冲区末尾，循环回到开头
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    ESP_LOGD(TAG, "添加流式音频块: %zu 字节, 写位置: %zu, 读位置: %zu", 
             size, streaming_write_pos, streaming_read_pos);
    
    // 🔍 检查是否有足够的数据可以播放
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        // 简单情况：写指针在读指针后面
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        // 复杂情况：数据跨越了缓冲区末尾
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    // 🎵 如果积累了足够的数据（200ms），开始播放
    while (available_data >= STREAMING_CHUNK_SIZE) {
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        
        // 📖 从环形缓冲区读取一块数据
        size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
        if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
            memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
            streaming_read_pos += STREAMING_CHUNK_SIZE;
        } else {
            memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
            memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
            streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
        }
        
        // 如果读位置到达缓冲区末尾，循环回到开头
        if (streaming_read_pos >= streaming_buffer_size) {
            streaming_read_pos = 0;
        }
        
        // 🎶 播放这一块音频（使用流式版本，不会中断播放）
        esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "流式音频播放失败: %s", esp_err_to_name(ret));
            break;
        }
        
        // 重新计算可用数据
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
    }
    
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "结束流式音频播放");
    
    // 🎬 处理最后的尾巴数据（不足200ms的部分）
    size_t remaining_data;
    if (streaming_write_pos >= streaming_read_pos) {
        remaining_data = streaming_write_pos - streaming_read_pos;
    } else {
        remaining_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    if (remaining_data > 0) {
        // 分配临时缓冲区
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            // 读取所有剩余数据
            if (streaming_write_pos >= streaming_read_pos) {
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, remaining_data);
            } else {
                size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, bytes_to_end);
                memcpy(remaining_buffer + bytes_to_end, streaming_buffer, streaming_write_pos);
            }
            
            // 🎹 播放最后的尾巴数据（使用普通版本，会停止I2S）
            esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ 播放剩余音频: %zu 字节", remaining_data);
            } else {
                ESP_LOGE(TAG, "❌ 播放剩余音频失败: %s", esp_err_to_name(ret));
            }
            
            free(remaining_buffer);
        }
    }
    
    is_streaming = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
}