// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_AGENT_CDP_SESSION_H_
#define STARRY_BROWSER_AGENT_CDP_SESSION_H_

#include <map>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class DevToolsAgentHost;
class WebContents;
}  // namespace content

namespace starry::agent {

// 进程里的 DevTools 协议（CDP）连接，一个标签一个：智能体的真实输入（Input.*）、截图
// （Page.captureScreenshot）、执行脚本（Runtime.evaluate）走它。和 chrome.debugger、
// 开发者工具一样的输入，事件的 isTrusted 是 true，只发给这个标签，不碰系统的鼠标键盘。
// 跨进程的 iframe 是单独的一个目标，要在它里面收事件（网页要选文件）时另开一条。
class CdpSession : public content::DevToolsAgentHostClient {
 public:
  using Result = base::expected<base::DictValue, std::string>;
  using Callback = base::OnceCallback<void(Result)>;
  using EventCallback =
      base::RepeatingCallback<void(const std::string& method,
                                   const base::DictValue& params)>;

  explicit CdpSession(content::WebContents* contents);
  explicit CdpSession(scoped_refptr<content::DevToolsAgentHost> host);
  CdpSession(const CdpSession&) = delete;
  CdpSession& operator=(const CdpSession&) = delete;
  ~CdpSession() override;

  bool attached() const { return attached_; }
  void Send(const std::string& method, base::DictValue params, Callback callback);
  // 协议的事件（Page.fileChooserOpened……），要先打开对应的域。
  base::CallbackListSubscription AddEventCallback(EventCallback callback);
  // Page 域打开过了（文件选择的事件要它）。
  bool page_enabled() const { return page_enabled_; }
  void set_page_enabled() { page_enabled_ = true; }
  base::WeakPtr<CdpSession> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;
  bool IsTrusted() override;
  bool MayReadLocalFiles() override;

 private:
  scoped_refptr<content::DevToolsAgentHost> host_;
  bool attached_ = false;
  bool page_enabled_ = false;
  int next_id_ = 1;
  std::map<int, Callback> pending_;
  base::RepeatingCallbackList<void(const std::string&, const base::DictValue&)>
      events_;
  base::WeakPtrFactory<CdpSession> weak_factory_{this};
};

}  // namespace starry::agent

#endif  // STARRY_BROWSER_AGENT_CDP_SESSION_H_
