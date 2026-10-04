#pragma once
#include <atomic>
#include <condition_variable>
#include <vector>
#include <queue>
#include <mutex>
#include <string>

#include "miniaudio.h"

struct RecordingContext {
    std::vector<int16_t> audio_buffer;
    // Guards audio_buffer: the miniaudio callback appends to it on the audio thread while the
    // UI thread clears it / moves it out.
    std::mutex buffer_mutex;
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
