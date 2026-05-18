#include "worker.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "device_utils.h"

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

// This function converts a UTF-8 std::string to UTF-16 std::wstring, which is the required format
// for the Windows clipboard.
std::wstring ConvertToUTF16(const std::string& input_string) {
    if (input_string.empty())
        return std::wstring();

    int required_size = MultiByteToWideChar(CP_UTF8, 0, input_string.c_str(), -1, NULL, 0);

    if (required_size == 0) {
        std::cerr << "Conversion failed.\n";
        return std::wstring();
    }

    std::wstring output_string(required_size, 0); // Allocate the buffer for the converted string

    // Perform the conversion
    MultiByteToWideChar(CP_UTF8, 0, input_string.c_str(), -1, &output_string[0], required_size);
    output_string.pop_back();

    return output_string;
}

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

void paste_text(const std::string& text) {
    HWND hwnd = CreateWindowEx(0, "STATIC", "DummyWindow", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL,
                               NULL); // Initialize hidden window

    if (hwnd == NULL) {
        std::cerr << "Failed to create invisible Windows window.\n";
        return;
    }

    // 1. Copy contents of clipboard if it's text, mark the flag as true to know to restore it at
    // the end
    std::wstring saved_clipboard_text;
    bool saved_clipboard_flag = false;

    if (OpenClipboard(hwnd)) {
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE hClipboardData = GetClipboardData(CF_UNICODETEXT);
            if (hClipboardData != nullptr) {
                auto pClipboardData = static_cast<wchar_t*>(GlobalLock(hClipboardData));
                if (pClipboardData != nullptr) {
                    saved_clipboard_text = pClipboardData;
                    GlobalUnlock(hClipboardData); // unlock memory after finish using
                    saved_clipboard_flag = true;
                }
            }
        }
        CloseClipboard();
    }

    // 2. Set clipboard contents to transcribed text
    bool clipboard_ready_to_paste = false;

    std::wstring transcription_text = ConvertToUTF16(text); // Convert from UTF-8 to UTF-16
    if (OpenClipboard(hwnd)) {
        // Take ownership of the clipboard, required for SetClipboardData
        if (EmptyClipboard() == 0) {
            CloseClipboard();
            return;
        }

        // Create the Handle that will hold the string to paste
        size_t bytes_needed_for_clipboard = (transcription_text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes_needed_for_clipboard);

        if (hMem == nullptr) {
            std::cerr << "Allocation by GlobalAlloc failed.\n";
            CloseClipboard();
            return; // Allocation failed
        }

        // Lock the handle
        void* pMem = GlobalLock(hMem);

        if (pMem == nullptr) {
            GlobalFree(hMem);
            std::cerr << "Failed to lock the handle for pasting the string contents.\n";
            CloseClipboard();
            return;
        }

        // Write the string to the handle
        memcpy(pMem, transcription_text.c_str(), bytes_needed_for_clipboard);

        // Unlock the handle, we finished
        GlobalUnlock(hMem);

        // Replace the clipboard contents with the text
        if (SetClipboardData(CF_UNICODETEXT, hMem) == nullptr) {
            // Windows failed with modifying clipboard content, we need to free the handle since we
            // are still owners of it
            GlobalFree(hMem);
            std::cerr << "Windows failed to modify the clipboard contents.\n";
        }

        clipboard_ready_to_paste = true;
        CloseClipboard();
    }

    // 3. Simulate Ctrl+V in order to paste clipboard contents
    if (clipboard_ready_to_paste) {
        // Construct the inputs array, we release Ctrl+WIN in case they are pressed and then
        // simulate Ctrl+V and release
        INPUT inputs[6] = {};

        // Release CTRL
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = VK_CONTROL;
        inputs[0].ki.dwFlags = KEYEVENTF_KEYUP;

        // Release WIN key
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wVk = VK_LWIN;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

        // Press CTRL
        inputs[2].type = INPUT_KEYBOARD;
        inputs[2].ki.wVk = VK_CONTROL;

        // Press V
        inputs[3].type = INPUT_KEYBOARD;
        inputs[3].ki.wVk = 'V';

        // Release V
        inputs[4].type = INPUT_KEYBOARD;
        inputs[4].ki.wVk = 'V';
        inputs[4].ki.dwFlags = KEYEVENTF_KEYUP;

        // Release CTRL
        inputs[5].type = INPUT_KEYBOARD;
        inputs[5].ki.wVk = VK_CONTROL;
        inputs[5].ki.dwFlags = KEYEVENTF_KEYUP;

        UINT uSent = SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
        if (uSent != ARRAYSIZE(inputs)) {
            std::cerr << "Failed to send all the inputs.\n";
        }

        Sleep(50); // Added sleep to allow the application to process the keyboard inputs before we
                   // modify clipboard contents
    }

    // 4. Restore clipboard contents to previous state
    if (saved_clipboard_flag) {
        // saved_clipboard_text wstring
        if (OpenClipboard(hwnd)) {
            // Take ownership of the clipboard, required for SetClipboardData
            if (EmptyClipboard() == 0) {
                CloseClipboard();
                return;
            }

            // Create the Handle that will hold the string to paste
            size_t bytes_needed_for_clipboard =
                (saved_clipboard_text.size() + 1) * sizeof(wchar_t); // add 1 for null terminator
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes_needed_for_clipboard);

            if (hMem == nullptr) {
                std::cerr << "Allocation by GlobalAlloc failed.\n";
                CloseClipboard();
                return; // Allocation failed
            }

            // Lock the handle
            void* pMem = GlobalLock(hMem);

            if (pMem == nullptr) {
                GlobalFree(hMem);
                std::cerr << "Failed to lock the handle for pasting the old string contents.\n";
                CloseClipboard();
                return;
            }

            // Write the string to the handle
            memcpy(pMem, saved_clipboard_text.c_str(), bytes_needed_for_clipboard);

            // Unlock the handle, we finished
            GlobalUnlock(hMem);

            // Replace the clipboard contents with the text
            if (SetClipboardData(CF_UNICODETEXT, hMem) == nullptr) {
                // Windows failed with modifying clipboard content, we need to free the handle since
                // we are still owners of it
                GlobalFree(hMem);
                std::cerr << "Windows failed to modify the clipboard contents.\n";
            }

            CloseClipboard();
        }
    }

    // 5. Destroy the window created for the Handle to avoid leak
    DestroyWindow(hwnd);
}

void process_audio_queue(QueueContext& q_context) {
    std::string model_name = "whisper-large-v3";

    auto cli = std::make_unique<httplib::SSLClient>("api.groq.com");
    cli->set_bearer_token_auth("API_KEY_GOES_HERE");

    while (true) {
        std::vector<int16_t> local_audio;

        {
            // Wait on the conditional variable for signal from main thread
            std::unique_lock<std::mutex> lock(q_context.queue_mutex);
            q_context.cv.wait(lock, [&q_context]() {
                return !q_context.buffer_queue.empty() || q_context.need_to_exit;
            }); // Wake up and take lock

            // If queue is empty and need to quit, shut down thread
            if (q_context.need_to_exit && q_context.buffer_queue.empty()) {
                return;
            }

            // Otherwise, process the buffer until it is empty
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

        // For testing: Saving the 'in-memory' file to disk to validate file created correctly
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
            paste_text(res->body);
            break;

        default:
            std::cout << "Status code:\n";
            std::cout << res->status << "\n";
            std::cout << "Response body:\n";
            std::cout << res->body << "\n";
            break;
        }
    }
}
