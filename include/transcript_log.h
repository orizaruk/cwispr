#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

// Outcome of one recording that went through the worker.
enum class TranscriptStatus {
  Pasted,        // Transcribed and pasted into the active window.
  PasteFailed,   // Transcribed, but the clipboard/paste sequence failed.
  Empty,         // The API answered with an empty transcription.
  RequestFailed, // The HTTP request never got an answer (network, TLS, ...).
  ApiError,      // The API answered with a non-200 status.
};

std::string_view to_string(TranscriptStatus status);

struct TranscriptEntry {
  TranscriptStatus status = TranscriptStatus::Pasted;
  double audio_seconds = 0.0;
  std::string model;
  std::string text;       // UTF-8, exactly as returned by the API.
  std::string error;      // Set for RequestFailed / ApiError.
  std::string audio_file; // Set when the audio was kept on disk so it can be retried.
};

// Keeps a history of every transcription so text can be recovered when the paste did not land.
//
// Layout under the given directory:
//   YYYY-MM-DD.jsonl     one JSON object per line, one file per local day
//   audio/*.wav          audio of recordings whose transcription request failed
class TranscriptLog {
  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::string last_text_;

public:
  // Throws std::runtime_error if the directory cannot be created.
  explicit TranscriptLog(std::filesystem::path directory);

  const std::filesystem::path& directory() const {
    return directory_;
  }

  // Appends the entry to today's file. Thread-safe.
  void append(const TranscriptEntry& entry);

  // Writes the WAV bytes to audio/<timestamp>.wav and returns the path, or an empty path on
  // failure. Thread-safe.
  std::filesystem::path save_audio(const std::string& wav_bytes);

  // Text of the most recent non-empty transcription in this session, or "" if there is none.
  std::string last_text() const;
};
