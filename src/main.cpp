#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "device_utils.h"
#include "logger.h"
#include "transcript_log.h"
#include "tray_icon.h"
#include "worker.h"

#include "miniaudio.h"

#define WIN32_LEAN_AND_MEAN
#include <shellapi.h>
#include <windows.h>

namespace {
// Messages sent to the hidden window.
constexpr UINT WM_APP_TRAY = WM_APP + 1;            // Mouse events on the tray icon
constexpr UINT WM_APP_HOTKEY_PRESSED = WM_APP + 2;  // Posted by the keyboard hook
constexpr UINT WM_APP_TRANSCRIPT_DONE = WM_APP + 3; // Posted by the worker, wParam = status

constexpr UINT_PTR TIMER_ID_RELEASE_POLL = 1;
constexpr UINT RELEASE_POLL_INTERVAL_MS = 20;

constexpr double MINIMUM_RECORDING_SECONDS = 1.5;

enum MenuId : UINT {
  ID_MENU_COPY_LAST = 1,
  ID_MENU_OPEN_TRANSCRIPTS,
  ID_MENU_OPEN_LOG,
  ID_MENU_EXIT,
};

// --- GLOBAL APPLICATION STATE
// Only touched from the UI thread, except where noted. The keyboard hook and window procedure
// have no user-data parameter, so the state lives here.
struct App {
  HWND hwnd = nullptr;
  HHOOK keyboard_hook = nullptr;
  UINT taskbar_created_msg = 0;
  ma_device device;
  RecordingContext recorder; // Shared with the miniaudio callback thread
  QueueContext queue;        // Shared with the worker thread
  TranscriptLog* transcripts = nullptr;
  std::unique_ptr<TrayIcon> tray;
  int pending_jobs = 0; // Recordings handed to the worker that have not reported back yet
};

App g_app;

void data_callback(ma_device* p_device, void* p_output, const void* p_input,
                   ma_uint32 frame_count) {
  const auto p_context = static_cast<RecordingContext*>(p_device->pUserData);

  if (!p_context->is_recording) {
    return; // Drop the audio and exit
  }

  const auto p_samples = static_cast<const int16_t*>(p_input);
  std::lock_guard<std::mutex> lock(p_context->buffer_mutex);
  if (!p_context->is_recording) {
    return; // Recording stopped while we were waiting for the lock
  }
  // Insert the frames into vector in the recording context
  auto& buffer = p_context->audio_buffer;
  buffer.insert(buffer.end(), p_samples, p_samples + frame_count);
}

bool is_key_down(int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

bool is_hotkey_held() {
  return is_key_down(VK_LCONTROL) && is_key_down(VK_LWIN);
}

// Low-level keyboard hook, runs on the UI thread (through its message loop) for every key event
// in the session. It must return quickly, so it only posts a message to the window.
// RegisterHotKey cannot be used because it does not support a modifier-only combination.
LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM w_param, LPARAM l_param) {
  if (code == HC_ACTION && (w_param == WM_KEYDOWN || w_param == WM_SYSKEYDOWN)) {
    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(l_param);
    // Ignore synthetic input, including the Ctrl+V the worker sends when pasting.
    if ((key->flags & LLKHF_INJECTED) == 0) {
      // The async state of the key in this event is not updated yet, so only check the other one.
      const bool hotkey_pressed = (key->vkCode == VK_LCONTROL && is_key_down(VK_LWIN)) ||
                                  (key->vkCode == VK_LWIN && is_key_down(VK_LCONTROL));
      if (hotkey_pressed) {
        PostMessageW(g_app.hwnd, WM_APP_HOTKEY_PRESSED, 0, 0);
      }
    }
  }
  return CallNextHookEx(nullptr, code, w_param, l_param);
}

void update_tray_state() {
  if (!g_app.tray) {
    return;
  }
  if (g_app.recorder.is_recording) {
    g_app.tray->set_state(TrayState::Recording);
  } else if (g_app.pending_jobs > 0) {
    g_app.tray->set_state(TrayState::Transcribing);
  } else {
    g_app.tray->set_state(TrayState::Idle);
  }
}

void start_recording() {
  if (g_app.recorder.is_recording) {
    return; // Key auto-repeat keeps posting the hotkey message while it is held
  }

  {
    std::lock_guard<std::mutex> lock(g_app.recorder.buffer_mutex);
    g_app.recorder.audio_buffer.clear();
    g_app.recorder.is_recording = true;
  }
  // Poll for the release instead of waiting for the key-up in the hook: the hook does not see
  // key events while an elevated window has focus, and a missed key-up would keep recording.
  SetTimer(g_app.hwnd, TIMER_ID_RELEASE_POLL, RELEASE_POLL_INTERVAL_MS, nullptr);
  GlobalLog->info("main", "Starting recording.");
  update_tray_state();
}

void stop_recording() {
  KillTimer(g_app.hwnd, TIMER_ID_RELEASE_POLL);

  std::vector<int16_t> captured;
  {
    std::lock_guard<std::mutex> lock(g_app.recorder.buffer_mutex);
    g_app.recorder.is_recording = false;
    captured.swap(g_app.recorder.audio_buffer);
  }
  GlobalLog->info("main", "Stopping recording.");

  const auto num_frames = captured.size();
  const double duration_secs = static_cast<double>(num_frames) / g_app.device.sampleRate;

  // Check for minimum duration and discard mis-recordings
  if (duration_secs < MINIMUM_RECORDING_SECONDS) {
    GlobalLog->info("main", std::format("Discarding {:.2f}s recording, shorter than {:.2f}s.",
                                        duration_secs, MINIMUM_RECORDING_SECONDS));
  } else {
    {
      std::lock_guard<std::mutex> lock(g_app.queue.queue_mutex);
      g_app.queue.buffer_queue.push(std::move(captured));
    }
    g_app.queue.cv.notify_one(); // Notify the consumer
    ++g_app.pending_jobs;
    GlobalLog->info("main",
                    std::format("Moved audio buffer containing {} frames to the queue to be "
                                "processed by background thread.",
                                num_frames));
  }
  update_tray_state();
}

void on_transcript_done(TranscriptStatus status) {
  if (g_app.pending_jobs > 0) {
    --g_app.pending_jobs;
  }
  update_tray_state();

  switch (status) {
  case TranscriptStatus::PasteFailed:
    g_app.tray->notify(L"cwispr: paste failed",
                       L"The transcription was saved. Use \"Copy last transcription\" in the "
                       L"tray menu to get it.",
                       true);
    break;
  case TranscriptStatus::RequestFailed:
  case TranscriptStatus::ApiError:
    g_app.tray->notify(L"cwispr: transcription failed",
                       L"The recording was kept in the transcripts\\audio folder. See the log "
                       L"file for details.",
                       true);
    break;
  case TranscriptStatus::Pasted:
  case TranscriptStatus::Empty:
    break;
  }
}

void open_in_shell(const std::filesystem::path& path) {
  const auto result = reinterpret_cast<INT_PTR>(
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    GlobalLog->error("main", std::format("Failed to open {}.", ConvertWideToUtf8(path.wstring())));
  }
}

void copy_last_transcription(HWND hwnd) {
  std::wstring text = ConvertToUTF16(g_app.transcripts->last_text());
  if (text.empty()) {
    return;
  }
  if (!modify_clipboard(hwnd, text)) {
    g_app.tray->notify(L"cwispr", L"Could not copy the transcription to the clipboard.", true);
  }
}

void show_tray_menu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }

  const bool has_last_text = !g_app.transcripts->last_text().empty();
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"cwispr - hold Left Ctrl + Left Win to dictate");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (has_last_text ? 0 : MF_GRAYED), ID_MENU_COPY_LAST,
              L"Copy last transcription");
  AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_TRANSCRIPTS, L"Open transcripts folder");
  AppendMenuW(menu, MF_STRING, ID_MENU_OPEN_LOG, L"Open log file");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");

  POINT cursor;
  GetCursorPos(&cursor);
  // Without these two calls the menu does not close when the user clicks elsewhere.
  SetForegroundWindow(hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hwnd, nullptr);
  PostMessageW(hwnd, WM_NULL, 0, 0);

  DestroyMenu(menu);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
  // Explorer was restarted and the notification area is empty again
  if (msg == g_app.taskbar_created_msg && msg != 0) {
    if (g_app.tray) {
      g_app.tray->add();
    }
    return 0;
  }

  switch (msg) {
  case WM_APP_HOTKEY_PRESSED:
    start_recording();
    return 0;

  case WM_TIMER:
    if (w_param == TIMER_ID_RELEASE_POLL && !is_hotkey_held()) {
      stop_recording();
    }
    return 0;

  case WM_APP_TRANSCRIPT_DONE:
    on_transcript_done(static_cast<TranscriptStatus>(w_param));
    return 0;

  case WM_APP_TRAY:
    if (LOWORD(l_param) == WM_LBUTTONUP || LOWORD(l_param) == WM_RBUTTONUP) {
      show_tray_menu(hwnd);
    }
    return 0;

  case WM_COMMAND:
    switch (LOWORD(w_param)) {
    case ID_MENU_COPY_LAST:
      copy_last_transcription(hwnd);
      break;
    case ID_MENU_OPEN_TRANSCRIPTS:
      open_in_shell(g_app.transcripts->directory());
      break;
    case ID_MENU_OPEN_LOG:
      open_in_shell(GlobalLog->path());
      break;
    case ID_MENU_EXIT:
      GlobalLog->info("main", "Exit selected from the tray menu, initiating shutdown.");
      DestroyWindow(hwnd);
      break;
    }
    return 0;

  case WM_DESTROY:
    if (g_app.recorder.is_recording) {
      KillTimer(hwnd, TIMER_ID_RELEASE_POLL);
      g_app.recorder.is_recording = false; // Drop the recording in progress
    }
    if (g_app.tray) {
      g_app.tray->remove();
    }
    PostQuitMessage(0); // Tell the message loop to exit
    return 0;
  }
  return DefWindowProcW(hwnd, msg, w_param, l_param);
}

void show_error(const std::wstring& message) {
  MessageBoxW(nullptr, message.c_str(), L"cwispr", MB_OK | MB_ICONERROR);
}

int run(HINSTANCE instance) {
  // Only one instance may run: two keyboard hooks would record and paste everything twice.
  HANDLE instance_mutex = CreateMutexW(nullptr, TRUE, L"Local\\cwispr-single-instance");
  if (instance_mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    MessageBoxW(nullptr, L"cwispr is already running. Look for its icon in the notification area.",
                L"cwispr", MB_OK | MB_ICONINFORMATION);
    CloseHandle(instance_mutex);
    return 0;
  }

  // 1. --- TRANSCRIPT HISTORY ---
  TranscriptLog transcripts(create_appdata_folder() / "transcripts");
  g_app.transcripts = &transcripts;
  GlobalLog->info("main", std::format("Saving transcripts to {}.",
                                      ConvertWideToUtf8(transcripts.directory().wstring())));

  // 2. --- WIN32 UI INITIALIZATION ---
  WNDCLASSEXW wc = {sizeof(wc)};
  wc.lpfnWndProc = window_proc; // This points to the message handler function
  wc.hInstance = instance;
  wc.lpszClassName = L"cwispr-tray-window";
  if (RegisterClassExW(&wc) == 0) {
    GlobalLog->error("main", "Failed to register the window class.");
    show_error(L"Failed to register the window class.");
    return -1;
  }

  // A hidden top-level window rather than a message-only one: message-only windows do not get
  // the TaskbarCreated broadcast, and the tray menu needs a window that can be foreground.
  g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"cwispr", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                               nullptr, instance, nullptr);
  if (g_app.hwnd == nullptr) {
    GlobalLog->error("main", "Failed to create the hidden window.");
    show_error(L"Failed to create the hidden window.");
    return -1;
  }
  g_app.taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");

  // Setup the Tray Icon. If Explorer is not up yet (e.g. started at logon), it is added on
  // TaskbarCreated instead.
  g_app.tray = std::make_unique<TrayIcon>(g_app.hwnd, WM_APP_TRAY);
  g_app.tray->add();

  // Install the hotkey hook. It must be installed by the thread that runs the message loop.
  g_app.keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook_proc, instance, 0);
  if (g_app.keyboard_hook == nullptr) {
    GlobalLog->error("main", "Failed to install the keyboard hook.");
    show_error(L"Failed to install the keyboard hook.");
    DestroyWindow(g_app.hwnd);
    return -1;
  }

  // 3. --- MINIAUDIO & THREAD INITIALIZATION ---
  // Initialize the recorder device config
  ma_device_config config = ma_device_config_init(ma_device_type_capture);
  config.capture.format = ma_format_s16;
  config.capture.channels = 1;
  config.sampleRate = 16000;
  config.dataCallback = data_callback;
  config.pUserData = &g_app.recorder;

  // Initialize recording device
  if (ma_device_init(nullptr, &config, &g_app.device) != MA_SUCCESS) {
    GlobalLog->error("main", "Failed to init recording device.");
    show_error(L"Failed to initialize the microphone.");
    UnhookWindowsHookEx(g_app.keyboard_hook);
    DestroyWindow(g_app.hwnd);
    return -1;
  }

  // Log the device name
  std::string recording_device_name = getCaptureDeviceName(g_app.device);
  GlobalLog->info("main", std::format("Initialized recording device: {}", recording_device_name));

  // Start the device
  if (ma_device_start(&g_app.device) != MA_SUCCESS) {
    GlobalLog->error("main", "Failed to start recording device after initialization.");
    show_error(L"Failed to start the microphone.");
    ma_device_uninit(&g_app.device);
    UnhookWindowsHookEx(g_app.keyboard_hook);
    DestroyWindow(g_app.hwnd);
    return -1;
  }

  // Spawn transcription thread. It reports each finished recording back to the UI thread.
  const HWND hwnd = g_app.hwnd;
  std::thread transcription_thread(process_audio_queue, std::ref(g_app.queue),
                                   std::ref(transcripts), [hwnd](TranscriptStatus status) {
                                     PostMessageW(hwnd, WM_APP_TRANSCRIPT_DONE,
                                                  static_cast<WPARAM>(status), 0);
                                   });

  if (getEnvVariable(L"GROQ_API_KEY").empty()) {
    g_app.tray->notify(L"cwispr: GROQ_API_KEY is not set",
                       L"Transcription requests will fail until the GROQ_API_KEY environment "
                       L"variable is set and cwispr is restarted.",
                       true);
  }

  GlobalLog->info("main", "Finished initialization. Entering the message loop.");

  // 4. --- THE MESSAGE PUMP ---
  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // GetMessage puts the thread to sleep until Windows sends us an event
    TranslateMessage(&msg);
    DispatchMessageW(&msg); // Sends the event to window_proc
  }

  // 5. --- SHUTDOWN ---
  // The window is destroyed and the tray icon removed (WM_DESTROY).
  GlobalLog->info("main", "Message loop exited.");

  UnhookWindowsHookEx(g_app.keyboard_hook);

  ma_device_uninit(&g_app.device);
  GlobalLog->info("main", "Uninitialized recording device.");

  {
    std::lock_guard<std::mutex> lock(g_app.queue.queue_mutex);
    g_app.queue.need_to_exit = true; // Modify flag for worker thread to close
  }
  g_app.queue.cv.notify_all();
  GlobalLog->info("main", "Waiting on background thread to finish queued recordings and close.");
  if (transcription_thread.joinable()) {
    transcription_thread.join();
  }

  g_app.tray.reset();
  g_app.transcripts = nullptr;
  CloseHandle(instance_mutex);

  GlobalLog->info("main", "Finished shutdown sequence. Closing the program.");
  return 0;
}
} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
#ifdef CWISPR_DEBUG_CONSOLE
  // A GUI-subsystem app has no console; open one so the log lines are visible while debugging.
  if (AllocConsole()) {
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
  }
#endif

  // Lets the tray icon be drawn at the display's real resolution instead of being upscaled.
  SetProcessDPIAware();

  try {
    // Create Logger object and make the global Logger pointer point to it.
    Logger logger;
    GlobalLog = &logger;

    try {
      const int exit_code = run(hInstance);
      GlobalLog = nullptr;
      return exit_code;
    } catch (const std::exception& e) {
      logger.error("main", std::format("Unhandled exception: {}", e.what()));
      GlobalLog = nullptr;
      show_error(ConvertToUTF16(std::format("cwispr stopped because of an error:\n{}", e.what())));
      return -1;
    }
  } catch (const std::exception& e) {
    // The logger itself could not be created
    show_error(ConvertToUTF16(std::format("cwispr failed to start:\n{}", e.what())));
    return -1;
  }
}
