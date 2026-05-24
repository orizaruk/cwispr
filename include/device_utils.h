#pragma once
#include <vector>
#include <queue>
#include <mutex>
#include <string>

#include "miniaudio.h"

struct RecordingContext {
    std::vector<int16_t> audio_buffer;
    std::atomic<bool> is_recording{false};
};

struct QueueContext {
    std::queue<std::vector<int16_t>> buffer_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    bool need_to_exit{false};
};

int print_audio_devices();
std::wstring getEnvVariable(const std::wstring& var_name);
std::wstring ConvertToUTF16(const std::string& input_string);
std::string ConvertWideToUtf8(const std::wstring& wstr);
std::string getCaptureDeviceName(ma_device& device);