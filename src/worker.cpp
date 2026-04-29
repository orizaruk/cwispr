#include "worker.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include <device_utils.h>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <miniaudio.h>


// WAV File Header according to https://en.wikipedia.org/wiki/WAV#WAV_file_header
#pragma pack(push, 1)
struct WavHeader {
    // [Master RIFF chunk]
    char riff[4] = {'R', 'I', 'F', 'F'};        // FileTypeBlocID
    uint32_t file_size;                         // FileSize
    char file_format[4] = {'W', 'A', 'V', 'E'}; // FileFormatID

    // [Chunk describing the data format]
    char fmt[4] = {'f', 'm', 't', ' '};              // FormatBlocID
    uint32_t chunk_size = 16;                        // BlocSize
    uint16_t audio_format = 1;                       // AudioFormat
    uint16_t num_channels = 1;                       // NbrChannels
    uint32_t frequency = 16000;                      // Frequency
    uint32_t bytes_per_sec = 16000 * (1 * (16 / 8)); // BytePerSec
    uint16_t bytes_per_bloc = 1 * (16 / 8);          // BytePerBloc
    uint16_t bits_per_sample = 16;                   // BitsPerSample

    // [Chunk containing the sampled data]
    char data_id[4] = {'d', 'a', 't', 'a'}; // DataBlocID
    uint32_t data_size;                     // DataSize
};
#pragma pack(pop)

void save_memory_to_disk(const std::string& memory_bucket, const std::string& fileName) {
    std::ofstream out_file(fileName, std::ios::binary);

    if (!out_file) {
        std::cerr << "Failed to open file for writing.\n";
        return;
    }

    out_file.write(memory_bucket.data(), memory_bucket.size());

    out_file.close();
}

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
    std::string model_name = "whisper-large-v3";

    auto cli = std::make_unique<httplib::SSLClient>("api.groq.com");
    cli->set_bearer_token_auth("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");

    while (true) {
        std::vector<int16_t> local_audio;

        {
            // Wait on the conditional variable for signal from main thread
            std::unique_lock<std::mutex> lock(q_context.queue_mutex);
            q_context.cv.wait(lock, [&q_context]() {
                return !q_context.buffer_queue.empty();
            }); // Wake up and take lock

            local_audio = std::move(q_context.buffer_queue.front());
            // Move the vector out of the queue into the local vector
            q_context.buffer_queue.pop(); // Now delete the empty vector from the queue
        }

        std::cout << "Processing " << local_audio.size() << " frames...\n";

        // Create .WAV file

        // a. In the disk:
        /*if (create_wav_file(local_audio) != MA_SUCCESS) {
            std::cout << "Failed to create .wav file.\n";
        }*/

        // b. In memory:
        WavHeader header;
        uint32_t data_byte_size = sizeof(uint16_t) * local_audio.size();

        header.data_size = data_byte_size; // Size of the data portion of the file
        header.file_size =
            data_byte_size + sizeof(WavHeader) - 8; // Size of the whole file according to standard

        // Creating the full .WAV file in memory (header + local_audio data portion)
        std::string memory_file;
        memory_file.resize(sizeof(WavHeader) + data_byte_size);

        std::memcpy(memory_file.data(), &header, sizeof(WavHeader)); // Copy the header
        std::memcpy(memory_file.data() + sizeof(WavHeader), local_audio.data(), data_byte_size);

        // For testing: Saving the 'in-memory' file to disk to validate correct behavior
        // save_memory_to_disk(memory_file, "test.wav");

        // Create the multipart form data
        httplib::UploadFormDataItems items = {{"file", memory_file, "audio.wav", "audio/wav"},
                                              {"model", model_name, "", ""},
                                              {"response_format", "text", "", ""}};
        // Send the request
        auto res = cli->Post("/openai/v1/audio/transcriptions", items);

        if (!res) {
            auto err = res.error();
            std::cerr << "Error: " << httplib::to_string(err) << "\n";
            if (err == httplib::Error::SSLConnection) {
                std::cerr << "SSL error code: " << res.ssl_error() << "\n";
                std::cerr << "Backend error: " << res.ssl_backend_error() << "\n";
            }
            continue;
        }

        switch (res->status) {
        case httplib::StatusCode::OK_200:
            std::cout << "SUCCESS!\n";
            std::cout << "Status code:\n";
            std::cout << res->status << "\n";
            std::cout << "Response body:\n";
            std::cout << res->body << "\n";
            break;

        default:
            std::cout << "Status code:\n";
            std::cout << res->status << "\n";
            std::cout << "Response body:\n";
            std::cout << res->body << "\n";
            break;
        }

        // Paste the text
    }
}

void paste_text(const std::string& text) {
    HWND hwnd =
        CreateWindowEx(0, "STATIC", "DummyWindow", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL); // Initialize hidden window

    if (hwnd == NULL) {
        std::cerr << "Failed to create invisible Windows window.\n";
        return;
    }

    if (OpenClipboard(hwnd)) {
        
    }
        
}