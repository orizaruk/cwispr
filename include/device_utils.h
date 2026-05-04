#pragma once
#include <vector>
#include <queue>
#include <mutex>

struct RecordingContext {
    std::vector<int16_t> audio_buffer;
    std::atomic<bool> is_recording{false};
};

struct QueueContext {
    std::queue<std::vector<int16_t>> buffer_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
    bool need_to_exit;
};

int print_audio_devices();

