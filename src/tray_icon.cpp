#include "tray_icon.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "logger.h"

namespace {
constexpr UINT TRAY_ICON_ID = 1;

// Draws a filled, anti-aliased circle with a darker 1px outline so it reads on both light and dark
// taskbars. Generated at runtime so the app needs no .ico resource.
HICON create_dot_icon(BYTE r, BYTE g, BYTE b) {
  const int size = GetSystemMetrics(SM_CXSMICON);

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size; // Top-down rows
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC screen_dc = GetDC(nullptr);
  HBITMAP color_bitmap = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen_dc);
  if (color_bitmap == nullptr) {
    return nullptr;
  }

  const double center = size / 2.0;
  const double radius = size * 0.4;
  auto* pixels = static_cast<uint32_t*>(bits);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const double distance = std::hypot(x + 0.5 - center, y + 0.5 - center);
      const double outer = std::clamp(radius + 0.5 - distance, 0.0, 1.0);
      const double inner = std::clamp(radius - 0.5 - distance, 0.0, 1.0);
      // Blend from the outline colour (60% brightness) to the fill colour.
      const double shade = 0.6 + 0.4 * inner;
      const auto channel = [shade](BYTE value) { return static_cast<uint32_t>(value * shade); };
      const auto alpha = static_cast<uint32_t>(outer * 255.0);
      pixels[y * size + x] = (alpha << 24) | (channel(r) << 16) | (channel(g) << 8) | channel(b);
    }
  }

  // The AND mask is ignored for 32bpp icons with alpha, but CreateIconIndirect still needs one.
  // Monochrome rows are WORD aligned.
  std::vector<BYTE> mask_bits(((size + 15) / 16) * 2 * size, 0);
  HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, mask_bits.data());

  ICONINFO icon_info = {};
  icon_info.fIcon = TRUE;
  icon_info.hbmColor = color_bitmap;
  icon_info.hbmMask = mask_bitmap;
  HICON icon = CreateIconIndirect(&icon_info);

  DeleteObject(color_bitmap);
  DeleteObject(mask_bitmap);
  return icon;
}

template <size_t N> void copy_truncated(wchar_t (&dest)[N], const std::wstring& src) {
  const size_t count = std::min(src.size(), N - 1);
  std::copy_n(src.data(), count, dest);
  dest[count] = L'\0';
}

const wchar_t* tooltip_for(TrayState state) {
  switch (state) {
  case TrayState::Recording:
    return L"cwispr - recording...";
  case TrayState::Transcribing:
    return L"cwispr - transcribing...";
  case TrayState::Idle:
    break;
  }
  return L"cwispr - hold Left Ctrl + Left Win to dictate";
}
} // namespace

TrayIcon::TrayIcon(HWND hwnd, UINT callback_message)
    : hwnd_(hwnd), callback_message_(callback_message),
      idle_icon_(create_dot_icon(0x9A, 0xA0, 0xA6)),
      recording_icon_(create_dot_icon(0xE5, 0x39, 0x35)),
      transcribing_icon_(create_dot_icon(0xFB, 0xC0, 0x2D)) {
}

TrayIcon::~TrayIcon() {
  remove();
  for (HICON icon : {idle_icon_, recording_icon_, transcribing_icon_}) {
    if (icon != nullptr) {
      DestroyIcon(icon);
    }
  }
}

HICON TrayIcon::icon_for(TrayState state) const {
  HICON icon = idle_icon_;
  if (state == TrayState::Recording) {
    icon = recording_icon_;
  } else if (state == TrayState::Transcribing) {
    icon = transcribing_icon_;
  }
  return icon != nullptr ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

bool TrayIcon::add() {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = TRAY_ICON_ID;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = callback_message_;
  nid.hIcon = icon_for(state_);
  copy_truncated(nid.szTip, tooltip_for(state_));

  if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
    GlobalLog->error("tray", "Failed to add the tray icon.");
    return false;
  }
  return true;
}

void TrayIcon::remove() {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = TRAY_ICON_ID;
  Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayIcon::set_state(TrayState state) {
  if (state == state_) {
    return;
  }
  state_ = state;

  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = TRAY_ICON_ID;
  nid.uFlags = NIF_ICON | NIF_TIP;
  nid.hIcon = icon_for(state);
  copy_truncated(nid.szTip, tooltip_for(state));
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::notify(const std::wstring& title, const std::wstring& message, bool is_error) {
  NOTIFYICONDATAW nid = {sizeof(nid)};
  nid.hWnd = hwnd_;
  nid.uID = TRAY_ICON_ID;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = is_error ? NIIF_WARNING : NIIF_INFO;
  copy_truncated(nid.szInfoTitle, title);
  copy_truncated(nid.szInfo, message);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}
