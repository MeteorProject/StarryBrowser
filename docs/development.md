# 开发与构建

Starry Browser 的自研代码全部位于独立的 `starry/` 目录（在 macOS 上符号链接到 `chromium/src/starry`，在 Windows 上通过 `scripts/windows_overlay.py` 同步到同一位置），对 Chromium 上游的修改以最小补丁形式保存在 `patches/` 中。

硬件要求与 Chromium 一致：建议 16 GB 以上内存（推荐 32 GB）、100 GB 以上可用磁盘空间，首次完整编译需要数小时。

## 构建步骤

### macOS

需要 Xcode 及 Metal 工具链（Xcode 26 起需单独执行 `xcodebuild -downloadComponent MetalToolchain`）：

```sh
scripts/bootstrap.sh          # 下载 depot_tools 到 vendor/depot_tools
scripts/sync.sh               # 同步 Chromium 源码、链接 starry/ 并应用 patches/
scripts/build.sh Release      # 编译 chrome（若未生成构建目录会自动执行 scripts/gen.sh）
scripts/run.sh Release        # 使用临时用户数据目录启动
```

日常增量开发可使用 `Dev` 配置（组件构建，链接速度更快，但不可用于打包）：

```sh
scripts/build.sh Dev
scripts/run.sh Dev
```

### Windows

需要 Visual Studio（安装「使用 C++ 的桌面开发」、MFC/ATL）、对应版本的 Windows 11 SDK 与 SDK Debugging Tools，并将 `depot_tools` 加入 `PATH` 最前，设置 `DEPOT_TOOLS_WIN_TOOLCHAIN=0` 与 `git config --global core.autocrlf false`。在 `cmd` 中执行（假设仓库位于短路径 `C:\src\starry-browser-chromium`）：

```bat
cd /d C:\src\starry-browser-chromium
mkdir chromium
```

创建 `chromium\.gclient`（将版本替换为 `chromium.version` 中的标签）：

```python
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git@refs/tags/157.0.8103.51",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = ["win"]
```

同步源码、同步 `starry/` 并编译：

```bat
cd chromium
gclient sync --nohooks --no-history -D --force
gclient runhooks

python3 ..\scripts\windows_overlay.py ..\starry src\starry

cd src
for /r "..\..\patches" %f in (*.patch) do git apply --whitespace=nowarn "%f"

mkdir out\Release
echo import("//starry/build/args/release.gn") > out\Release\args.gn
gn gen out\Release
autoninja -C out\Release chrome
```

运行：

```bat
out\Release\starry-browser.exe --user-data-dir=%TEMP%\starry-dev --no-first-run --no-default-browser-check
```

在 Windows 上修改 `starry\` 后，重新执行 `windows_overlay.py` 再增量编译即可。

## 构建配置

`scripts/gen.sh [Release|Dev]` 会生成 `chromium/src/out/<配置>/args.gn`，引用 `starry/build/args/` 下的配置文件（`common.gni`、`release.gn`、`dev.gn`）。

如需本地额外 GN 参数，可在仓库根目录创建被 `.gitignore` 忽略的 `args.local.gn`，`scripts/gen.sh` 会自动追加其内容；加 `--no-local` 可忽略本地参数。

## 补丁管理

尽量将代码放在 `starry/` 中，仅在必要时修改 `chromium/src` 中的上游文件，且保持改动最小化：

```sh
scripts/patches.sh apply      # 链接 starry/ 并应用 patches/
scripts/patches.sh export     # 将 chromium/src 的当前改动重新导出到 patches/
scripts/patches.sh check      # 校验 chromium/src 与 patches/ 是否一致
scripts/patches.sh revert     # 还原补丁修改过的上游文件
```

## 品牌、图标与打包

- 品牌标识与产品名集中在 [`starry/app/theme/BRANDING`](../starry/app/theme/BRANDING)，由 `common.gni` 中的 `branding_file_path` 选用。
- 应用图标源文件位于 `starry/app/icon/`，在 macOS 上可通过 `scripts/app-icon.sh` 重新生成各平台图标并导出补丁。
- 使用 `scripts/package.sh` 为当前平台打包（仅支持 `Release` 非组件构建，产物输出到 `dist/`）：

```sh
scripts/package.sh                                  # 编译 Release 并打包
scripts/package.sh --skip-build                     # 直接使用已有 out/Release 产物打包
scripts/package.sh --identity "Developer ID Application: …" --notary-profile <profile>   # macOS 签名与公证
```

## 界面本地化

语言文件位于 `starry/browser/resources/l10n/`（`zh-Hans`、`zh-Hant`、`en`、`ja`、`ko`）。C++ 中使用 `L("key")`，页面 JS 使用 `L('key')`，HTML 使用 `$L{key}`。

提交前运行以下检查：

```sh
scripts/patches.sh check      # 需要已同步的 Chromium 源码
scripts/l10n.py check
scripts/l10n.py hardcoded
git diff --check
```
