#include <atomic>
#include <format>
#include <iostream>
#include <vector>

#include "miniaudio.h"
#include <windows.h>

#include "device_utils.h"

struct RecordingContext {
    std::vector<int16_t> audio_buffer;
    std::atomic<bool> is_recording{false};
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

int main() {
    RecordingContext context;

    // Initialize the config
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = 16000;
    config.dataCallback = data_callback;
    config.pUserData = &context;

    ma_device device;
    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        return -1; // Failed to initialize the device    
    }

    ma_device_start(&device); // Start the device

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
            context.is_recording = true;
            std::cout << "Recording started...\n";
        }
        else if (!is_pressing && prev_state) {
            context.is_recording = false;
            std::cout << "Stopping recording...\n";
            /* Start processing audio */

        }

        prev_state = is_pressing;

        Sleep(10);
    }

    ma_device_uninit(&device);
    return 0;
}