#pragma once

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <functional>

// 前向声明
class DevLog;
class MemoryCompressor;

// 用户输入上下文历史记录类
// 按「窗口」分开维护最近提交的文本（原文，保留标点），用于 LLM 预测候选词：
// - 不同应用/窗口/分页各自一份，避免上下文互相污染
// - 某窗口闲置超过 idle 时间后，其旧上下文不再使用
// - 每份超过 max_size 段时，异步将最旧的 (max_size/2) 段通过记忆 LLM 压缩（或直接丢弃）
// 所有读写都作用在当前窗口（SetActiveKey 设置）上。
class ContextHistory {
 public:
  // max_size：每个窗口最多保留的提交段数（每次提交的文本为一段）
  explicit ContextHistory(size_t max_size = 200);

  ~ContextHistory();

  // 切换当前窗口（key 由调用方生成，例如 应用名|窗口句柄|标题）；不存在则新建，
  // 窗口数超过上限时淘汰最久未使用的
  void SetActiveKey(const std::wstring& key, DevLog* dev_console = nullptr);
  const std::wstring& GetActiveKey() const { return m_active_key; }

  // 闲置多久（毫秒）后该窗口的旧上下文失效；0 = 永不失效
  void SetIdleTimeout(unsigned long long idle_ms) { m_idle_ms = idle_ms; }

  // 添加用户提交的文本到当前窗口的历史（保留原文与标点）
  // dev_console: 开发终端实例，用于输出日志（可为nullptr）
  void AddText(const std::wstring& text, DevLog* dev_console = nullptr);

  // 取当前窗口最近的上下文：各段原文直接相连，只保留最后 max_chars 个字（0 = 不截断）
  std::wstring GetRecentContext(size_t max_chars) const;

  // 获取当前窗口的所有历史记录
  std::vector<std::wstring> GetAllHistory() const;

  // 清空当前窗口的历史记录
  // dev_console: 开发终端实例，用于输出日志（可为nullptr）
  void Clear(DevLog* dev_console = nullptr);

  // 获取当前窗口的记录段数
  size_t GetSize() const;

  // 获取每个窗口的最大段数
  size_t GetMaxSize() const;

  // 设置记忆压缩器（从 weasel 配置 llm/memory/ 加载），用于超过容量时压缩旧文本
  void SetMemoryCompressor(MemoryCompressor* compressor) { m_memory_compressor = compressor; }
  // 设置压缩完成回调（压缩成功替换历史后触发）
  void SetCompressionCompletedCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_compression_completed_callback = std::move(callback);
  }

 private:
  struct Bucket {
    std::vector<std::wstring> segments;  // 线性：最旧在 0，最新在 back
    unsigned long long last_used = 0;    // base::MonotonicMs
    bool compressing = false;            // 是否正在压缩，避免重复触发
  };

  // 取当前窗口的 bucket（锁内调用）；闲置过期时清空
  Bucket* ActiveBucketLocked();
  const Bucket* ActiveBucketLocked() const;
  bool IsExpiredLocked(const Bucket& b) const;

  // 当 size >= max_size 时尝试异步压缩最旧的 (max_size/2) 段（不阻塞）
  void TryTriggerCompression(const std::wstring& key, DevLog* dev_console);

  // 每次压缩取最旧的段数（max_size 的一半）
  size_t GetCompressWordCount() const { return m_max_size / 2; }

  // 用压缩后的内容替换指定窗口最旧的段
  void ReplaceOldestWithCompressed(const std::wstring& key,
                                   const std::vector<std::wstring>& compressed,
                                   DevLog* dev_console);

  static const size_t kMaxBuckets = 20;  // 最多同时记住多少个窗口

  mutable std::mutex m_mutex;  // 线程安全锁
  std::map<std::wstring, Bucket> m_buckets;
  std::wstring m_active_key;
  size_t m_max_size;  // 每个窗口的最大段数
  unsigned long long m_idle_ms = 10ull * 60 * 1000;
  MemoryCompressor* m_memory_compressor;  // 记忆压缩 LLM（可为 nullptr）
  std::function<void()> m_compression_completed_callback;
};
