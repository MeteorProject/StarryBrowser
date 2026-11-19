// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ASSISTANT_LINE_PROCESS_H_
#define STARRY_BROWSER_ASSISTANT_LINE_PROCESS_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/platform_file.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/timer/timer.h"

namespace starry::agent {
class LineChannel;
}

// 按行通信的子进程封装：标准输出一行一条消息地读，标准输入一行一条地写（Claude Code
// 的 stream-json、Codex app-server 的 JSON-RPC）。回调都在 UI 线程。标准错误
// 只留最后 4 KB，进程异常退出时给界面看原因。
namespace starry::assistant {

class LineProcess {
 public:
  struct Options {
    Options();
    Options(const Options&);
    Options& operator=(const Options&);
    ~Options();

    base::FilePath executable;
    std::vector<std::string> arguments;
    // 子进程的全部环境变量（不继承浏览器的）。
    base::EnvironmentMap environment;
    base::FilePath directory;
  };

  // on_exit：退出码和标准错误的最后一段。标准输出读到头、进程也退出了才调用（最后几行
  // 输出不会丢）；子进程的子进程还开着输出管道时，退出后最多再等 1 秒。
  LineProcess(base::RepeatingCallback<void(std::string)> on_line,
              base::OnceCallback<void(int, std::string)> on_exit);
  LineProcess(const LineProcess&) = delete;
  LineProcess& operator=(const LineProcess&) = delete;
  // 还在运行就结束它。
  ~LineProcess();

  // 在后台线程上启动。started 的参数是起不来的原因，空的是成功。
  void Start(Options options, base::OnceCallback<void(std::string)> started);
  bool running() const { return process_.IsValid() && !exited_; }

  // 写一行（自动加换行）。还在启动时排着，起来了再写；退出了就什么也不做。
  void Write(std::string line);
  // 关掉标准输入：stream-json 的 Claude Code 读到结尾就退出。
  void CloseInput();
  // 协议中断超时后的处理：POSIX 发 SIGINT，Windows 写完已排队的请求后关闭
  // stdin。
  void Interrupt();
  // 结束子进程。Windows 一并关闭此次启动的 Job。
  void Terminate();

 private:
  struct Launched {
    Launched();
    ~Launched();
    base::Process process;
    base::ScopedPlatformFile input;
    base::ScopedPlatformFile output;
    base::ScopedPlatformFile errors;
#if BUILDFLAG(IS_WIN)
    base::win::ScopedHandle job;
#endif
    std::string error;
  };
  static std::unique_ptr<Launched> Launch(Options options);
  void DidLaunch(base::OnceCallback<void(std::string)> started,
                 std::unique_ptr<Launched> launched);
  void OutputClosed();
  void Exited(int code);
  void MaybeFinish();

  base::RepeatingCallback<void(std::string)> on_line_;
  base::OnceCallback<void(int, std::string)> on_exit_;
  base::Process process_;
#if BUILDFLAG(IS_WIN)
  base::win::ScopedHandle job_;
#endif
  std::unique_ptr<agent::LineChannel> io_;
  std::unique_ptr<agent::LineChannel> errors_;
  std::string error_tail_;
  std::vector<std::string> pending_;
  bool close_input_ = false;
  bool output_closed_ = false;
  bool exited_ = false;
  std::optional<int> exit_code_;
  base::OneShotTimer exit_grace_;
  base::OneShotTimer kill_timer_;
  base::WeakPtrFactory<LineProcess> weak_factory_{this};
};

// 子进程的环境：浏览器自己的环境变量，PATH 前面加上常见的安装目录（从访达打开的 App 拿不到
// shell 的 PATH）和可执行文件所在的目录。
base::EnvironmentMap ChildEnvironment(const base::FilePath& executable);

}  // namespace starry::assistant

#endif  // STARRY_BROWSER_ASSISTANT_LINE_PROCESS_H_
