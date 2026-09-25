#pragma once
#include <unordered_map>
#include <shellscalingapi.h>

// Per-monitor DPI support for dialog-template based dialogs.
//
// Windows lays out a dialog template at the *system* DPI, even for per-monitor
// aware processes. So the only scaling needed is from the system DPI to the DPI
// of the monitor the dialog is on (e.g. none at all on a single 200% display).
// Scaling from 96 instead would enlarge everything twice on high-DPI systems.
template <typename T>
class CDialogDpiAware : public CDialogImpl<T> {
 public:
  CDialogDpiAware() {}
  ~CDialogDpiAware() {
    if (m_currentFont)
      DeleteObject(m_currentFont);
  }

 protected:
  BEGIN_MSG_MAP(T)
  MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
  END_MSG_MAP()

  void InitCtrlRects() {
    m_baseDpi = GetSystemDpi();
    ::GetWindowRect(m_hWnd, &m_rect);
    // Only direct children: grandchildren (e.g. a list view's header) are
    // positioned by their own parent.
    for (HWND child = ::GetWindow(m_hWnd, GW_CHILD); child;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
      RECT rect;
      ::GetWindowRect(child, &rect);
      ::MapWindowPoints(HWND_DESKTOP, m_hWnd, (LPPOINT)&rect, 2);
      m_controlOriginalRects[child] = rect;
    }
    HFONT font = (HFONT)::SendMessage(m_hWnd, WM_GETFONT, 0, 0);
    m_hasBaseFont = font && ::GetObject(font, sizeof(m_baseFont), &m_baseFont);

    m_currentDpi = m_baseDpi;
    const UINT newDpi = GetWindowDpi();
    if (newDpi != m_baseDpi) {
      const float scaleFactor = (float)newDpi / (float)m_baseDpi;
      const int width = (int)((m_rect.right - m_rect.left) * scaleFactor);
      const int height = (int)((m_rect.bottom - m_rect.top) * scaleFactor);
      SetWindowPos(nullptr, m_rect.left, m_rect.top, width, height,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
      ScaleControlsAndFonts(newDpi);
      Invalidate();
    }
    m_currentDpi = newDpi;
  }

 private:
  static UINT GetSystemDpi() {
    HDC hdc = ::GetDC(NULL);
    const int dpi = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc)
      ::ReleaseDC(NULL, hdc);
    return dpi > 0 ? (UINT)dpi : 96;
  }

  UINT GetWindowDpi() {
    HMONITOR hMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(
            GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
      if (dpiX == 0)
        dpiX = 96;
      return dpiX;
    }
    return 96;
  }

  void ScaleControlsAndFonts(UINT newDpi) {
    // Always scale from the original layout and font, so repeated monitor
    // changes do not compound rounding or font growth.
    const float scaleFactor = static_cast<float>(newDpi) / m_baseDpi;
    for (const auto& [hWnd, originalRect] : m_controlOriginalRects) {
      int newX = static_cast<int>(originalRect.left * scaleFactor);
      int newY = static_cast<int>(originalRect.top * scaleFactor);
      int newWidth = static_cast<int>((originalRect.right - originalRect.left) *
                                      scaleFactor);
      int newHeight = static_cast<int>(
          (originalRect.bottom - originalRect.top) * scaleFactor);
      ::SetWindowPos(hWnd, nullptr, newX, newY, newWidth, newHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!m_hasBaseFont)
      return;
    LOGFONT lf = m_baseFont;
    lf.lfHeight = static_cast<int>(lf.lfHeight * scaleFactor);
    HFONT hNewFont = CreateFontIndirect(&lf);
    ::SendMessage(m_hWnd, WM_SETFONT, (WPARAM)hNewFont, TRUE);
    for (const auto& [hWnd, rect] : m_controlOriginalRects) {
      ::SendMessage(hWnd, WM_SETFONT, (WPARAM)hNewFont, TRUE);
    }
    if (m_currentFont) {
      DeleteObject(m_currentFont);
    }
    m_currentFont = hNewFont;
  }

  LRESULT OnDpiChanged(UINT, WPARAM wParam, LPARAM lParam, BOOL&) {
    UINT newDpi = HIWORD(wParam);
    if (newDpi == m_currentDpi)
      return 0;
    const RECT* pNewRect = reinterpret_cast<const RECT*>(lParam);
    // 直接使用系统建议的矩形
    SetWindowPos(nullptr, pNewRect->left, pNewRect->top,
                 pNewRect->right - pNewRect->left,
                 pNewRect->bottom - pNewRect->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ScaleControlsAndFonts(newDpi);
    m_currentDpi = newDpi;
    Invalidate();
    return 0;
  }

  UINT m_baseDpi = 96;
  UINT m_currentDpi = 96;
  RECT m_rect;
  std::unordered_map<HWND, RECT> m_controlOriginalRects;
  LOGFONT m_baseFont = {};
  bool m_hasBaseFont = false;
  HFONT m_currentFont = nullptr;
};
