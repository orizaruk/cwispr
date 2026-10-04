# cwispr

cwispr is a small native Windows push-to-talk speech-to-text injector written in C++.

It runs in the Windows notification area (system tray), records microphone input while a hotkey is held, sends the captured audio to a transcription API, and inserts the resulting text into the currently active application. Every transcription is also saved to a local history so it can be recovered when the paste does not land. The project is mostly an experiment in native Windows APIs, audio capture, in-memory audio formatting, and a simple producer-consumer worker pipeline.

## What it does

- Captures microphone input with miniaudio.
- Buffers raw 16-bit mono PCM samples at 16 kHz.
- Packages the captured samples as an in-memory WAV payload.
- Sends the WAV payload to Groq's OpenAI-compatible transcription endpoint.
- Converts the UTF-8 response to UTF-16.
- Temporarily uses the Windows clipboard and `SendInput` to paste the text into the active application.
- Restores the previous clipboard text when possible.
- Appends every result to a transcript history file.

## Controls

Run `cwispr.exe`. No window opens; a small dot appears in the notification area (you may need to expand the hidden icons with the `^` arrow and drag it onto the taskbar).

- Hold `Left Ctrl + Left Win` to record. Recordings shorter than 1.5 seconds are discarded.
- Release the keys to send the recorded audio for transcription.
- Click the tray icon (left or right click) for the menu:
  - **Copy last transcription** - puts the most recent transcription on the clipboard.
  - **Open transcripts folder**
  - **Open log file**
  - **Exit**

The tray icon shows the current state:

| Icon | State |
| --- | --- |
| Gray dot | Idle, waiting for the hotkey |
| Red dot | Recording |
| Amber dot | Waiting for the transcription |

A Windows notification is shown when a paste or a transcription request fails, and at startup if `GROQ_API_KEY` is not set.

Only one instance can run at a time.

The app uses the default capture device selected by Windows.

The hotkey press is detected with Raw Input (`WM_INPUT` messages), which only observes the keyboard and never delays it; the release is detected by polling the key state while recording. Like the paste itself, the hotkey may not work while an elevated (administrator) window has focus, unless cwispr is also run as administrator.

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

The output executable is named `cwispr.exe`. It is a GUI-subsystem app, so it does not open a console window.

To see the log output live while debugging, configure with `-DCWISPR_DEBUG_CONSOLE=ON`; the app then opens a console window next to the tray icon.

To start cwispr when you sign in, put a shortcut to `cwispr.exe` in the folder that opens with `Win + R` -> `shell:startup`.

## Transcript history

Every recording that is sent for transcription is recorded in:

```text
%LOCALAPPDATA%\cwispr\transcripts\YYYY-MM-DD.jsonl
```

There is one file per local day, and one JSON object per line (JSON Lines), appended as soon as the result is known. Non-ASCII text is stored as plain UTF-8, so the files are readable in any text editor. Example:

```json
{"time":"2026-10-04T14:32:05+0200","status":"pasted","audio_seconds":3.12,"model":"whisper-large-v3","text":" Hello there."}
{"time":"2026-10-04T14:35:41+0200","status":"api_error","audio_seconds":5.80,"model":"whisper-large-v3","text":"","error":"HTTP 401: ...","audio_file":"C:\\Users\\...\\transcripts\\audio\\2026-10-04_14-35-41.512.wav"}
```

Fields:

- `time` - local time the result was recorded, ISO 8601 with UTC offset.
- `status` - one of:
  - `pasted` - the text was put on the clipboard and Ctrl+V was sent. The target app may still have ignored it, so the text is kept here either way.
  - `paste_failed` - the clipboard or keyboard step failed. Use **Copy last transcription** or copy `text` from the file.
  - `empty` - the API returned no text.
  - `request_failed` - the request got no response (network or TLS error). See `error`.
  - `api_error` - the API answered with a non-200 status. See `error`.
- `audio_seconds` - length of the recording.
- `model` - transcription model used.
- `text` - the transcription, exactly as returned by the API.
- `error` - present for `request_failed` and `api_error`.
- `audio_file` - present when the request failed. The recording is saved as a WAV file under `transcripts\audio\` so it is not lost and can be transcribed again later.

## Logging

The logger writes to:

```text
%LOCALAPPDATA%\cwispr\log.txt
```

The directory is created automatically when the logger starts.

`Logger::write()` also prints every log line to `std::cout`, which is only visible in a `CWISPR_DEBUG_CONSOLE` build.

## Project layout

- `src/main.cpp` - `WinMain`, hidden window and message loop, raw keyboard input, recording state, tray menu, shutdown.
- `src/tray_icon.cpp` - notification-area icon, generated state icons, notifications.
- `src/worker.cpp` - worker thread, WAV payload construction, Groq request, text injection.
- `src/transcript_log.cpp` - transcript history files and saved audio of failed requests.
- `src/device_utils.cpp` - device names, environment variables, UTF-8/UTF-16 conversion helpers.
- `include/logger.h` - simple file logger.
- `extern/` - vendored single-header libraries.

## Notes

This is still a work-in-progress personal project, not a polished desktop app.
