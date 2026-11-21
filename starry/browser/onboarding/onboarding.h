// Copyright 2026 The Starry Browser Authors

#ifndef STARRY_BROWSER_ONBOARDING_ONBOARDING_H_
#define STARRY_BROWSER_ONBOARDING_ONBOARDING_H_

class GURL;
class Profile;

// 首次启动设置向导（starry://welcome）的状态与网址判断。
namespace starry::onboarding {

// 强制显示向导的命令行开关（测试、预览用）；本次启动完成后不再重复触发。
inline constexpr char kSwitch[] = "starry-onboarding";

// 当前普通窗口是否应进入向导；新用户资料首次查询时记下 pending，直到 MarkFinished。
bool ShouldShow(Profile* profile);
void MarkFinished(Profile* profile);

GURL WelcomeURL();
bool IsWelcomeURL(const GURL& url);

}  // namespace starry::onboarding

#endif  // STARRY_BROWSER_ONBOARDING_ONBOARDING_H_
