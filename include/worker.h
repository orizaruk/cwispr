#pragma once

#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "device_utils.h"
#include "transcript_log.h"

// Called on the worker thread once per processed recording, after it was written to the
// transcript log.
using WorkerNotify = std::function<void(TranscriptStatus)>;

void process_audio_queue(QueueContext& q_context, TranscriptLog& transcripts,
                         WorkerNotify notify);

// Replaces the clipboard contents with in_text. hwnd becomes the clipboard owner.
bool modify_clipboard(HWND hwnd, std::wstring& in_text);
