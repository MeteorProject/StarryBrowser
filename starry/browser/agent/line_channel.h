// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_LINE_CHANNEL_H_
#define STARRY_BROWSER_AGENT_LINE_CHANNEL_H_

#include <string>

#include "base/files/platform_file.h"
#include "base/functional/callback.h"
#include "base/threading/sequence_bound.h"

// 一行一条消息的通道：从一个描述符按行读，往另一个描述符写（自动加换行）。读写都在后台
// 序列上，回调在建它的序列上（UI 线程）。
// 供 MCP 本机连接和智能体子进程的标准输入、输出和错误管道使用。
namespace starry::agent {

class LineChannel {
 public:
  // 一行最长 32 MB（截图的 base64 也远小于这个），超过就断开，免得对方把内存撑满。
  static constexpr size_t kMaxLineLength = 32u << 20;

  // read_fd 读到头（对方关了）或者出错时调用 on_closed，之后不再有 on_line。
  // write_fd 可以是空的（只读，比如子进程的标准错误）。
  LineChannel(base::ScopedPlatformFile read_fd,
              base::ScopedPlatformFile write_fd,
              base::RepeatingCallback<void(std::string)> on_line,
              base::OnceClosure on_closed);
  LineChannel(const LineChannel&) = delete;
  LineChannel& operator=(const LineChannel&) = delete;
  // 两个描述符都关掉。
  ~LineChannel();

  void Send(std::string line);
  // 写完已经排着的，再关掉写的一端（子进程读到标准输入的结尾）。
  void CloseWrite();

 private:
  class Core;
  base::SequenceBound<Core> core_;
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_LINE_CHANNEL_H_
