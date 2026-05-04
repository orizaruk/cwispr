#include <atomic>
#include <condition_variable>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <vector>

#include "device_utils.h"
#include "worker.h"

#include "miniaudio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
void data_callback(ma_device* p_device, void* p_output, const void* p_input,
                   ma_uint32 frame_count) {
    const auto p_context = static_cast<RecordingContext*>(p_device->pUserData);

    if (!p_context->is_recording) {
        return; // Drop the audio and exit
    }

    const auto p_samples = static_cast<const int16_t*>(p_input);
    auto& buffer = p_context->audio_buffer;
    // Insert the frames into vector in the recording context
    buffer.insert(buffer.end(), p_samples, p_samples + frame_count);
}
} // namespace

int main() {
    QueueContext queue_context;
    RecordingContext recorder_context;

    // Initialize the recorder device config
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = 16000;
    config.dataCallback = data_callback;
    config.pUserData = &recorder_context;

    ma_device device;

    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        std::cerr << "CRITICAL ERROR: Failed to initialize the microphone!\n";
        std::cerr << "Press Enter to exit...\n";
        std::cin.get(); // Forces the console to stay open
        return -1; // Failed to initialize the device
    }

    ma_device_start(&device); // Start the device

    std::thread transcription_thread(process_audio_queue,
                                     std::ref(queue_context)); // Start the transcription thread

    // Hotkey Logic
    bool prev_state = false;
    // States: Not recording, Recording, Finished recording
    
    std::cout << "Hardware initialized. Press Left Ctrl + Left Win to record. Press Escape to "
                 "quit.\n";

    while (true) {

        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            std::cout << "Initiating shutdown...\n";
            {
                std::lock_guard<std::mutex> lock(queue_context.queue_mutex);
                queue_context.need_to_exit = true; // Modify flag for worker thread to close
            }
            queue_context.cv.notify_all();
            break;
        }

        const bool is_pressing =
            (GetAsyncKeyState(VK_LCONTROL) & 0x8000) && (GetAsyncKeyState(VK_LWIN) & 0x8000);

        if (is_pressing && !prev_state) {
            recorder_context.is_recording = true;
            std::cout << "Recording started...\n";
        } else if (!is_pressing && prev_state) {
            recorder_context.is_recording = false;
            std::cout << "Stopping recording...\n";
            // Grab mutex, transfer ownership of the vector the queue using move and unlock it.
            // Then, notify the CV.
            {
                std::unique_lock<std::mutex> lock(queue_context.queue_mutex); // Grab the lock
                queue_context.buffer_queue.push(
                    std::move(recorder_context.audio_buffer)); // Move the buffer into the queue and reset it
            } // Lock will go out of scope and unlock

            queue_context.cv.notify_one(); // Notify the consumer
        }

        prev_state = is_pressing;

        Sleep(10);
    }

    ma_device_uninit(&device);

    std::cout << "Waiting for background thread to finish...\n";
    if (transcription_thread.joinable()) {
        transcription_thread.join();
    }
    
    std::cout << "Finished shutdown process.\n";
    return 0;
}
