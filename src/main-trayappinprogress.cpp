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
#include <shellapi.h>

constexpr int WM_TRAYICON = WM_USER + 1;
constexpr int HOTKEY_ID = 1;
constexpr int TIMER_ID_POLL_KEYS = 1001;

// --- GLOBAL APPLICATION STATE
HWND g_hwnd = nullptr;
QueueContext g_queue_context;
RecordingContext g_recorder_context;
ma_device g_device;
std::thread g_transcription_thread;

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

void SetupTrayIcon(HWND hwnd) {
    NOTIFYICONDATA nid = {sizeof(NOTIFYICONDATA)};
    nid.hWnd = hwnd;
    nid.uID = 1; // ID of icon
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage =
        WM_TRAYICON; // Tell Windows to send this message when the icon is clicked
    nid.hIcon =
        LoadIcon(NULL, IDI_APPLICATION); // Loads a default icon (you can add your own .ico later)
    strcpy_s(nid.szTip, "AI Transcription Tool"); // The hover tooltip

    Shell_NotifyIcon(NIM_ADD, &nid);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool is_recording = false;

    switch (msg) {

    case WM_HOTKEY: {
        if (wParam == 1) { // The ID we gave the Ctrl + Win hotkey
            // START/STOP RECORDING LOGIC GOES HERE
            if (!is_recording) {
                // Start Recording
                is_recording = true;
                std::cout << "Recording started...\n";

                // Start recording miniaudio here

                // Start a Timer that sends WM_TIMER message every 50ms for hold-to-record hotkey
                // logic
                SetTimer(hwnd, TIMER_ID_POLL_KEYS, 50, NULL); 
            }
        }
        break;
    }
    
    case WM_TIMER: {
        // Check if the message initiated from Hotkey Detection timer
        if (wParam == TIMER_ID_POLL_KEYS) {
            // Check if Ctrl or Win key were released and stop recording
            if (!(GetAsyncKeyState(VK_LCONTROL) & 0x8000) || !(GetAsyncKeyState(VK_LWIN) & 0x8000)) {
                is_recording = false;
                std::cout << "Stopping recording...\n";

                KillTimer(hwnd, TIMER_ID_POLL_KEYS); // Kill the timer
            }
        }
        break;
    }

    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            // User right-clicked the tray icon!
            // Here you would spawn a small menu (e.g., "Settings", "Exit")
        }
        break;
    }

    case WM_DESTROY: {
        // This is your Graceful Shutdown sequence!

        // 1. Remove the tray icon
        NOTIFYICONDATA nid = {sizeof(NOTIFYICONDATA)};
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIcon(NIM_DELETE, &nid);

        // 2. Unregister hotkeys
        UnregisterHotKey(hwnd, 1);

        // 3. Set your shutdown flag and cv.notify_one() here!
        // 4. join() your worker thread here!

        // Tell the Message Pump to exit
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 1. --- MINIAUDIO & THREAD INITIALIZATION --- 
    // Initialize the recorder device config
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = 16000;
    config.dataCallback = data_callback;
    config.pUserData = &g_recorder_context;

    // Initialize and start the recorder device 
    if (ma_device_init(nullptr, &config, &g_device) != MA_SUCCESS) {
        std::cerr << "CRITICAL ERROR: Failed to initialize the microphone!\n";
        std::cerr << "Press Enter to exit...\n";
        std::cin.get(); // Forces the console to stay open
        return -1;      // Failed to initialize the device
    }
    ma_device_start(&g_device);

    // Start the transcription thread
    std::thread transcription_thread(process_audio_queue, std::ref(g_queue_context));


    // 2. --- WIN32 UI INITIALIZATION ---
    WNDCLASSEX wc = {sizeof(WNDCLASSEX)};
    wc.lpfnWndProc = WndProc; // This points to the message handler function
    wc.hInstance = hInstance;
    wc.lpszClassName = "cwispr";
    RegisterClassEx(&wc);

    // Create the hidden window
    g_hwnd = CreateWindowEx(0, "MyTranscriptionApp", "Transcription Tool", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    // Setup the Tray Icon
    SetupTrayIcon(g_hwnd);

    // Register Global Hotkey
    // ID 1 (HOTKEY_ID): Ctrl + Win
    RegisterHotKey(g_hwnd, HOTKEY_ID, MOD_CONTROL | MOD_WIN, 0); // 0 means no extra standard key, just modifiers


    // 3. --- THE MESSAGE PUMP ---
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        // GetMessage puts the thread to sleep until Windows sends us an event
        TranslateMessage(&msg);
        DispatchMessage(&msg); // Sends the event to WndProc
    }

    return 0;
}