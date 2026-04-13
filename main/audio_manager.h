/**
 * @file audio_manager.h
 * @brief 🎧 音频管理器类 - 统一管理音频的录制和播放
 * 
 * 这个类就像一个“音频指挥家”，负责协调所有音频相关的工作：
 * 
 * 🎙️ 录音功能：
 * - 管理录音缓冲区（最多10秒）
 * - 控制录音的开始/停止
 * - 跟踪录音时长
 * 
 * 🔊 播放功能：
 * - 播放本地音频文件
 * - 流式播放网络音频
 * - 缓冲区管理，避免卡顿
 * 
 * 🌐 网络功能：
 * - 接收WebSocket音频流
 * - 处理不同采样率的音频
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"

class AudioManager {
public:
    /**
     * @brief 创建音频管理器
     * 
     * @param sample_rate 采样率（默认16000Hz，人声标准）
     * @param recording_duration_sec 最大录音时长（默认10秒）
     * @param response_duration_sec AI回复最大时长（默认32秒）
     */
    AudioManager(uint32_t sample_rate = 16000,
                 uint32_t recording_buffer_sec = 15,
                 uint32_t response_duration_sec = 32);
    
    /**
     * @brief 析构函数，释放所有分配的内存
     */
    ~AudioManager();

    /**
     * @brief 初始化音频管理器
     * 
     * 这个函数会分配所需的内存缓冲区。
     * 必须在使用其他功能前调用。
     * 
     * @return ESP_OK=成功，ESP_ERR_NO_MEM=内存不足
     */
    esp_err_t init();

    /**
     * @brief 反初始化，释放资源
     */
    void deinit();

    // 🎙️ ========== 录音相关功能 ==========
    
    /**
     * @brief 开始录音
     */
    void startRecording();

    /**
     * @brief 停止录音
     */
    void stopRecording();

    /**
     * @brief 查询录音状态
     * 
     * @return true=正在录音中，false=没在录音
     */
    bool isRecording() const { return is_recording; }

    /**
     * @brief 添加音频数据到录音环形缓冲区（满了覆盖旧数据）
     *
     * @param data 音频数据指针
     * @param samples 样本数量
     * @return true=添加成功
     */
    bool addRecordingData(const int16_t* data, size_t samples);

    /**
     * @brief 导出录音数据（按时间顺序拷贝到外部缓冲区）
     *
     * @param[out] out_buf 外部缓冲区（调用者分配）
     * @param[out] out_samples 导出的样本数
     */
    void exportRecordingData(int16_t* out_buf, size_t& out_samples) const;

    /**
     * @brief 清空录音缓冲区
     */
    void clearRecordingBuffer();

    /**
     * @brief 获取已录音时间
     * 
     * @return 录音时长（单位：秒）
     */
    float getRecordingDuration() const;

    /**
     * @brief 获取录音缓冲区中有效数据的样本数
     */
    size_t getRecordingLength() const { return recording_length; }

    // 🔊 ========== 音频播放相关功能 ==========

    /**
     * @brief 开始接收响应音频数据（用于WebSocket）
     */
    void startReceivingResponse();

    /**
     * @brief 添加响应音频数据块
     * 
     * @param data 音频数据
     * @param size 数据大小（字节）
     * @return true 成功，false 失败（缓冲区溢出等）
     */
    bool addResponseData(const uint8_t* data, size_t size);

    /**
     * @brief 完成响应音频接收并播放
     * 
     * @return esp_err_t 播放结果
     */
    esp_err_t finishResponseAndPlay();
    
    // 🌊 ========== 流式播放功能（边下载边播放） ==========
    
    /**
     * @brief 开始流式播放模式
     * 
     * 调用后可以不断添加音频数据块，实现边下载边播放。
     */
    void startStreamingPlayback();
    
    /**
     * @brief 添加一小段音频到播放队列
     * 
     * 在流式播放模式下，不断调用这个函数添加新的音频段。
     * 
     * @param data 音频数据
     * @param size 数据字节数
     * @return true=添加成功，false=缓冲区满
     */
    bool addStreamingAudioChunk(const uint8_t* data, size_t size);
    
    /**
     * @brief 结束流式播放
     * 
     * 播放剩余的音频数据并停止流式模式。
     */
    void finishStreamingPlayback();
    
    /**
     * @brief 检查流式播放是否正在进行
     * 
     * @return true 正在播放，false 未在播放
     */
    bool isStreamingActive() const { return is_streaming; }
    
    /**
     * @brief 标记流式播放已完成
     */
    void setStreamingComplete() { response_played = true; }

    /**
     * @brief 播放一段完整的音频
     * 
     * 用于播放本地存储的音频文件，一次性播放完毕。
     * 
     * @param audio_data 音频数据（PCM格式）
     * @param data_len 数据字节数
     * @param description 音频描述（如“欢迎音频”）
     * @return ESP_OK=播放成功
     */
    esp_err_t playAudio(const uint8_t* audio_data, size_t data_len, const char* description);

    /**
     * @brief 查询AI回复是否播放完成
     * 
     * @return true=已播放完成，false=还没播完
     */
    bool isResponsePlayed() const { return response_played; }

    /**
     * @brief 重置响应播放标志
     */
    void resetResponsePlayedFlag() { response_played = false; }


    // 🔧 ========== 工具函数 ==========

    /**
     * @brief 获取采样率
     * 
     * @return 采样率（Hz）
     */
    uint32_t getSampleRate() const { return sample_rate; }

    /**
     * @brief 获取录音缓冲区大小（样本数）
     * 
     * @return 缓冲区大小
     */
    size_t getRecordingBufferSize() const { return recording_buffer_size; }

    /**
     * @brief 获取响应缓冲区大小（字节）
     * 
     * @return 缓冲区大小
     */
    size_t getResponseBufferSize() const { return response_buffer_size; }

private:
    // 🎶 音频参数
    uint32_t sample_rate;               // 采样率（Hz）
    uint32_t response_duration_sec;     // 最大回复时长（秒）

    // 🎙️ 录音环形缓冲区相关变量
    int16_t* recording_buffer;          // 录音数据缓冲区
    size_t recording_buffer_size;       // 缓冲区大小（样本数）
    size_t recording_length;            // 有效数据样本数（未满时 = write_pos）
    size_t recording_write_pos;         // 环形缓冲区写入位置
    bool recording_wrapped;             // 缓冲区是否已经绕回过
    bool is_recording;                  // 是否正在录音

    // 🔊 响应音频相关变量
    int16_t* response_buffer;           // AI回复音频缓冲区
    size_t response_buffer_size;        // 缓冲区大小（字节数）
    size_t response_length;             // 已接收的样本数
    bool response_played;               // 是否已播放完成

    
    // 🌊 流式播放相关变量
    bool is_streaming;                  // 是否在流式播放中
    uint8_t* streaming_buffer;          // 环形缓冲区（仅保留，不再在回调里直接播放）
    size_t streaming_buffer_size;       // 缓冲区大小
    size_t streaming_write_pos;         // 写入位置
    size_t streaming_read_pos;          // 读取位置
    static const size_t STREAMING_BUFFER_SIZE = 65536;
    static const size_t STREAMING_CHUNK_SIZE = 3200;

    // 🎬 独立播放任务
    TaskHandle_t  playback_task_handle;
    QueueHandle_t audio_queue;           // 存放 {size_t size, uint8_t data[]} 的 malloc 块
    volatile bool playback_task_running;
    volatile bool streaming_finished;    // finishStreamingPlayback 已被调用
    static const size_t AUDIO_QUEUE_SIZE = 64; // 每块约 2~6KB，共约 10 秒缓冲

    static void playbackTaskFunc(void* param);

    // 🏷️ 日志标签
    static const char* TAG;
};

#endif // AUDIO_MANAGER_H