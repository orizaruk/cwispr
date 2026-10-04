#include "transcript_log.h"

#include <chrono>
#include <format>
#include <fstream>
#include <stdexcept>

#include "device_utils.h"
#include "logger.h"

namespace {
// Escapes a UTF-8 string for use inside a JSON string literal. Non-ASCII bytes are kept as-is so
// the file stays readable in a text editor.
std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char c : input) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
      } else {
        out += c;
      }
    }
  }
  return out;
}

std::string path_to_utf8(const std::filesystem::path& path) {
  return ConvertWideToUtf8(path.wstring());
}

auto local_now() {
  return std::chrono::zoned_time{
    std::chrono::current_zone(),
    std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())};
}
} // namespace

std::string_view to_string(TranscriptStatus status) {
  switch (status) {
  case TranscriptStatus::Pasted:
    return "pasted";
  case TranscriptStatus::PasteFailed:
    return "paste_failed";
  case TranscriptStatus::Empty:
    return "empty";
  case TranscriptStatus::RequestFailed:
    return "request_failed";
  case TranscriptStatus::ApiError:
    return "api_error";
  }
  return "unknown";
}

TranscriptLog::TranscriptLog(std::filesystem::path directory) : directory_(std::move(directory)) {
  std::error_code ec;
  std::filesystem::create_directories(directory_, ec);
  if (ec) {
    throw std::runtime_error(
      std::format("Failed to create transcript directory: {}", ec.message()));
  }
}

void TranscriptLog::append(const TranscriptEntry& entry) {
  const auto now = local_now();
  const auto now_seconds = std::chrono::zoned_time{
    now.get_time_zone(), std::chrono::floor<std::chrono::seconds>(now.get_sys_time())};

  std::string line = std::format(R"({{"time":"{:%Y-%m-%dT%H:%M:%S%z}","status":"{}",)"
                                 R"("audio_seconds":{:.2f},"model":"{}","text":"{}")",
                                 now_seconds, to_string(entry.status), entry.audio_seconds,
                                 json_escape(entry.model), json_escape(entry.text));
  if (!entry.error.empty()) {
    line += std::format(R"(,"error":"{}")", json_escape(entry.error));
  }
  if (!entry.audio_file.empty()) {
    line += std::format(R"(,"audio_file":"{}")", json_escape(entry.audio_file));
  }
  line += "}\n";

  const auto file_path = directory_ / std::format("{:%Y-%m-%d}.jsonl", now);

  std::lock_guard<std::mutex> lock(mutex_);
  if (!entry.text.empty()) {
    last_text_ = entry.text;
  }

  // Open/append/close on every entry so each record is on disk even if the app dies right after.
  std::ofstream file(file_path, std::ios::app | std::ios::binary);
  if (!file) {
    GlobalLog->error("transcripts",
                     std::format("Failed to open {} for appending.", path_to_utf8(file_path)));
    return;
  }
  file << line;
  if (!file.flush()) {
    GlobalLog->error("transcripts", std::format("Failed to write to {}.", path_to_utf8(file_path)));
  }
}

std::filesystem::path TranscriptLog::save_audio(const std::string& wav_bytes) {
  const auto audio_dir = directory_ / "audio";
  const auto file_path = audio_dir / std::format("{:%Y-%m-%d_%H-%M-%S}.wav", local_now());

  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(audio_dir, ec);
  if (ec) {
    GlobalLog->error("transcripts",
                     std::format("Failed to create {}: {}", path_to_utf8(audio_dir), ec.message()));
    return {};
  }

  std::ofstream file(file_path, std::ios::binary);
  if (!file || !file.write(wav_bytes.data(), wav_bytes.size())) {
    GlobalLog->error("transcripts",
                     std::format("Failed to write audio to {}.", path_to_utf8(file_path)));
    return {};
  }
  return file_path;
}

std::string TranscriptLog::last_text() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_text_;
}
