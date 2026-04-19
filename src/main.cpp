#include <atomic>
#include <format>
#include <iostream>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <condition_variable>

#include "miniaudio.h"
#include <windows.h>

#include "device_utils.h"

struct RecordingContext {
    std::vector<int16_t> audio_buffer;
    std::atomic<bool> is_recording{false};
};

struct QueueContext {
    std::queue<std::vector<int16_t>> buffer_queue;
    std::mutex queue_mutex;
    std::condition_variable cv;
};

namespace {
void data_callback(ma_device* p_device, void* p_output, const void* p_input,
                   ma_uint32 frame_count) {
    const auto p_context = static_cast<RecordingContext*>(p_device->pUserData);

    if (!p_context->is_recording) {
        return; /* Drop the audio and exit */
    }

    const auto p_samples = static_cast<const int16_t*>(p_input);
    auto& buffer = p_context->audio_buffer;
    /* Insert the frames into the vector */
    buffer.insert(buffer.end(), p_samples, p_samples + frame_count);
}
}

void process_audio_queue(QueueContext& q_context) {
    /* The thread should consume audio vectors from the queue and process them.
     * We implement a producer-consumer mechanism with the main thread.
     */
    while (true) {
        /* Flow:
         * 1. Wait for waking up if there are jobs in the queue
         * 2. Try to grab mutex
         * 3. Extract buffer from queue
         * 4. Release
         */
        {
            std::unique_lock<std::mutex> lock(q_context.queue_mutex); /* Create the unique lock */
            /* Conditional variable unlocks mutex, waits for signal to wake up and then locks mutex,
             * checks that buffer is not empty If true, it will keep the lock and finish executing.
             * Else, will return to sleep.
             * Only then will it move on.
             */
            q_context.cv.wait(lock, [&q_context]() { return !q_context.buffer_queue.empty(); });

            /* Move the audio buffer into the transcription thread, release mutex, and then begin processing */
        }
    }
}

int main() {
    QueueContext queue_context;
    RecordingContext recorder_context;

    // Initialize the config
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = 16000;
    config.dataCallback = data_callback;
    config.pUserData = &recorder_context;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        return -1; // Failed to initialize the device    
    }

    ma_device_start(&device); /* Start the device */

    std::thread transcription_thread(process_audio_queue, std::ref(queue_context)); /* Start the transcription thread */

    /* Hotkey Logic */
    bool prev_state = false;
    /* States: Not recording, Recording, Finished recording */
    while (true) {

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            std::cout << "Breaking the loop...\n";
            break;
        }

        const bool is_pressing =
            (GetAsyncKeyState(VK_LCONTROL) & 0x8000) && (GetAsyncKeyState(VK_LWIN) & 0x8000);

        if (is_pressing && !prev_state) {
            recorder_context.is_recording = true;
            std::cout << "Recording started...\n";
        }
        else if (!is_pressing && prev_state) {
            recorder_context.is_recording = false;
            std::cout << "Stopping recording...\n";
            /* Grab mutex, transfer ownership of the vector the queue using move and unlock it. Then, notify the CV. */
            {
                std::unique_lock<std::mutex> lock(queue_context.queue_mutex); /* Grab the lock */
                queue_context.buffer_queue.push();
            }
        }

        prev_state = is_pressing;

        Sleep(10);
    }

    ma_device_uninit(&device);
    return 0;
}