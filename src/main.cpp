#include <atomic>
#include <condition_variable>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <vector>

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
        return; // Drop the audio and exit
    }

    const auto p_samples = static_cast<const int16_t*>(p_input);
    auto& buffer = p_context->audio_buffer;
    // Insert the frames into vector in the recording context
    buffer.insert(buffer.end(), p_samples, p_samples + frame_count);
}
} // namespace

int create_wav_file(const std::vector<int16_t>& audio_vector) {
    ma_encoder_config encoder_config =
        ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);

    ma_encoder encoder;

    // Initialize the encoder, creates the file
    if (ma_encoder_init_file("output.wav", &encoder_config, &encoder) != MA_SUCCESS) {
        return -1;
    }

    if (ma_encoder_write_pcm_frames(&encoder, audio_vector.data(), audio_vector.size(), NULL) !=
        MA_SUCCESS) {
        return -1;    
    }  

    ma_encoder_uninit(&encoder);

    return 0;
}

void process_audio_queue(QueueContext& q_context) {
    while (true) {
        std::vector<int16_t> local_audio;

        {   // Wait on the conditional variable for signal from main thread
            std::unique_lock<std::mutex> lock(q_context.queue_mutex); 
            q_context.cv.wait(lock, [&q_context]() { return !q_context.buffer_queue.empty(); }); // Wake up and take lock

            
            local_audio = std::move(q_context.buffer_queue.front()); // Move the vector out of the queue into the local vector
            q_context.buffer_queue.pop(); // Now delete the empty vector from the queue
        } 

        std::cout << "Processing " << local_audio.size() << " frames...\n";

        // Create .WAV file
        if (create_wav_file(local_audio) != MA_SUCCESS) {
            std::cout << "Failed to create .wav file.\n";
        }
    }
}

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
        return -1; // Failed to initialize the device
    }

    ma_device_start(&device); // Start the device

    std::thread transcription_thread(process_audio_queue,
                                     std::ref(queue_context)); // Start the transcription thread

    transcription_thread.detach(); // ADD FOR PROTOTYPING, ADD PROPER SHUTDOWN MECHANISM LATER!!!!!

    // Hotkey Logic
    bool prev_state = false;
    // States: Not recording, Recording, Finished recording
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
    return 0;
}
