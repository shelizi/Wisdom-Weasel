#include "stdafx.h"
#include "ContextHistory.h"
#include "MemoryCompressor.h"
#include "DevConsole.h"
#include <algorithm>
#include <sstream>
#include <thread>

ContextHistory::ContextHistory(size_t max_size)
    : m_max_size(max_size > 0 ? max_size : 200),
      m_memory_compressor(nullptr) {}

ContextHistory::~ContextHistory() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_buckets.clear();
}

bool ContextHistory::IsExpiredLocked(const Bucket& b) const {
  return m_idle_ms > 0 && !b.segments.empty() && b.last_used > 0 &&
         GetTickCount64() - b.last_used > m_idle_ms;
}

ContextHistory::Bucket* ContextHistory::ActiveBucketLocked() {
  Bucket& b = m_buckets[m_active_key];
  if (IsExpiredLocked(b)) {
    b.segments.clear();
    b.compressing = false;
  }
  return &b;
}

const ContextHistory::Bucket* ContextHistory::ActiveBucketLocked() const {
  auto it = m_buckets.find(m_active_key);
  if (it == m_buckets.end() || IsExpiredLocked(it->second))
    return nullptr;
  return &it->second;
}

void ContextHistory::SetActiveKey(const std::wstring& key, DevConsole* dev_console) {
  bool switched = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (key == m_active_key)
      return;
    m_active_key = key;
    m_buckets[key];  // 确保存在
    switched = true;
    // 超过上限时淘汰最久未使用的窗口（当前窗口除外）
    while (m_buckets.size() > kMaxBuckets) {
      auto oldest = m_buckets.end();
      for (auto it = m_buckets.begin(); it != m_buckets.end(); ++it) {
        if (it->first == m_active_key || it->second.compressing)
          continue;
        if (oldest == m_buckets.end() || it->second.last_used < oldest->second.last_used)
          oldest = it;
      }
      if (oldest == m_buckets.end())
        break;
      m_buckets.erase(oldest);
    }
  }
  if (switched && dev_console && dev_console->IsEnabled()) {
    dev_console->WriteLine(L"[上下文] 切换窗口: " + key + L" | 该窗口已有 " +
                           std::to_wstring(GetSize()) + L" 段");
  }
}

void ContextHistory::AddText(const std::wstring& text, DevConsole* dev_console) {
  if (text.empty()) {
    return;
  }

  size_t current_size = 0;
  bool should_trigger_compression = false;
  std::wstring key;

  {
    std::lock_guard<std::mutex> lock(m_mutex);
    Bucket* b = ActiveBucketLocked();  // 闲置过期的旧内容会在这里被清掉
    key = m_active_key;
    b->segments.push_back(text);
    b->last_used = GetTickCount64();
    if (!m_memory_compressor || !m_memory_compressor->IsAvailable()) {
      size_t batch = GetCompressWordCount();
      if (batch == 0) batch = 1;
      while (b->segments.size() > m_max_size) {
        size_t erase_count = (std::min)(batch, b->segments.size());
        b->segments.erase(b->segments.begin(), b->segments.begin() + erase_count);
      }
    } else {
      while (b->segments.size() > m_max_size) {
        b->segments.erase(b->segments.begin());
      }
    }
    current_size = b->segments.size();
    if (current_size >= m_max_size && m_memory_compressor &&
        m_memory_compressor->IsAvailable() && !b->compressing &&
        current_size >= GetCompressWordCount()) {
      should_trigger_compression = true;
    }
  }

  if (dev_console && dev_console->IsEnabled()) {
    std::wstringstream ss;
    ss << L"[上下文更新] 添加文本: " << text << L" | 当前窗口段数: " << current_size;
    dev_console->WriteLine(ss.str());
    std::wstring recent = GetRecentContext(30);
    if (!recent.empty()) dev_console->WriteLine(L"  最近30字: " + recent);
  }

  if (should_trigger_compression) {
    TryTriggerCompression(key, dev_console);
  }
}

std::wstring ContextHistory::GetRecentContext(size_t max_chars) const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const Bucket* b = ActiveBucketLocked();
  if (!b || b->segments.empty()) return L"";
  // 从最新的段往回取，够 max_chars 个字就停
  std::wstring result;
  for (auto it = b->segments.rbegin(); it != b->segments.rend(); ++it) {
    result.insert(0, *it);
    if (max_chars > 0 && result.size() >= max_chars)
      break;
  }
  if (max_chars > 0 && result.size() > max_chars)
    result.erase(0, result.size() - max_chars);
  return result;
}

std::vector<std::wstring> ContextHistory::GetAllHistory() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const Bucket* b = ActiveBucketLocked();
  return b ? b->segments : std::vector<std::wstring>();
}

void ContextHistory::Clear(DevConsole* dev_console) {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    Bucket& b = m_buckets[m_active_key];
    b.segments.clear();
    b.compressing = false;
  }
  if (dev_console && dev_console->IsEnabled()) {
    dev_console->WriteLine(L"[上下文更新] 当前窗口的历史记录已清空");
  }
}

size_t ContextHistory::GetSize() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  const Bucket* b = ActiveBucketLocked();
  return b ? b->segments.size() : 0;
}

size_t ContextHistory::GetMaxSize() const {
  return m_max_size;
}

void ContextHistory::TryTriggerCompression(const std::wstring& key, DevConsole* dev_console) {
  size_t compress_count = GetCompressWordCount();
  std::vector<std::wstring> to_compress;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_buckets.find(key);
    if (it == m_buckets.end())
      return;
    Bucket& b = it->second;
    if (b.compressing || b.segments.size() < compress_count ||
        !m_memory_compressor || !m_memory_compressor->IsAvailable()) {
      return;
    }
    to_compress.assign(b.segments.begin(), b.segments.begin() + compress_count);
    b.compressing = true;
  }
  if (dev_console && dev_console->IsEnabled()) {
    dev_console->WriteLine(L"[记忆压缩] 异步压缩最旧 " +
                           std::to_wstring(compress_count) + L" 段");
  }
  m_memory_compressor->CompressAsync(to_compress, [this, key, dev_console](
      std::vector<std::wstring> compressed) {
    if (compressed.empty()) {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_buckets.find(key);
      if (it != m_buckets.end())
        it->second.compressing = false;
      if (dev_console && dev_console->IsEnabled()) {
        dev_console->WriteLine(L"[记忆压缩] 压缩失败或返回为空，保留原文");
      }
      return;
    }
    ReplaceOldestWithCompressed(key, compressed, dev_console);
  });
}

void ContextHistory::ReplaceOldestWithCompressed(
    const std::wstring& key,
    const std::vector<std::wstring>& compressed,
    DevConsole* dev_console) {
  std::function<void()> on_compression_completed;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_buckets.find(key);
    if (it == m_buckets.end())
      return;  // 窗口已被淘汰
    Bucket& b = it->second;
    b.compressing = false;
    size_t compress_count = GetCompressWordCount();
    if (b.segments.size() < compress_count) return;
    b.segments.erase(b.segments.begin(), b.segments.begin() + compress_count);
    b.segments.insert(b.segments.begin(), compressed.begin(), compressed.end());
    while (b.segments.size() > m_max_size) {
      b.segments.erase(b.segments.begin());
    }
    if (dev_console && dev_console->IsEnabled()) {
      std::wstringstream ss;
      ss << L"[记忆压缩] 完成，压缩为 " << compressed.size() << L" 段，当前窗口: "
         << b.segments.size();
      dev_console->WriteLine(ss.str());
    }
    on_compression_completed = m_compression_completed_callback;
  }
  if (on_compression_completed) {
    // 在压缩回调线程中异步触发后续预热，不影响当前历史更新路径。
    std::thread([on_compression_completed]() {
      on_compression_completed();
    }).detach();
  }
}
