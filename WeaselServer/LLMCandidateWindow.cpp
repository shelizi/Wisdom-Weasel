#include "stdafx.h"
#include "LLMCandidateWindow.h"
#include <sstream>

LLMCandidateWindow::LLMCandidateWindow()
    : m_shown(false),
      m_hFont(NULL),
      m_hBackgroundBrush(NULL),
      m_hBorderPen(NULL) {
  m_size.cx = 0;
  m_size.cy = 0;
  m_position = {0, 0, 0, 0};
}

LLMCandidateWindow::~LLMCandidateWindow() {
  Destroy();
}

bool LLMCandidateWindow::Create(HWND parent) {
  if (IsWindow())
    return true;

  // 创建窗口
  HWND hWnd = CWindowImpl<LLMCandidateWindow>::Create(
      parent, 0, 0, WS_POPUP,
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, 0U,
      0);

  if (!hWnd)
    return false;

  // 创建字体
  LOGFONT lf = {0};
  lf.lfHeight = -16;  // 16像素高
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
  lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
  lf.lfQuality = DEFAULT_QUALITY;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
  m_hFont = CreateFontIndirect(&lf);

  // 创建背景画刷（浅灰色背景）
  m_hBackgroundBrush = CreateSolidBrush(RGB(240, 240, 240));

  // 创建边框画笔（深灰色边框）
  m_hBorderPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));

  return true;
}

void LLMCandidateWindow::Destroy() {
  if (m_hFont) {
    DeleteObject(m_hFont);
    m_hFont = NULL;
  }
  if (m_hBackgroundBrush) {
    DeleteObject(m_hBackgroundBrush);
    m_hBackgroundBrush = NULL;
  }
  if (m_hBorderPen) {
    DeleteObject(m_hBorderPen);
    m_hBorderPen = NULL;
  }
  if (IsWindow()) {
    DestroyWindow();
  }
}

void LLMCandidateWindow::Show() {
  if (!IsWindow())
    return;

  if (m_candidates.empty()) {
    Hide();
    return;
  }

  CalculateSize();
  SetWindowPos(NULL, m_position.left, m_position.top, m_size.cx, m_size.cy,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  m_shown = true;
  Invalidate();
}

void LLMCandidateWindow::Hide() {
  if (!IsWindow())
    return;
  ShowWindow(SW_HIDE);
  m_shown = false;
}

void LLMCandidateWindow::SetCandidates(
    const std::vector<std::wstring>& candidates) {
  m_candidates = candidates;
  CalculateSize();
  if (m_shown && IsWindow()) {
    SetWindowPos(NULL, 0, 0, m_size.cx, m_size.cy,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Invalidate();
  }
}

void LLMCandidateWindow::SetPosition(RECT const& rc) {
  m_position = rc;
  // 计算窗口位置（在输入位置下方）
  if (m_position.bottom > 0) {
    m_position.top = m_position.bottom + 5;  // 在输入位置下方5像素
    m_position.bottom = m_position.top + m_size.cy;
    m_position.right = m_position.left + m_size.cx;
  }
  if (m_shown && IsWindow()) {
    SetWindowPos(NULL, m_position.left, m_position.top, m_size.cx, m_size.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void LLMCandidateWindow::Update() {
  if (m_shown && IsWindow()) {
    Invalidate();
  }
}

void LLMCandidateWindow::CalculateSize() {
  if (m_candidates.empty()) {
    m_size.cx = 0;
    m_size.cy = 0;
    return;
  }

  HDC hdc = GetDC();
  if (!hdc) {
    m_size.cx = 200;
    m_size.cy = 30 * (int)m_candidates.size() + 20;
    return;
  }

  HFONT hOldFont = (HFONT)SelectObject(hdc, m_hFont);

  int maxWidth = 0;
  int lineHeight = 24;
  int padding = 10;

  for (const auto& candidate : m_candidates) {
    SIZE textSize = {0, 0};
    GetTextExtentPoint32(hdc, candidate.c_str(), (int)candidate.length(),
                         &textSize);
    int width = textSize.cx + 40;  // 加上标签和边距
    if (width > maxWidth)
      maxWidth = width;
  }

  SelectObject(hdc, hOldFont);
  ReleaseDC(hdc);

  m_size.cx = maxWidth + padding * 2;
  m_size.cy = lineHeight * (int)m_candidates.size() + padding * 2;

  // 设置窗口位置（在输入位置下方）
  if (m_position.bottom > 0) {
    m_position.left = m_position.left;
    m_position.top = m_position.bottom + 5;  // 在输入位置下方5像素
  }
}

LRESULT LLMCandidateWindow::OnCreate(UINT uMsg,
                                     WPARAM wParam,
                                     LPARAM lParam,
                                     BOOL& bHandled) {
  bHandled = TRUE;
  return 0;
}

LRESULT LLMCandidateWindow::OnDestroy(UINT uMsg,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      BOOL& bHandled) {
  bHandled = TRUE;
  return 0;
}

LRESULT LLMCandidateWindow::OnPaint(UINT uMsg,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    BOOL& bHandled) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(&ps);

  if (hdc) {
    DrawCandidates(hdc);
  }

  EndPaint(&ps);
  bHandled = TRUE;
  return 0;
}

LRESULT LLMCandidateWindow::OnEraseBkgnd(UINT uMsg,
                                         WPARAM wParam,
                                         LPARAM lParam,
                                         BOOL& bHandled) {
  bHandled = TRUE;
  return 1;  // 已处理，不需要擦除背景
}

void LLMCandidateWindow::DrawCandidates(HDC hdc) {
  if (m_candidates.empty())
    return;

  RECT clientRect;
  GetClientRect(&clientRect);

  // 绘制背景
  FillRect(hdc, &clientRect, m_hBackgroundBrush);

  // 绘制边框
  HPEN hOldPen = (HPEN)SelectObject(hdc, m_hBorderPen);
  Rectangle(hdc, clientRect.left, clientRect.top, clientRect.right,
            clientRect.bottom);
  SelectObject(hdc, hOldPen);

  // 设置字体
  HFONT hOldFont = (HFONT)SelectObject(hdc, m_hFont);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(0, 0, 0));

  // 绘制候选词
  int y = 10;
  int lineHeight = 24;

  for (size_t i = 0; i < m_candidates.size(); ++i) {
    std::wstringstream ss;
    ss << (i + 1) << L". " << m_candidates[i];
    std::wstring text = ss.str();

    TextOut(hdc, 10, y, text.c_str(), (int)text.length());
    y += lineHeight;
  }

  SelectObject(hdc, hOldFont);
}
