// Copyright 2026 The Starry Browser Authors

#include "starry/browser/assistant/assistant_settings.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "starry/browser/l10n/l10n.h"
#include "starry/browser/prefs/starry_prefs.h"

namespace starry::assistant {

namespace {

struct CodexLevels {
  std::string configured_model;
  std::vector<std::string> levels;
};

std::optional<CodexLevels>& ReportedCodexLevels() {
  static base::NoDestructor<std::optional<CodexLevels>> levels;
  return *levels;
}

}  // namespace

Settings::Settings() = default;
Settings::Settings(const Settings&) = default;
Settings& Settings::operator=(const Settings&) = default;
Settings::~Settings() = default;

std::string Settings::reasoning_key() const {
  return is_local() ? backend : "chat:" + provider_id;
}

bool Settings::Configured() const {
  if (is_local()) {
    return true;
  }
  return !provider_id.empty() && !provider.base_url.empty() &&
         !model.empty() && (!provider.requires_key || has_key);
}

namespace {

// 用哪一家 OpenAI 兼容服务：服务、它选的模型、有没有 Key。
void ApplyProvider(PrefService* prefs,
                   const std::string& provider_id,
                   Settings& settings) {
  settings.provider_id = provider_id;
  settings.provider = ChatProvider();
  settings.model.clear();
  if (settings.provider_id == kCustomProviderId) {
    settings.provider.id = kCustomProviderId;
    settings.provider.name = base::UTF16ToUTF8(L("settings.ai.assistant.custom.defaultName"));
    settings.provider.base_url = prefs->GetString(prefs::kAssistantCustomUrl);
    // 自定义的服务：没填 Key 就不带。
    settings.provider.requires_key = false;
    settings.provider.stream_usage = false;
  } else if (const ChatProvider* provider =
                 FindProvider(settings.provider_id)) {
    settings.provider = *provider;
  } else {
    settings.provider_id.clear();
  }
  if (const std::string* model =
          prefs->GetDict(prefs::kAssistantModels).FindString(
              settings.provider_id)) {
    settings.model = *model;
  }
  settings.has_key =
      prefs->GetDict(prefs::kAssistantKeys).FindString(settings.provider_id) !=
      nullptr;
}

}  // namespace

Settings ReadOrganizerSettings(Profile* profile) {
  Settings settings = ReadSettings(profile);
  PrefService* prefs = prefs::PrefsFor(profile);
  const std::string backend = prefs->GetString(prefs::kOrganizerBackend);
  // 没单独选：和 AI 助手相同。思考程度都用默认的（只要一段 JSON）。
  settings.reasoning.clear();
  settings.reasoning_levels.clear();
  if (backend == kBackendClaudeCode || backend == kBackendCodex) {
    settings.backend = backend;
    settings.local_path = LocalPath(profile, backend);
    settings.local_model = LocalModel(profile, backend);
  } else if (backend == kBackendChat) {
    settings.backend = kBackendChat;
    const std::string provider = prefs->GetString(prefs::kOrganizerProvider);
    if (!provider.empty()) {
      ApplyProvider(prefs, provider, settings);
    }
    if (const std::string& model = prefs->GetString(prefs::kOrganizerModel);
        !model.empty()) {
      settings.model = model;
    }
  }
  return settings;
}

Settings ReadSettings(Profile* profile) {
  PrefService* prefs = prefs::PrefsFor(profile);
  Settings settings;
  const std::string backend = prefs->GetString(prefs::kAssistantBackend);
  if (backend == kBackendClaudeCode || backend == kBackendCodex) {
    settings.backend = backend;
    settings.local_path = LocalPath(profile, backend);
    settings.local_model = LocalModel(profile, backend);
  }
  ApplyProvider(prefs, prefs->GetString(prefs::kAssistantProvider), settings);
  const std::string mode = prefs->GetString(prefs::kAssistantMode);
  settings.mode =
      mode == kModeAgent || mode == kModeAuto ? mode : std::string(kModeAsk);
  settings.allow_scripts = prefs->GetBoolean(prefs::kAgentAllowScripts);
  settings.allow_uploads = prefs->GetBoolean(prefs::kAgentAllowUploads);
  settings.allow_private_tabs =
      prefs->GetBoolean(prefs::kAgentAllowPrivateTabs);
  settings.panel_style =
      prefs->GetString(prefs::kAssistantPanelStyle) == kPanelDock ? kPanelDock
                                                                  : kPanelFloat;
  if (settings.backend == kBackendClaudeCode) {
    settings.reasoning_levels = ClaudeCodeReasoningLevels();
  } else if (settings.backend == kBackendCodex) {
    const std::optional<CodexLevels>& reported = ReportedCodexLevels();
    settings.reasoning_levels =
        reported && reported->configured_model == settings.local_model
            ? reported->levels
            : CodexFallbackReasoningLevels();
  } else if (!settings.provider_id.empty()) {
    settings.reasoning_levels = ReasoningLevels(settings.provider);
  }
  if (const std::string* level = prefs->GetDict(prefs::kAssistantReasoning)
                                     .FindString(settings.reasoning_key());
      level && std::ranges::find(settings.reasoning_levels, *level) !=
                   settings.reasoning_levels.end()) {
    settings.reasoning = *level;
  }
  return settings;
}

void SetReasoning(Profile* profile, const std::string& level) {
  const Settings settings = ReadSettings(profile);
  ScopedDictPrefUpdate update(prefs::PrefsFor(profile),
                              prefs::kAssistantReasoning);
  if (level.empty()) {
    update->Remove(settings.reasoning_key());
  } else {
    update->Set(settings.reasoning_key(), level);
  }
}

void SetPanelStyle(Profile* profile, const std::string& style) {
  prefs::PrefsFor(profile)->SetString(
      prefs::kAssistantPanelStyle,
      style == kPanelDock ? kPanelDock : kPanelFloat);
}

bool Enabled(Profile* profile) {
  return prefs::PrefsFor(profile)->GetBoolean(prefs::kAssistantEnabled);
}

void SetEnabled(Profile* profile, bool enabled) {
  prefs::PrefsFor(profile)->SetBoolean(prefs::kAssistantEnabled, enabled);
}

void SetCodexReasoningLevels(const std::string& configured_model,
                             std::vector<std::string> levels) {
  ReportedCodexLevels() = CodexLevels{configured_model, std::move(levels)};
}

void SetProvider(Profile* profile, const std::string& provider_id) {
  prefs::PrefsFor(profile)->SetString(prefs::kAssistantProvider, provider_id);
}

void SetModel(Profile* profile,
              const std::string& provider_id,
              const std::string& model) {
  ScopedDictPrefUpdate update(prefs::PrefsFor(profile),
                              prefs::kAssistantModels);
  if (model.empty()) {
    update->Remove(provider_id);
  } else {
    update->Set(provider_id, model);
  }
}

void SetCustomBaseUrl(Profile* profile, const std::string& url) {
  prefs::PrefsFor(profile)->SetString(prefs::kAssistantCustomUrl, url);
}

std::string CustomBaseUrl(Profile* profile) {
  return prefs::PrefsFor(profile)->GetString(prefs::kAssistantCustomUrl);
}

void SetMode(Profile* profile, const std::string& mode) {
  prefs::PrefsFor(profile)->SetString(
      prefs::kAssistantMode,
      mode == kModeAgent || mode == kModeAuto ? mode : std::string(kModeAsk));
}

void SetBackend(Profile* profile, const std::string& backend) {
  prefs::PrefsFor(profile)->SetString(
      prefs::kAssistantBackend,
      backend == kBackendClaudeCode || backend == kBackendCodex ? backend
                                                                : kBackendChat);
}

std::string LocalPath(Profile* profile, const std::string& backend) {
  const std::string* path = prefs::PrefsFor(profile)
                                ->GetDict(prefs::kAssistantLocalPaths)
                                .FindString(backend);
  return path ? *path : std::string();
}

std::string LocalModel(Profile* profile, const std::string& backend) {
  const std::string* model = prefs::PrefsFor(profile)
                                 ->GetDict(prefs::kAssistantLocalModels)
                                 .FindString(backend);
  return model ? *model : std::string();
}

void SetLocalPath(Profile* profile,
                  const std::string& backend,
                  const std::string& path) {
  ScopedDictPrefUpdate update(prefs::PrefsFor(profile),
                              prefs::kAssistantLocalPaths);
  if (path.empty()) {
    update->Remove(backend);
  } else {
    update->Set(backend, path);
  }
}

void SetLocalModel(Profile* profile,
                   const std::string& backend,
                   const std::string& model) {
  ScopedDictPrefUpdate update(prefs::PrefsFor(profile),
                              prefs::kAssistantLocalModels);
  if (model.empty()) {
    update->Remove(backend);
  } else {
    update->Set(backend, model);
  }
}

void SetAllowScripts(Profile* profile, bool allow) {
  prefs::PrefsFor(profile)->SetBoolean(prefs::kAgentAllowScripts, allow);
}

void SetAllowUploads(Profile* profile, bool allow) {
  prefs::PrefsFor(profile)->SetBoolean(prefs::kAgentAllowUploads, allow);
}

void SetAllowPrivateTabs(Profile* profile, bool allow) {
  prefs::PrefsFor(profile)->SetBoolean(prefs::kAgentAllowPrivateTabs, allow);
}

void GetApiKey(Profile* profile,
               const std::string& provider_id,
               base::OnceCallback<void(std::string)> callback) {
  const std::string* stored = prefs::PrefsFor(profile)
                                  ->GetDict(prefs::kAssistantKeys)
                                  .FindString(provider_id);
  std::string cipher;
  if (!stored || !base::Base64Decode(*stored, &cipher)) {
    std::move(callback).Run(std::string());
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      [](std::string cipher, base::OnceCallback<void(std::string)> callback,
         scoped_refptr<os_crypt_async::Encryptor> encryptor) {
        std::string plain;
        if (!encryptor || !encryptor->DecryptString(cipher, &plain)) {
          plain.clear();
        }
        std::move(callback).Run(std::move(plain));
      },
      std::move(cipher), std::move(callback)));
}

void SetApiKey(Profile* profile,
               const std::string& provider_id,
               const std::string& key,
               base::OnceClosure done) {
  PrefService* prefs = prefs::PrefsFor(profile);
  if (key.empty()) {
    ScopedDictPrefUpdate(prefs, prefs::kAssistantKeys)->Remove(provider_id);
    std::move(done).Run();
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      [](base::WeakPtr<Profile> profile, std::string provider_id,
         std::string key, base::OnceClosure done,
         scoped_refptr<os_crypt_async::Encryptor> encryptor) {
        std::string cipher;
        if (profile && encryptor && encryptor->EncryptString(key, &cipher)) {
          ScopedDictPrefUpdate(prefs::PrefsFor(profile.get()),
                               prefs::kAssistantKeys)
              ->Set(provider_id, base::Base64Encode(cipher));
        }
        std::move(done).Run();
      },
      profile->GetWeakPtr(), provider_id, key, std::move(done)));
}

std::u16string ProviderDisplayName(const ChatProvider& provider) {
  for (const char* id :
       {"dashscope", "moonshot", "siliconflow", "volcengine", "zhipu"}) {
    if (provider.id == id) {
      return L(std::string("assistant.provider.") + id);
    }
  }
  return base::UTF8ToUTF16(provider.name);
}

}  // namespace starry::assistant
