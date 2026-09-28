#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <atlbase.h>
#include <atlwin.h>

// 简单的LLM候选词显示窗口
class LLMCandidateWindow
    : public CWindowImpl<LLMCandidateWindow,
                         CWindow,
                         CWinTraits<WS_POPUP | WS_CLIPSIBLINGS,
                                    WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
                                        WS_EX_NOACTIVATE | WS_EX_LAYERED>> {
 public:
  DECLARE_WND_CLASS(L"LLMCandidateWindow")

  BEGIN_MSG_MAP(LLMCandidateWindow)
  MESSAGE_HANDLER(WM_CREATE, OnCreate)
  MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  MESSAGE_HANDLER(WM_PAINT, OnPaint)
  MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
  END_MSG_MAP()

  LLMCandidateWindow();
  ~LLMCandidateWindow();

  bool Create(HWND parent);
  void Destroy();
  void Show();
  void Hide();
  bool IsShown() const { return m_shown; }

  void SetCandidates(const std::vector<std::wstring>& candidates);
  void SetPosition(RECT const& rc);
  void Update();

 private:
  LRESULT OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);
  LRESULT OnEraseBkgnd(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled);

  void DrawCandidates(HDC hdc);
  void CalculateSize();

  std::vector<std::wstring> m_candidates;
  RECT m_position;
  SIZE m_size;
  bool m_shown;
  HFONT m_hFont;
  HBRUSH m_hBackgroundBrush;
  HPEN m_hBorderPen;
};
