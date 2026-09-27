#pragma once

// 按鍵：Rime 用的 X11 keysym 與修飾鍵（Windows 的 Weasel 與 mac 的 Squirrel 都轉成這一套）
namespace ime {

namespace key {
constexpr int kVoidSymbol = 0xFFFFFF;
constexpr int kTab = 0xFF09;
constexpr int kReturn = 0xFF0D;
constexpr int kEscape = 0xFF1B;
constexpr int kBackSpace = 0xFF08;
constexpr int kDelete = 0xFFFF;
constexpr int kHome = 0xFF50;
constexpr int kLeft = 0xFF51;
constexpr int kUp = 0xFF52;
constexpr int kRight = 0xFF53;
constexpr int kDown = 0xFF54;
constexpr int kPageUp = 0xFF55;
constexpr int kPageDown = 0xFF56;
constexpr int kEnd = 0xFF57;
constexpr int kKPEnter = 0xFF8D;
constexpr int kShiftL = 0xFFE1;
constexpr int kShiftR = 0xFFE2;
constexpr int kGrave = 0x060;
constexpr int kBracketLeft = 0x05B;
}  // namespace key

namespace mod {
constexpr int kShift = 1 << 0;
constexpr int kControl = 1 << 2;
constexpr int kAlt = 1 << 3;
constexpr int kSuper = 1 << 26;
constexpr int kRelease = 1 << 30;
}  // namespace mod

}  // namespace ime
