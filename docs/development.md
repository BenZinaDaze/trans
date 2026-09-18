# 开发与维护指南

本文面向 Trans 的开发者和维护者。安装、配置和日常使用见 [README](../README.md)。

## 本地构建与运行

Trans 使用 C++20、Qt 6 / QML 和 KDE Frameworks 6。CMake 最低版本为 3.24，Qt 最低版本为 6.5。现有开发环境使用 conda `myself`，构建链接系统 Qt/KDE 库，无需安装 Python 包；发行包运行不需要 conda。

```sh
sudo pacman -S --needed base-devel cmake ninja qt6-base qt6-declarative qt6-svg kglobalaccel kwindowsystem
conda run -n myself --no-capture-output cmake --preset dev
conda run -n myself --no-capture-output cmake --build --preset dev
conda run -n myself --no-capture-output ctest --preset dev
conda run -n myself --no-capture-output ./build/trans --settings
```

也可先 `conda activate myself` 再执行命令，或在系统环境中直接执行去掉 `conda run -n myself --no-capture-output` 前缀的命令。预设指定系统 `/usr` 中的 Qt/KDE；不要通过 `QT_PLUGIN_PATH`、`QML_IMPORT_PATH`、`LD_LIBRARY_PATH` 混入另一套 Qt 库。

首次启动显示配置中心；以后可从托盘菜单、翻译弹窗右上角的设置图标或 `./build/trans --settings` 打开。语言栏右侧的箭头可直接打开翻译偏好。再次启动会将命令转交已有实例。`./build/trans --translate` 翻译当前选区；`./build/trans --ocr` 发起截图翻译。退出位于托盘菜单。

## 架构与参考

```text
KGlobalAccel → X11 SelectionReader → TranslationController → OpenAI / DeepSeek
                                           ↓
                                     QML 翻译弹窗

QML 配置中心 → AppSettings + DesktopBridge（保存及快捷键）
             → ProviderTools（获取模型、试译）
```

`TranslationProvider` / `TranslationJob` 封装异步接口，统一结果和错误。`ProviderRegistry` 仅注册两家提供商。`AppSettings` 提供完整配置快照、校验与持久化；页面修改的是草稿，保存成功后统一生效。`ProviderTools` 使用独立的可取消请求，不改变当前选区翻译。

参考 [Read Frog](https://github.com/mengxi-ream/read-frog) 的独立提供商、基础地址、模型、温度、推理级别、自定义请求头与参数配置方式，未引入其浏览器扩展或 AI SDK 依赖。参考版本：`0cd93f25d98018e1a502df4819ce8d22c5aa1254`，主要查阅 `src/utils/constants/providers.ts`、`src/types/config/provider/schemas.ts` 和 `src/utils/providers/model.ts`。协议实现依据 [OpenAI Responses](https://developers.openai.com/api/reference/resources/responses/methods/create)、[OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create) 和 [DeepSeek Chat Completion](https://api-docs.deepseek.com/api/create-chat-completion) 文档。

## 配置与协议细节

所有应用选项均可在页面修改，无需手动编辑配置文件。侧边栏分为翻译服务、翻译偏好、快捷键与窗口、截图 OCR，窄窗口下收起为图标导航；四页共用底部的 **保存全部** 按钮。“放弃修改”重新载入已保存值，关闭设置会丢弃未保存的输入。

### API 提供商

仅支持 **OpenAI** 和 **DeepSeek**，每家单独保存配置。

| 提供商 | 默认地址 | 默认模型 | API |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-5.6-luna` | 默认 Responses，可切换 Chat Completions |
| DeepSeek | `https://api.deepseek.com` | `deepseek-flash` | Chat Completions |

默认模型参考 Read Frog 的提供商预设；实际可用模型取决于账户和服务端。可点击“获取模型”查询，也可直接输入任意模型 ID。OpenAI 代理可通过修改基础地址接入，不额外增加提供商条目。程序会自动追加 `/responses`、`/chat/completions` 或 `/models`；地址不要包含这些末级路径。

页面提供以下选项：

- API 密钥、基础地址、模型与 OpenAI API 模式。
- 手动调整随机性（温度）：数值范围为 0–2，越低措辞通常越稳定。默认关闭，使用服务默认值；开启后才发送填写的温度参数。
- 最大输出 token 数：0 为服务默认，或 16–131072；最终上限以模型为准。
- 推理级别：默认关闭，可改为服务默认（省略参数）或提供商支持的级别。较旧模型不支持推理参数时选择“服务默认”。
- 自定义请求头与额外请求参数：在页面的 JSON 编辑框填写对象。例如请求头 `{"OpenAI-Project":"proj_example"}`、参数 `{"top_p":0.9}`。已有独立设置项的字段不能在此重复指定。
- “测试翻译”：使用当前页面尚未保存的配置，发送固定文本 `Hello, world!`；按服务的正常请求计费。可取消。获取模型同样使用页面中的密钥、地址和请求头。

切换提供商会保留两边的编辑内容，“保存全部”同时保存；当前所选提供商用于之后的选区翻译。允许先保存未填写密钥的配置，实际翻译或测试前会提示补全。

OpenAI Responses 请求设置 `store: false`，从完成的 assistant 消息中读取译文；DeepSeek 仅显示最终 `content`，不把 `reasoning_content` 当作译文。所有请求均为非流式，支持取消、超时和旧响应隔离。

### 翻译

- 源语言（默认自动检测）、目标语言（默认简体中文）。
- 翻译提示词及恢复默认按钮。`{{sourceLanguage}}`、`{{targetLanguage}}` 在请求前替换；目标语言占位符必须保留。
- 请求超时：1–600 秒，默认 30 秒。
- 选中文本长度上限：1–200000 个 UTF-16 代码单元，默认 20000。
- 响应大小上限：16–16384 KiB，默认 2048 KiB。

### 快捷键与窗口

- 点击“录制”，按含 Ctrl、Alt 或 Meta 的组合；Esc 取消录制。“清空”禁用，“默认快捷键”恢复 Meta+Shift+T。保存时通过 KGlobalAccel 检查冲突，失败会提示且不保存新设置；无需打开 KDE 系统设置。
- 弹窗位置：鼠标所在屏幕中央或鼠标附近，均限制在屏幕可用区域内。
- 译文字号、是否置顶，以及关闭后是否恢复原应用焦点。翻译窗统一按原文和译文自动计算宽高，不再提供或保存手动尺寸。

自动尺寸使用与显示控件相同的字体测量文本，译文返回或字号变化时合并布局更新。短词使用紧凑窗口，长段落扩展到当前屏幕可用区域内的阅读上限，超出部分滚动显示。结果返回时在原屏幕调整尺寸，不跟随已经移开的鼠标；窗口装饰边框也包含在边界检查中。

设置窗是翻译窗的非模态从属对话框，翻译窗置顶或重新显示时，设置窗仍保持在它上方；单独显示设置窗时不置顶。翻译窗使用普通窗口类型以避免 KDE 的工具窗口层级冲突，通过 KDE 的窗口状态接口隐藏任务栏和分页器条目。滚轮先交给鼠标下的控件处理，提示词等内层编辑框到达边界后交给外层页面；侧边滚动条也支持滚轮，无需先点击获得焦点。

全局快捷键通过 KGlobalAccel 经会话 D-Bus 注册到 KDE 的 `kglobalacceld`，组件名为 `trans`，动作名为 `translate-selection`。KDE 在 `~/.config/kglobalshortcutsrc` 中保留绑定；程序启动时先恢复绑定，再应用 Trans 本地配置中的快捷键。KDE 的可用性查询会把当前动作自己的活动绑定也视为占用，因此冲突检查先排除当前动作已持有的键。通过 D-Bus 服务 `io.github.trans.Trans` 确保单实例，后续进程仅向已有实例转发命令，不重复注册快捷键。

### 截图 OCR

`ScreenshotJob` 隔离截图后端，`createScreenshotJob` 根据平台选择实现。X11 使用 `X11RegionScreenshotJob`：先捕获所有屏幕，再为每块屏幕显示冻结画面的 `RegionOverlay`，用户左键拖动并松开后只返回框选的 `QImage`。坐标按逻辑窗口尺寸与实际图片尺寸映射，支持不同缩放比例、反向拖动和负坐标屏幕；拖动限制在起始屏幕内。单击或小于 15 像素的选区不会提交。Esc、右键、超时、屏幕布局变化和请求替换均关闭遮罩，释放键盘抓取。截图全程在内存中处理。

保留 `PortalScreenshotJob` 供后续 Wayland 适配，通过 Qt DBus 调用 `org.freedesktop.portal.Screenshot`；必须是版本 3 且 `AvailableTargets` 包含区域目标才发送 `target=4`。旧版 `interactive=true` 并不保证有区域选项，因此不再回退为全屏截图，而是明确提示区域截图不可用。调用前订阅预期请求路径的 `Response`，返回不同路径时调整订阅；取消时调用 `Request.Close`。两种截图交互最长等待 180 秒。

`BaiduOcrProvider` / `OcrJob` 单独实现百度 `accurate_basic`：PNG → Base64 → 百分号转义表单，`language_type=auto_detect`。按 docs/OCR.pdf 该接口章节限制像素和编码后体积（15–8192 像素、10 MB），不套用文档概述中其他接口的限制。采用固定百度 HTTPS 地址，保留 TLS 验证，禁止自动重定向；测试可注入本地地址。API Key 和 Secret Key 获取的 token 仅内存缓存，预留 60 秒过期裕量；错误 110/111 最多刷新一次，刷新也计入本次 OCR 超时。响应最多 2 MiB。

控制器状态增加 `capturing`、`recognizing`，共用请求编号隔离旧截图、OCR 和翻译响应。识别文字逐行保留，交给现有翻译方法和输入上限检查；重新翻译不重新识别。截图时隐藏两个窗口但保留设置草稿；截图取消恢复先前内容，识别失败提示重新截图。关闭结果窗取消当前请求。

新增动作 `translate-screenshot`，默认 Meta+Shift+O；`TranslateScreenshot` 同时通过 QML、托盘和单实例 D-Bus 暴露。快捷键保存先释放变更绑定，以支持交换两个快捷键；应用绑定或持久化失败时恢复原绑定。OCR 密钥使用原有配置文件权限与原子保存机制。Portal 返回本地 URI 后读取为 `QImage`，不保存历史、不删除归属不明的 Portal 文件。

`trans_ocr_tests` 使用本地 HTTP 服务与私有 D-Bus，覆盖鉴权缓存和刷新、请求编码、图片限制、空结果/额度/超时/取消、Portal 能力与响应、任务替换和快捷键回滚，不访问真实百度服务。配置页测试覆盖 OCR 草稿、保存和截图取消恢复。区域裁剪测试验证像素内容、反向拖动、缩放映射和误点击；可选 X11 测试使用真实鼠标拖动验证返回的图片尺寸及 Esc 取消。

### 本地存储与迁移

1.0.0 更名为 Trans：CMake 项目/目标与命令使用 `trans`，C++ 命名空间和 QML 模块使用 `Trans` / `Trans.Core`，桌面与 D-Bus ID 为 `io.github.trans.Trans`，对象路径为 `/Trans`，KGlobalAccel 组件为 `trans`。旧进程应先退出；系统快捷键中的旧组件绑定可能需要手动清除。构建选项和测试环境变量同步为 `TRANS_*`。

新版只使用 `trans/settings.ini`，不尝试读取或导入旧 `tran/settings.ini`。冒烟测试使用临时配置，不访问用户配置。

配置默认保存于 `~/.config/trans/settings.ini`，遵循 `XDG_CONFIG_HOME`。**密钥以明文保存在应用本地文件**，不使用系统密钥库。目录权限为 `0700`，文件为 `0600`，通过原子替换保存。保存失败时保留原文件和生效配置。

旧版的 OpenAI 地址、密钥、模型会自动读入并保留 Chat Completions 模式。旧版其他提供商不再可选，原先选中其他提供商时回到 OpenAI；下一次从页面保存时清理它们的配置。模型获取不写入配置，测试翻译不会自动保存编辑内容。

旧版 `window/width`、`window/height` 和 `window/rememberSize` 不再读取；下一次保存设置时移除这些字段。窗口关闭和自动调整大小都不会写入配置。

最近一次原文和译文保留在内存中，关闭弹窗不会清除，退出程序后清除，不写入磁盘。应用只在用户触发翻译时读取 X11 PRIMARY 选区，重新打开窗口不会读取选区；除“复制译文”外，不修改普通剪贴板。当前不支持 Wayland 选区读取或翻译历史；截图路径在 X11 使用内置框选，并保留 Portal 后端供后续适配 Wayland，但本版仅验收 X11。

## 验证与本地安装

`ctest --preset dev` 使用本机模拟 HTTP 服务、临时配置和隔离 D-Bus，覆盖两家提供商请求格式、响应解析、鉴权/超时/取消、过期响应、配置迁移、权限、配置页编辑和保存、快捷键冲突及失败回滚。不访问真实翻译服务，也不注册真实桌面快捷键。离屏测试加载全部配置页；QML 静态检查可执行：

```sh
conda run -n myself --no-capture-output cmake --build build --target trans_qmllint
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software ./build/trans --smoke-test --screenshot build/settings-preview.png
```

界面回归覆盖深浅主题、480×360 翻译窗、500×420 设置窗、长文本、等待/成功/失败/取消状态，以及复制、重试、服务下拉菜单、导航与页面滚动。可用临时配置和模拟译文生成界面预览，不调用翻译 API：

```sh
conda run -n myself --no-capture-output env QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software TRANS_UI_SCREENSHOT_DIR=/tmp/trans-ui-previews ./build-release/trans_tests redesignedUi
```

README 的截图使用上述输出中的 `dark-compact-translation.png`、`light-compact-translation.png`、`dark-settings-0.png`、`dark-settings-3.png`，分别对应 `docs/images/translation-dark.png`、`translation-light.png`、`settings-dark.png`、`ocr-settings-dark.png`。版本文字来自 CMake 项目版本，示例密钥和译文均为测试数据。

桌面验收：在浏览器、编辑器、终端和含文本层的 PDF 中选中内容并触发翻译，检查快捷键、双屏位置和关闭后焦点；关闭翻译窗后从托盘重新打开，确认恢复原文和译文且不发起新请求；在配置中心填写实际密钥后点击“测试翻译”验证服务。

选区翻译窗通过窗口事件过滤器处理 Esc，关闭时取消正在执行的翻译。设置窗使用自己的 Esc 快捷键；录制快捷键时，第一次 Esc 仅取消录制。两窗同时显示时，Esc 只操作当前获得焦点的窗口。

可选的真实 KDE X11 回归测试需要 `xdotool`，会打开临时选区源窗口、注册临时全局快捷键并发送实际鼠标和 Esc 按键事件；使用固定文本和模拟翻译，不调用 API。测试包含设置入口的窗口层级和滚轮、快捷键重启恢复、重复应用与真实冲突，以及焦点交接、原文选区、译文控件焦点、两个窗口同时显示和关闭后焦点恢复，结束后清理测试窗口及快捷键：

```sh
conda run -n myself --no-capture-output cmake --preset dev -DTRANS_X11_TESTS=ON
conda run -n myself --no-capture-output cmake --build build --target trans_x11_tests
conda run -n myself --no-capture-output env QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software ./build/trans_x11_tests
```

更新代码后须重新构建正在使用的版本，并从托盘退出旧进程后重新启动；再次启动程序会转发给已有实例，不会自动替换正在运行的旧版本。

```sh
conda run -n myself --no-capture-output cmake --preset release
conda run -n myself --no-capture-output cmake --build --preset release
conda run -n myself --no-capture-output ctest --preset release
cmake --install build-release --prefix "$HOME/.local"
```

确保 `~/.local/bin` 在桌面会话 PATH 中。安装后可从 KDE 应用启动器打开；需要登录自启时，在 KDE“自动启动”添加 Trans。

## GitHub 自动打包与发布

将本仓库（包含 `.github/workflows/package.yml` 与 `packaging/`）提交并推送到 GitHub 后，Actions 会自动构建 **Arch Linux x86_64 原生安装包**。构建运行在官方 `archlinux:base-devel` 容器中，使用系统 Qt/KDE，不需要 conda。

- **普通 push / PR**：运行打包检查、Release 编译、QML 检查、全部离屏回归测试；再用新容器安装包并检查启动。成功后的包和 `SHA256SUMS` 位于该次 Actions 的 `trans-arch-x86_64` Artifact，保留 14 天。
- **推送版本标签**：完成相同检查后，自动创建 GitHub Release 并上传安装包和校验文件。使用内置 `GITHUB_TOKEN`，无需添加个人令牌；仅发布任务需要 `contents: write` 权限。
- 标签必须是 `vX.Y.Z`，并与 `CMakeLists.txt` 的 `project(trans VERSION X.Y.Z ...)` 一致，否则失败且不发布。程序版本、设置页版本与包版本都来自该 CMake 版本。
- 重跑同一标签会复用 Release 并替换同名附件。普通提交不会创建 Release。

例如发布当前 `1.0.0` 版本，先提交并推送代码，再推送标签；分支名按实际仓库替换：

```sh
git push origin main
git tag v1.0.0
git push origin v1.0.0
```

后续版本先修改并提交 CMake 项目版本，再推送对应标签。GitHub 仓库的 Actions 必须启用；组织的令牌权限限制仍需允许发布任务写入 Releases。

用户下载、更新和安装步骤见 [README](../README.md#安装)。实际容器构建、安装验证和附件上传结果以对应 Actions 运行记录为准。

### 本地打包

本地也可生成相同包。需要仓库具有可用的 Git 提交，并以普通用户执行 `makepkg`。`prepare.py` 使用 `git archive HEAD`，仅打包**已提交文件**，不会带入未提交配置或本机构建产物；`PKGBUILD` 是由该脚本填入版本及源码校验值的模板。

```sh
sudo pacman -S --needed base-devel python cmake ninja qt6-base qt6-declarative qt6-svg kglobalaccel kwindowsystem ca-certificates hicolor-icon-theme dbus desktop-file-utils ttf-dejavu noto-fonts-cjk
python3 packaging/prepare.py --output-dir dist/arch
cd dist/arch
makepkg --cleanbuild --noconfirm
```

`prepare.py` 可附加 `--repository owner/repository` 设置包的项目地址，附加 `--tag v1.0.0` 验证标签与版本匹配。在 Actions 中，这两个值由工作流传入。生成的文件为 `trans-版本-1-x86_64.pkg.tar.zst`。

打包脚本的快速本地检查：

```sh
bash -n packaging/PKGBUILD packaging/build-arch.sh packaging/verify-arch.sh
python3 -m unittest discover -s tests -p test_packaging.py -v
```

[build-arch.sh](../packaging/build-arch.sh) 和 [verify-arch.sh](../packaging/verify-arch.sh) 供一次性 Arch 容器使用，会安装依赖并创建构建或测试用户；容器挂载和执行方式见 [工作流](../.github/workflows/package.yml)。验证脚本检查校验值、安装依赖、精确版本、程序/图标/桌面入口及包中是否有意外文件，再以普通用户运行 `trans --version` 和离屏启动。真实 KDE X11 交互测试不在 CI 中执行。第三方 Actions 固定到提交 SHA。
