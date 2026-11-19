// Copyright 2026 The Starry Browser Authors

#include "starry/browser/tab_organizer/organizer_model.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/browser/storage_partition.h"
#include "starry/browser/assistant/chat_backend.h"
#include "starry/browser/assistant/claude_code_backend.h"
#include "starry/browser/assistant/codex_backend.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace starry::tab_organizer {

namespace {

// 本机智能体启动要几秒，整理上百个标签的回答也长：给足时间。
constexpr base::TimeDelta kTimeout = base::Seconds(150);
// 回答最多多少字节（再长就不对劲了）。
constexpr size_t kMaxAnswer = 256 * 1024;

}  // namespace

OrganizerModel::OrganizerModel(Profile* profile) : profile_(profile) {}

OrganizerModel::~OrganizerModel() = default;

void OrganizerModel::Run(std::string system_prompt,
                         std::string prompt,
                         Callback callback) {
  DCHECK(!callback_);
  callback_ = std::move(callback);
  system_prompt_ = std::move(system_prompt);
  prompt_ = std::move(prompt);
  settings_ = assistant::ReadOrganizerSettings(profile_);
  timeout_.Start(FROM_HERE, kTimeout,
                 base::BindOnce(&OrganizerModel::Finish,
                                weak_factory_.GetWeakPtr(),
                                base::unexpected("timeout")));
  if (settings_.is_local()) {
    const std::optional<assistant::LocalAgentKind> kind =
        assistant::LocalAgentFromBackend(settings_.backend);
    base::FilePath user_data;
    base::PathService::Get(chrome::DIR_USER_DATA, &user_data);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&assistant::PrepareLocalLaunch, *kind,
                       settings_.local_path, user_data),
        base::BindOnce(&OrganizerModel::StartLocal,
                       weak_factory_.GetWeakPtr()));
    return;
  }
  if (!settings_.Configured()) {
    Finish(base::unexpected("notConfigured"));
    return;
  }
  assistant::GetApiKey(profile_, settings_.provider_id,
                       base::BindOnce(&OrganizerModel::StartChat,
                                      weak_factory_.GetWeakPtr()));
}

void OrganizerModel::StartChat(std::string key) {
  if (!callback_) {
    return;
  }
  assistant::ChatBackend::Config config;
  config.provider = settings_.provider;
  config.api_key = std::move(key);
  config.model = settings_.model;
  config.system_prompt = system_prompt_;
  config.tools = base::ListValue();
  config.max_steps = 1;
  backend_ = std::make_unique<assistant::ChatBackend>(
      std::move(config),
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      this);
  Send();
}

void OrganizerModel::StartLocal(assistant::LocalLaunch launch) {
  if (!callback_) {
    return;
  }
  if (launch.executable.empty()) {
    Finish(base::unexpected("localAgentMissing"));
    return;
  }
  if (settings_.backend == assistant::kBackendClaudeCode) {
    assistant::ClaudeCodeBackend::Config config;
    config.executable = launch.executable;
    config.model = settings_.local_model;
    config.system_prompt = system_prompt_;
    config.working_directory = launch.workspace;
    config.environment = std::move(launch.environment);
    config.persists_session = false;
    backend_ = std::make_unique<assistant::ClaudeCodeBackend>(
        std::move(config), this);
  } else {
    assistant::CodexBackend::Config config;
    config.executable = launch.executable;
    config.model = settings_.local_model;
    config.system_prompt = system_prompt_;
    config.working_directory = launch.workspace;
    config.environment = std::move(launch.environment);
    config.disabled_features = std::move(launch.disabled_features);
    config.disabled_servers = std::move(launch.disabled_servers);
    config.ephemeral = true;
    backend_ =
        std::make_unique<assistant::CodexBackend>(std::move(config), this);
  }
  Send();
}

void OrganizerModel::Send() {
  assistant::UserMessage message;
  message.text = std::move(prompt_);
  backend_->Send(std::move(message));
}

void OrganizerModel::Cancel() {
  Finish(base::unexpected("cancelled"));
}

void OrganizerModel::OnTextDelta(const std::string& text) {
  if (text_.size() + text.size() > kMaxAnswer) {
    Finish(base::unexpected("tooLong"));
    return;
  }
  text_ += text;
}

void OrganizerModel::OnToolStarted(const std::string& id,
                                   const std::string& name,
                                   const base::DictValue& arguments) {
  // 没给它工具：想调用就是不对劲。
  Finish(base::unexpected("toolCall"));
}

void OrganizerModel::OnUsage(const assistant::Usage& usage) {
  usage_.input_tokens += usage.input_tokens;
  usage_.output_tokens += usage.output_tokens;
  usage_.cost += usage.cost;
}

void OrganizerModel::OnFinished(assistant::StopReason reason,
                                assistant::FailureKind failure,
                                const std::string& message) {
  if (reason == assistant::StopReason::kCompleted) {
    Finish(base::ok(text_));
    return;
  }
  Finish(base::unexpected(base::StrCat({"failed: ", message})));
}

void OrganizerModel::Finish(Result result) {
  if (!callback_) {
    return;
  }
  timeout_.Stop();
  weak_factory_.InvalidateWeakPtrs();
  // 之后后端再发来的事件都不理（callback_ 空了）。
  std::move(callback_).Run(std::move(result));
}

}  // namespace starry::tab_organizer
