# cwispr

cwispr is a small native Windows push-to-talk speech-to-text injector written in C++.

It records microphone input while a hotkey is held, sends the captured audio to a transcription API, and inserts the resulting text into the currently active application. The project is mostly an experiment in native Windows APIs, audio capture, in-memory audio formatting, and a simple producer-consumer worker pipeline.

## What it does

- Captures microphone input with miniaudio.
- Buffers raw 16-bit mono PCM samples at 16 kHz.
- Packages the captured samples as an in-memory WAV payload.
- Sends the WAV payload to Groq's OpenAI-compatible transcription endpoint.
- Converts the UTF-8 response to UTF-16.
- Temporarily uses the Windows clipboard and `SendInput` to paste the text into the active application.
- Restores the previous clipboard text when possible.

The current executable is a console/background style app. There is an in-progress tray version in `src/main-trayappinprogress.cpp`, but it is not part of the active CMake target.

## Controls

Run the executable, then:

- Hold `Left Ctrl + Left Win` to record.
- Release the keys to send the recorded audio for transcription.
- Press `Esc` to shut the app down.

The app uses the default capture device selected by Windows.

## Requirements

- Windows
- CMake 3.20 or newer
- A C++20 compiler, currently tested around MSVC
- OpenSSL development libraries
- A Groq API key

The project vendors these single-header dependencies under `extern/`:

- `miniaudio.h`
- `httplib.h`

`cpp-httplib` is built with OpenSSL support, so OpenSSL still needs to be available to CMake.

If you use vcpkg, installing OpenSSL looks like:

```powershell
vcpkg install openssl:x64-windows
```

## API key

cwispr reads the Groq API key from the `GROQ_API_KEY` environment variable.

## Transcription model

The transcription model is currently selected in `src/worker.cpp` inside `process_audio_queue()`:

```cpp
std::string model_name = "whisper-large-v3";
```

Change that string to use a different Groq-supported transcription model.

## Building

The repository includes a CMake preset named `windows-vcpkg`.

```powershell
cmake --preset windows-vcpkg
cmake --build out/build/windows-vcpkg
```

The preset currently points `CMAKE_TOOLCHAIN_FILE` at a local vcpkg path. If your vcpkg checkout is somewhere else, update `CMakePresets.json` or configure manually:

```powershell
cmake -S . -B out/build/windows-vcpkg -G Ninja -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build out/build/windows-vcpkg
```

The output executable is named `cwispr.exe`.

## Logging

The logger writes to:

```text
%LOCALAPPDATA%\cwispr\log.txt
```

The directory is created automatically when the logger starts.

At the moment, `Logger::write()` also prints every log line to `std::cout`. If you want file-only logging, modify `include/logger.h` in the `Logger::write()` method and remove or guard this line:

```cpp
std::cout << log_message << "\n";
```

## Project layout

- `src/main.cpp` - application startup, audio device setup, hotkey polling, recording state, shutdown.
- `src/worker.cpp` - worker thread, WAV payload construction, Groq request, text injection.
- `src/device_utils.cpp` - device names, environment variables, UTF-8/UTF-16 conversion helpers.
- `include/logger.h` - simple file logger.
- `extern/` - vendored single-header libraries.

## Notes

This is still a work-in-progress personal project, not a polished desktop app.
