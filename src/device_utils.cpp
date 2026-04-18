#include "device_utils.h"
#include "miniaudio.h"
#include <iostream>
#include <format>

int print_audio_devices()
{
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