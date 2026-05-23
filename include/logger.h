#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

enum class LogLevel { Debug, Info, Warn, Error };

namespace fs = std::filesystem;

inline std::string to_string(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO ";
  case LogLevel::Warn:
    return "WARN ";
  case LogLevel::Error:
    return "ERROR";
  }
  return "UNKNOWN";
}

// This function reads the env variable LOCALAPPDATA and creates a cwispr folder in it.
// If succeeds, returns the path object. Otherwise, returns empty path.
inline fs::path create_appdata_folder() {
  std::error_code ec;
  bool created = false;
  wchar_t* pValue = nullptr;
  errno_t err = _wdupenv_s(&pValue, NULL, L"LOCALAPPDATA");

  if (err != 0) {
    std::wcerr << L"Error searching for the env variable.\n";
    return fs::path{};
  }

  // if pValue == nullptr, env variable not found
  if (pValue == nullptr) {
    std::cerr << "Env variable LOCALAPPDATA does not exist.\n";
    return fs::path{};
  }

  fs::path root(pValue); // save the value of env variable into path object
  free(pValue);          // free pointer after use
  pValue = nullptr;

  fs::path folder("cwispr");
  fs::path full_path = root / folder; // construct the path

  created = fs::create_directories(full_path, ec);
  if (ec) {
    std::cerr << "Failed to create the directories needed.\n";
    return fs::path{};
  }

  return full_path;
}

// Returns a formatted string of the current time YYYY-MM-DD HH-MM-SS
inline std::string current_timestamp() {
  auto now = std::chrono::system_clock::now();
  auto local_time = std::chrono::zoned_time{std::chrono::current_zone(), now};
  return std::format("{:%Y-%m-%d %H:%M:%S}", local_time);
}

class Logger {
  std::ofstream file_;
  std::mutex mutex_;

public:
  Logger() {
    // 1. Open Directories
    fs::path path_created = create_appdata_folder();
    if (path_created.empty()) {
      // Failed to create directories
      throw std::runtime_error("Failed to open the directories for the log file.");
    }

    // 2. open file in the created repo
    fs::path file_name{"log.txt"};
    fs::path full_path = path_created / file_name;
    file_.open(full_path, std::ios::app);
    if (!file_.is_open()) {
      throw std::runtime_error("Failed to open log file.");
    }
  }

  void debug(std::string_view component, std::string_view message) {
    write(LogLevel::Debug, component, message);
  }

  void info(std::string_view component, std::string_view message) {
    write(LogLevel::Info, component, message);
  }

  void warn(std::string_view component, std::string_view message) {
    write(LogLevel::Warn, component, message);
  }

  void error(std::string_view component, std::string_view message) {
    write(LogLevel::Error, component, message);
  }

private:
  // write to log file in format of [timestamp] [log_level] [component] [message]
  void write(LogLevel level, std::string_view component, std::string_view message) {
    std::string log_message = std::format("[{}] [{}] [{}] [{}]\n", current_timestamp(),
                                          to_string(level), component, message);
    std::cout << log_message << "\n";
    {
      std::lock_guard<std::mutex> lock(mutex_); // lock for writing to the file
      file_ << log_message;
      if (level == LogLevel::Error) {
        file_.flush();
      }
    }
  }
};

// Create global Logger pointer to be used across all translation units.
// Will be initialized in main.cpp and point to the shared Logger object.
inline Logger* GlobalLog = nullptr;