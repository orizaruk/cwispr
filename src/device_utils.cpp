#include "device_utils.h"

#include <format>
#include <iostream>
#include <string>

#include "logger.h"

#include "miniaudio.h"
#define WIN32_LEAN_AND_MEAN
#include "miniaudio.h"
#include <windows.h>

int print_audio_devices() {
  ma_context context;
  if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) {
    // Error
    return 1;
  }

  ma_device_info* pPlaybackInfos;
  ma_uint32 playbackCount;
  ma_device_info* pCaptureInfos;
  ma_uint32 captureCount;
  if (ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos,
                             &captureCount) != MA_SUCCESS) {
    // Error
    return 1;
  }

  // Print playback devices`
  for (ma_uint32 iDevice = 0; iDevice < playbackCount; iDevice += 1) {
    std::cout << std::format("[{}] {}\n", iDevice, pPlaybackInfos[iDevice].name);
  }

  // Print capture devices
  for (ma_uint32 iDevice = 0; iDevice < captureCount; iDevice += 1) {
    std::cout << std::format("[{}] {}\n", iDevice, pCaptureInfos[iDevice].name);
  }

  ma_context_uninit(&context);

  return 0;
}

// This function converts a UTF-8 std::string to UTF-16 std::wstring, which is the required format
// for the Windows clipboard.
std::wstring ConvertToUTF16(const std::string& input_string) {
  if (input_string.empty())
    return std::wstring();

  int required_size = MultiByteToWideChar(CP_UTF8, 0, input_string.c_str(), -1, NULL, 0);

  if (required_size == 0) {
    GlobalLog->error("worker", "Failed to convert string to UTF16.");
    return std::wstring();
  }

  std::wstring output_string(required_size, 0); // Allocate the buffer for the converted string

  // Perform the conversion
  MultiByteToWideChar(CP_UTF8, 0, input_string.c_str(), -1, &output_string[0], required_size);
  output_string.pop_back();

  return output_string;
}

// Converts a wide string to normal one
std::string ConvertWideToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) {
    return std::string();
  }

  // 1. Ask Windows how much space we need for the normal string
  int size_needed =
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);

  // 2. Create a normal string with exactly that much space
  std::string result(size_needed, 0);

  // 3. Do the actual conversion, writing into our new string
  WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size_needed,
                      nullptr, nullptr);

  return result;
}

// Returns empty string if key was not found/error encountered, the value otherwise.
std::wstring getEnvVariable(const std::wstring& var_name) {
  wchar_t* pValue = nullptr;
  errno_t err = _wdupenv_s(&pValue, NULL,
                           var_name.c_str()); // It modifies the pValue pointer to point to the wide
                                              // string contents of the value of the env variable

  if (err != 0) {
    GlobalLog->warn("utils",
                    std::format("Error encountered searching for env variable {}.", ConvertWideToUtf8(var_name)));

    return std::wstring{};
  }

  // if pValue == nullptr, env variable not found
  if (pValue == nullptr) {
    GlobalLog->warn("utils", std::format("Env variable {} not found.", ConvertWideToUtf8(var_name)));
    return std::wstring{};
  }

  std::wstring result(pValue);
  free(pValue);
  return result;
}

std::string getCaptureDeviceName(ma_device& device) {
  char nameBuffer[MA_MAX_DEVICE_NAME_LENGTH + 1]{};
  if (ma_device_get_name(&device, ma_device_type_capture, nameBuffer, sizeof(nameBuffer),
                         nullptr) != MA_SUCCESS) {
    GlobalLog->error("utils", "Failed to get recording device name.");
    return {};
  }

  return nameBuffer;
}