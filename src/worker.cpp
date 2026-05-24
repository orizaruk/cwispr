#include "worker.h"

#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "device_utils.h"
#include "logger.h"

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

// This function backs up the contents of the clipboard if it is in CF_UNICODETEXT format into
// out_text variable.
bool backup_clipboard_text(HWND hwnd, std::wstring& out_text) {
  if (!OpenClipboard(hwnd)) {
    GlobalLog->error("clipboard_backup", "Failed to open clipboard.");
    return false;
  }

  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    CloseClipboard();
    GlobalLog->warn("clipboard_backup",
                    "Clipboard content is not CF_UNICODETEXT, cannot back it up.");
    return false;
  }

  HANDLE hClipboardData = GetClipboardData(CF_UNICODETEXT);
  if (hClipboardData == nullptr) {
    CloseClipboard();
    GlobalLog->warn("clipboard_backup",
                    "Failed to GetClipboardData, although clipboard contents are CF_UNICODETEXT.");
    return false;
  }

  // GlobalLock() asks Windows to return the pointer to the clipboard data through the HANDLE it
  // gave us
  auto pClipboardData = static_cast<wchar_t*>(GlobalLock(hClipboardData));
  if (pClipboardData == nullptr) {
    CloseClipboard();
    GlobalLog->warn("clipboard_backup", "Windows failed to give us pointer to the clipboard data "
                                        "after calling GlobalLock on the HANDLE.");
    return false;
  }

  // Save the clipboard contents from the pointer Windows gave us, unlock the handle, close the
  // clipboard and return true
  out_text = pClipboardData;
  GlobalUnlock(hClipboardData);
  CloseClipboard();
  GlobalLog->info("clipboard_backup",
                  "Succesfully saved clipboard contents for back up, returning true.");
  return true;
}

// This function changes the contents of the clipboard to the contents of in_text.
bool modify_clipboard(HWND hwnd, std::wstring& in_text) {
  // Open Clipboard
  if (!OpenClipboard(hwnd)) {
    GlobalLog->error("clipboard_modify", "Failed to open clipboard.");
    return false;
  }

  // Empty it - if function returns 0, it failed
  if (!EmptyClipboard()) {
    CloseClipboard();
    GlobalLog->error("clipboard_modify", "Failed to empty clipboard.");
    return false;
  }

  // Create the Handle that will hold the string to paste
  size_t bytes_needed_for_clipboard = (in_text.size() + 1) * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes_needed_for_clipboard);

  if (hMem == nullptr) {
    CloseClipboard();
    GlobalLog->error("clipboard_modify", "Failed to allocate memory for the handle.");
    return false; // Allocation failed
  }

  // Lock the handle
  void* pMem = GlobalLock(hMem);

  if (pMem == nullptr) {
    GlobalFree(hMem);
    CloseClipboard();
    GlobalLog->error("clipboard_modify", "Failed to lock the handle.");
    return false;
  }

  // Write the string to the handle
  memcpy(pMem, in_text.c_str(), bytes_needed_for_clipboard);

  // Unlock the handle, we finished
  GlobalUnlock(hMem);

  // Replace the clipboard contents with the text
  if (SetClipboardData(CF_UNICODETEXT, hMem) == nullptr) {
    // Windows failed with modifying clipboard content, we need to free the handle since we
    // are still owners of it
    GlobalFree(hMem);
    CloseClipboard();
    GlobalLog->error("clipboard_modify", "Windows failed to modify the clipboard contents.");
    return false;
  }

  CloseClipboard();
  GlobalLog->info("clipboard_modify",
                  "Succesfully replaced clipboard contents with transcribed text.");
  return true;
}

void simulate_paste_keypress() {
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
    GlobalLog->error("simulate_paste_keypress", "Failed to send all the inputs.");
  }
}

void paste_text(const std::string& text_to_paste) {
  if (text_to_paste.empty()) {
    GlobalLog->warn("paste_text", "Empty string fed into paste_text function.");
    return;
  }

  GlobalLog->info("paste_text", "Starting text pasting sequence.");
  HWND hwnd = CreateWindowEx(0, "STATIC", "DummyWindow", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL,
                             NULL); // Initialize hidden window

  if (hwnd == NULL) {
    GlobalLog->error("paste_text", "Failed to create invisible Windows window.");
    return;
  }

  // 1. Copy contents of clipboard if it's text, mark the flag as true to know to restore it at
  // the end
  std::wstring saved_clipboard_text;
  bool clipboard_needs_restore_after_paste = backup_clipboard_text(hwnd, saved_clipboard_text);

  // Convert from UTF-8 to UTF-16
  std::wstring transcription_text = ConvertToUTF16(text_to_paste);
  if (transcription_text.empty()) {
    DestroyWindow(hwnd);
    GlobalLog->error("paste_text", "UTF8 to UTF16 conversion failed.");
    return;
  }

  // 2. Modify clipboard contents to the transcription text
  if (!modify_clipboard(hwnd, transcription_text)) {
    DestroyWindow(hwnd);
    GlobalLog->error("paste_text", "Failed to modify contents of clipboard.");
    return;
  }

  // 3. Simulate Ctrl+V in order to paste clipboard contents
  simulate_paste_keypress();
  Sleep(50); // Added sleep to allow the application to process the keyboard inputs before we modify
             // clipboard contents

  // 4. Restore clipboard contents to previous state
  if (clipboard_needs_restore_after_paste) {
    if (!modify_clipboard(hwnd, saved_clipboard_text)) {
      GlobalLog->error("paste_text", "Failed to restore contents of clipboard.");
    }
  }

  // 5. Destroy the window created for the Handle to avoid leak
  DestroyWindow(hwnd);
}

void process_audio_queue(QueueContext& q_context) {
  GlobalLog->info("worker", "Worker launched, waiting for audio.");
  std::string model_name = "whisper-large-v3";

  auto cli = std::make_unique<httplib::SSLClient>("api.groq.com");
  cli->set_bearer_token_auth(ConvertWideToUtf8(getEnvVariable(L"GROQ_API_KEY")));

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
    
    GlobalLog->info(
      "worker", std::format("Processing audio buffer containing {} frames.", local_audio.size()));
    // Create .WAV file

    // a. In the disk:
    /*if (create_wav_file(local_audio) != MA_SUCCESS) {
        std::cout << "Failed to create .wav file.\n";
    }*/

    // b. In memory:
    GlobalLog->info("worker", "Creating .WAV file in memory.");
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

    GlobalLog->info("worker", ".WAV file created succesfully.");

    // For testing: Saving the 'in-memory' file to disk to validate file created correctly
    // save_memory_to_disk(memory_file, "test.wav");

    // Create the multipart form data
    GlobalLog->info("worker", "Creating multipart form to send in request.");
    httplib::UploadFormDataItems items = {{"file", memory_file, "audio.wav", "audio/wav"},
                                          {"model", model_name, "", ""},
                                          {"response_format", "text", "", ""}};
    // Send the request
    auto res = cli->Post("/openai/v1/audio/transcriptions", items);
    GlobalLog->info("worker", "Sent HTTP request.");

    if (!res) {
      auto err = res.error();
      GlobalLog->error("worker", httplib::to_string(err));
      if (err == httplib::Error::SSLConnection) {
        GlobalLog->error("worker", std::format("SSL error code: {}", res.ssl_error()));
        GlobalLog->error("worker", std::format("Backend error: {}", res.ssl_backend_error()));
      }
      continue;
    }

    switch (res->status) {
    case httplib::StatusCode::OK_200:
      GlobalLog->info("worker", "Request returned code 200, calling paste_text function.");
      paste_text(res->body);
      break;

    default:
      GlobalLog->info("worker", std::format("Request returned code {}, response body: {}",
                                            res->status, res->body));
      break;
    }
  }
}
