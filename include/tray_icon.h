#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

enum class TrayState { Idle, Recording, Transcribing };

// Owns the notification-area icon of the app. All methods must be called on the thread that owns
// the window passed to the constructor.
class TrayIcon {
  HWND hwnd_;
  UINT callback_message_;
  HICON idle_icon_;
  HICON recording_icon_;
  HICON transcribing_icon_;
  TrayState state_ = TrayState::Idle;

  HICON icon_for(TrayState state) const;

public:
  // callback_message is the message Windows sends to hwnd for mouse events on the icon.
  TrayIcon(HWND hwnd, UINT callback_message);
  ~TrayIcon();
  TrayIcon(const TrayIcon&) = delete;
  TrayIcon& operator=(const TrayIcon&) = delete;

  // Adds the icon to the notification area. Also used to re-add it after Explorer restarts.
  bool add();
  void remove();
  void set_state(TrayState state);
  // Shows a balloon / toast notification anchored to the icon.
  void notify(const std::wstring& title, const std::wstring& message, bool is_error);
};
