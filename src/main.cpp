#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <vector>
#include <format>

#include "device_utils.h"
#include "worker.h"
#include "logger.h"

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
  // Create Logger object and make the global Logger pointer point to it. 
  Logger logger;
  GlobalLog = &logger;

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

  // Initialize recording device
  if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
    logger.error("main", "Failed to init recording device.");
    return -1;
  }

  // Start the device
  if (ma_device_start(&device) != MA_SUCCESS) {
    logger.error("main", "Failed to start recording device after initialization.");
    return -1;
  }
  
  // Spawn transcription thread
  std::thread transcription_thread(process_audio_queue, std::ref(queue_context));

  // Hotkey Logic
  bool prev_state = false;
  // States: Not recording, Recording, Finished recording

  
  logger.info(
    "main",
    "Finished initialization and starting of recording device. Starting Hotkey monitoring.");
  while (true) {
    // If ESC key detected, initiate shutdown
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
      logger.info("main", "ESC Key pressed, initiating shutdown.");
      {
        std::lock_guard<std::mutex> lock(queue_context.queue_mutex);
        queue_context.need_to_exit = true; // Modify flag for worker thread to close
      }
      queue_context.cv.notify_all();
      logger.info("main", "Flagged background thread to close through queue_context, notified via cv. Breaking the hotkey while loop.");
      break;
    }

    const bool is_pressing =
      (GetAsyncKeyState(VK_LCONTROL) & 0x8000) && (GetAsyncKeyState(VK_LWIN) & 0x8000);

    if (is_pressing && !prev_state) {
      recorder_context.is_recording = true;
      logger.info("main", "Starting recording.");
    } else if (!is_pressing && prev_state) {
      recorder_context.is_recording = false;
      logger.info("main", "Stopping recording.");
      // Grab mutex, transfer ownership of the vector the queue using move and unlock it.
      // Then, notify the CV.
      auto num_frames = recorder_context.audio_buffer.size();
      {
        std::unique_lock<std::mutex> lock(queue_context.queue_mutex); // Grab the lock
        queue_context.buffer_queue.push(
          std::move(recorder_context.audio_buffer)); // Move the buffer into the queue and reset it
      } // Lock will go out of scope and unlock
      queue_context.cv.notify_one(); // Notify the consumer
      logger.info("main", std::format("Moved audio buffer containing {} frames to the queue to be "
                                      "proceesed by background thread.",
                                      num_frames));
    }

    // Save current state into 'prev iteration state', sleep and move to the next iteration
    prev_state = is_pressing;
    Sleep(10);
  }

  // While loop broken, meaning shutdown was initiated. Background thread was notified it needs to process frames and close.
  logger.info("main", "Hotkey while loop broken.");
  
  ma_device_uninit(&device);
  logger.info("main", "Uninitialized recording device.");
  
  logger.info("main", "Waiting on background thread to close.");
  if (transcription_thread.joinable()) {
    transcription_thread.join();
  }

  logger.info("main", "Finished shutdown sequence. Closing the program.");
  return 0;
}
