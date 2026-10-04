cmake_minimum_required(VERSION 3.20)

project(cwispr)

find_package(OpenSSL REQUIRED)

# Specify C++ 20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opens a console window next to the tray icon that shows the log output live.
option(CWISPR_DEBUG_CONSOLE "Open a console window for log output" OFF)

# Define the executable (the name of the app and the files it needs).
# WIN32 makes it a GUI-subsystem app (WinMain entry point, no console window).
add_executable(cwispr WIN32
  "src/main.cpp"
  "src/miniaudio.cpp"
  "src/device_utils.cpp"
  "src/worker.cpp"
  "src/transcript_log.cpp"
  "src/tray_icon.cpp"
)

target_include_directories(cwispr PRIVATE include extern)
target_compile_definitions(cwispr PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT NOMINMAX)
if(CWISPR_DEBUG_CONSOLE)
  target_compile_definitions(cwispr PRIVATE CWISPR_DEBUG_CONSOLE)
endif()

target_link_libraries(cwispr PRIVATE OpenSSL::SSL OpenSSL::Crypto user32 gdi32 shell32)
