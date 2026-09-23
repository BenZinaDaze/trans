# 开发与维护指南

安装、配置和日常使用见 [README](../README.md)。本文描述当前 C++20 + Slint 实现，不再提供 Qt/QML 构建路径。

## 构建依赖与已验证范围

- CMake 3.24+、C++20 编译器、Ninja、Git、Rust **1.98.1**。
- Slint **1.18.1**：Winit 后端、FemtoVG 渲染器、系统托盘和可访问性；在编译时关闭 Qt 后端。Rust 用于编译 Slint，应用业务仍是 C++。
- libcurl **7.85+**、nlohmann/json **3.11+**、libpng、系统线程库。未找到系统 JSON 库时，CMake 获取锁定的 3.12.0。
- Linux：libdbus-1、XCB/RandR；Winit/FemtoVG 还需要 X11/Wayland、xkbcommon、字体与 OpenGL/EGL 运行依赖。KGlobalAccel 是可选桌面服务，不链接 KF 库。
- Windows x64：Win32、COM/UI Automation、GDI/DWM、原生 ACL 与命名管道；libcurl 使用启用证书验证的 TLS 后端，Windows 分发优先系统 Schannel。
- 测试使用 Python 3 标准库和一个原生 C++ 驱动，不要求 QtTest、QML 工具或 Python 第三方包。

本次迁移已有 **Linux 原生构建及真实 Slint 窗口 `--smoke-test` 通过**的记录，`native_business` CTest 也已通过；实际 UI 到本地 HTTP、X11 PRIMARY 读取和第二实例转发已经执行验证。Windows 后端/helper 通过 MinGW 交叉语法检查，但尚不能宣称 MSVC 或 Windows 桌面运行通过。真实 OpenAI/DeepSeek/百度 OCR、Wayland、多屏 DPI、完整窗口/快捷键交互等仍须分别验收，启动或局部回归成功不等于全部功能通过。

## Linux 源码构建

Arch Linux 开发依赖示例：

```sh
sudo pacman -S --needed base-devel cmake ninja git pkgconf rustup \
    curl nlohmann-json libpng dbus libxcb libx11 libxkbcommon libxkbcommon-x11 \
    wayland fontconfig freetype2 libglvnd ca-certificates hicolor-icon-theme \
    python noto-fonts-cjk
rustup toolchain install 1.98.1 --profile minimal
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/trans --settings
```

默认构建会获取锁定版本的 Slint 源码和 Corrosion，用 Cargo 的锁文件构建；首次配置需要访问相应源码仓库及 crate 下载源。无需 conda，也不要设置旧版 `QT_PLUGIN_PATH` 或 `QML_IMPORT_PATH`。

`cmake --build build` 构建主程序及启用的测试。Slint 的界面编译直接属于应用构建，不存在 `trans_qmllint` 目标。

### Slint 源码补丁与外部 SDK

上游 1.18.1 的 C++ 窗口接口没有本项目需要的 X11 窗口 ID 访问器。仓库的 [`cmake/slint-x11-window.patch`](../cmake/slint-x11-window.patch) 为同一个窗口适配器增加 `x11_window_id()`，通过 Slint 已有的 raw-window-handle 获取实际 XID；未创建窗口或 Wayland 返回 0。它不扫描其他窗口，也不把 Slint 私有 C++ 对象强转成系统句柄。

**这是需要维护的源码补丁，不是宣称上游已经提供该 C++ API。** 默认源码构建自动应用它。Slint 升级必须重新核对补丁、生成的 C 绑定与 ABI，并重新运行窗口集成验收。

使用自备 SDK 时，需要同时重建打过补丁的头文件、生成绑定和动态库；仅替换一个头文件不能使用。对一份干净的 Slint 1.18.1 源码应用补丁的命令为：

```sh
git -C /path/to/slint-1.18.1 apply --unidiff-zero /path/to/trans/cmake/slint-x11-window.patch
```

SDK 的功能开关必须与根 CMake 一致：启用 `BACKEND_WINIT`、`RENDERER_FEMTOVG`、`ACCESSIBILITY`、`SYSTEM_TRAY`，禁用 `BACKEND_QT`；不默认携带 Skia、Vello、LinuxKMS 或软件渲染器。构建及安装该 SDK 后，在独立构建目录指定它的配置目录：

```sh
cmake -S . -B build-sdk -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSlint_DIR=/path/to/patched-sdk/lib/cmake/Slint
cmake --build build-sdk --parallel 2
```

直接使用未修改的官方 Linux 1.18.1 SDK 缺少上述接口；不要通过扫描窗口或恢复 Qt 后端绕过这个错误。

### Windows 原生构建

目标为 Windows 10 22H2 / Windows 11 x64。安装 Visual Studio 2022 的 C++ 桌面组件、CMake、Git、Python、Rust 和 vcpkg，并在 x64 开发环境执行。以下使用已有的 `VCPKG_ROOT`；版本化发布以 Windows 工作流中锁定的依赖为准：

```powershell
rustup toolchain install 1.98.1 --profile minimal
& "$env:VCPKG_ROOT/vcpkg.exe" install 'curl[core,ssl]:x64-windows' libpng:x64-windows nlohmann-json:x64-windows
cmake -S . -B build-windows -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows -DBUILD_TESTING=ON
cmake --build build-windows --config Release --parallel 2
ctest --test-dir build-windows -C Release --output-on-failure
./build-windows/Release/trans.exe --smoke-test
```

当前 vcpkg 的 curl `ssl` 功能在普通 Windows 目标选择系统 TLS 路径；不要虚构不存在的 `schannel` port feature。需要核对实际锁定的 port 和产物，不能仅凭构建参数推断最终 TLS 后端。MSVC 的源码编码必须使用 `/utf-8`。

主程序和 `trans_selection_helper.exe` 必须一起分发。部署及工作流入口为 [`packaging/deploy-windows.ps1`](../packaging/deploy-windows.ps1)、[Windows 工作流](../.github/workflows/windows.yml)；它们收集实际原生 DLL、CRT 和第三方声明，不调用 `windeployqt`。不得从旧版目录带入 Qt DLL/QML 插件。Windows 编译或 CI 产物成功不能替代 Windows 10/11 普通用户桌面验收。

Windows 的 FemtoVG 渲染路径要求 OpenGL 2.0 驱动。GitHub 托管 Windows runner 默认不满足此条件，因此工作流在临时 runner 中安装固定版本 Mesa 软件 OpenGL 后运行真实分发程序的窗口冒烟；这不是禁用检查，也不是启用 Slint software renderer。Mesa 不进入应用 ZIP，用户机器仍需提供兼容显卡驱动；CI 通过不等于无驱动 Windows 环境可运行。

## 源码结构与执行模型

```text
ui/app.slint                    翻译窗口、四页设置、区域遮罩、系统托盘
        ↕ Slint C++ 绑定
src/native/main.cpp             UI 状态、设置草稿、任务代次、取消与命令装配
        ├── business.h/.cpp      配置导入/校验、翻译、模型列表、百度 OCR
        └── platform.h          原生能力与私有配置写入接口
              ├── platform_linux.cpp + linux_*.cpp
              └── platform_windows.cpp + selection_helper_windows.cpp
```

- Slint 主线程拥有 GUI。业务接口是同步、可取消的 C++ 函数，使用 `std::jthread`/`std::stop_token` 在工作线程执行；结果通过 `slint::invoke_from_event_loop` 回到主线程。
- 主翻译与设置页模型/试译任务有各自的代次。取消、关闭、替换任务后不得让迟到结果覆盖新状态。退出应取消并回收工作线程，再退出窗口循环。
- `trans_business` 只依赖 curl、JSON、标准库与线程库；不引用 Slint 或平台 GUI 类型。
- `Platform` 使用明确的字符串、像素缓冲区和来源对象。截图返回 RGBA 屏幕；共享 Slint 遮罩处理框选。Portal 可以直接返回已选择的区域。
- UI 调用 Slint 的真实复制接口；不能只显示“已复制”却没有修改剪贴板。关闭窗口隐藏程序，托盘退出才结束事件循环。
- 设置页编辑草稿。保存先校验、应用快捷键，再安全写入；失败尝试恢复旧绑定。恢复失败必须显式报告，不能把系统快捷键与文件保存描述为一个原子事务。

### 平台边界

**Linux：** XCB 读取 X11 PRIMARY，不读旧 CLIPBOARD 冒充当前选区，处理 UTF8_STRING/TARGETS、INCR、所有者消失、超时与取消。X11 屏幕捕获和窗口操作使用 XCB/EWMH。全局快捷键经 libdbus 直接访问 KDE KGlobalAccel，保留组件 `trans`、动作 `translate-selection`/`translate-screenshot`；不链接 QtDBus 或 KF6。Linux 单实例保留服务 `io.github.trans.Trans`、路径 `/Trans` 及 `ShowTranslation`、`TranslateSelection`、`TranslateScreenshot`、`ShowSettings` 四个方法。

Wayland 不开放任意应用选区读取。Screenshot Portal 必须提供版本 3 和区域目标 `AvailableTargets & 4` 才发起区域截图；不足时明确报错，不把整屏当区域上传。Portal 取消会关闭请求；显示与定位仍尊重 Wayland 权限边界。Slint 托盘在 Linux 使用桌面的 StatusNotifierItem/菜单服务，不能据 Linux 启动成功推断目标桌面已有托盘宿主。

**Windows：** 原生消息线程处理 `RegisterHotKey`，UI Automation 取词在独立 COM MTA helper 进程中完成。来源 HWND/PID/TID、密码控件、高权限来源、焦点变化以及协议长度均属于安全边界。父进程限制 helper 时间和输出，用 Job Object 管理生命周期。截图使用 GDI/DWM，窗口使用公开 HWND 接口，不模拟 Ctrl+C 或 Alt 来取得文本或强抢焦点。单实例使用当前用户/登录会话隔离的命名互斥与受限命名管道。

屏幕坐标使用物理像素；Slint 逻辑框选坐标须与源像素正确映射。置顶、鼠标附近定位、从属设置窗口和恢复焦点通过平台能力执行；这些交互必须在真实目标桌面验收，而不是仅检查属性设置成功。

## 配置与服务协议

### 新配置及旧 INI 导入

- Linux：`$XDG_CONFIG_HOME/trans/settings.json`，未设置时使用 `~/.config/trans/settings.json`。
- Windows：通常为 `%LOCALAPPDATA%\trans\settings.json`，沿用当前用户应用目录；低完整性进程使用对应的 LocalAppDataLow。
- JSON 根字段 `schemaVersion` 当前为 **1**。应用选项沿用原快照字段；`providerConfigs.openai` 与 `providerConfigs.deepseek` 分别保存 endpoint、model、apiKey、apiMode、temperatureEnabled、temperature、maxOutputTokens、reasoning、headersJson、optionsJson。
- 仅当 JSON 不存在时读取同目录 `settings.ini`。导入覆盖原应用写出的分组、反斜线键、百分号键、Unicode、引号与换行转义、JSON 字符串、布尔及数字。
- 旧 OpenAI 配置存在 endpoint 但没有 apiMode 时，导入为 `chat`、`reasoning=default`，保留密钥和模型。未知的已选服务报错，不静默切换提供商。
- `loadSettings` 不创建或修改文件。调用方只有在导入校验和安全原子保存成功后才启用迁移结果，原 INI 保留。损坏、未知版本或缺少必需字段的 JSON 会报错，不重新导入 INI 覆盖它。
- 不读取更早更名前的 `tran/settings.ini`。备份/清除密钥时同时处理保留的 INI 和 JSON。

密钥为本地明文，不是加密保险库。Linux 应用目录/文件权限分别为 `0700`/`0600`；Windows 为当前用户和 SYSTEM 的受保护 DACL。同目录临时文件从写入前就受保护，原子替换失败保留旧内容，不回退为无保护保存。Windows 的 UTF-8 路径在标准文件系统边界使用 `std::filesystem::u8path`。

### 翻译与模型发现

| 提供商 | 默认基础地址 | 默认模型 | 模式 |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-5.6-luna` | 默认 Responses，可切换 Chat Completions |
| DeepSeek | `https://api.deepseek.com` | `deepseek-flash` | Chat Completions |

模型名称不保证账户有权限；DeepSeek 预设还包含 `deepseek-v4-pro`，也可输入任意模型 ID，或用当前草稿查询 `/models`。服务地址只填基础路径，程序追加 `/responses`、`/chat/completions` 或 `/models`。地址不允许用户名、查询参数或片段。

- 全部请求为非流式。OpenAI Responses 设置 `store=false`，只接受 `status=completed` 的 assistant 输出；Chat 拒绝截断结果。DeepSeek 不把 `reasoning_content` 当译文。
- 默认不发送温度；启用后范围 0–2。输出 token 数为 0（服务默认）或 16–131072。Responses、OpenAI Chat、DeepSeek 分别使用 `max_output_tokens`、`max_completion_tokens`、`max_tokens`。
- 推理 `default` 省略专用参数。DeepSeek 的 `none` 显式关闭 thinking；其他有效级别启用 thinking 并设置 reasoning_effort。
- 请求头和高级参数须为 JSON 对象。Authorization、Content-Type 等由应用管理；model/messages/stream/temperature/reasoning/token 等专用字段不可通过高级参数覆盖。
- 试译与正常翻译共用 `translate`，固定文本为 `Hello, world!`，使用未保存草稿，正常计费且不会自动保存。
- TLS 和代理证书校验开启，不跟随重定向；curl multi 支持超时和主动取消。响应上限按解压后的内容计算，错误不回显服务端可能含密钥的文本。

可选语言为简体中文、英语、日语、韩语、德语、法语、西班牙语；源语言另有自动检测。提示词必须包含 `{{targetLanguage}}`，并可使用 `{{sourceLanguage}}`。长度沿用 UTF-16 代码单元，emoji 等补充平面字符算两个，不能以 UTF-8 字节数代替。

超时范围 1–600 秒，默认 30；输入上限 1–200000，默认 20000；响应上限 16–16384 KiB，默认 2048；译文字号 10–32。

### 百度 OCR

识别使用固定 HTTPS 地址的 `/oauth/2.0/token` 与 `/rest/2.0/ocr/v1/accurate_basic`，PNG 经 Base64 和表单转义提交，`language_type=auto_detect`。短边至少 15、长边最多 8192 像素，图像表单编码最多 10 MB，响应最多 2 MiB。

Token 只在内存缓存，按完整凭据区分，预留 60 秒过期裕量；错误 110/111 最多重新获取一次 token，所有网络阶段共用本次超时。空结果、权限和额度错误明确失败。重新翻译复用已识别文字，不重新发送图片；截图和译文不写入历史文件。

## 验证、运行与安装

### 可重复的业务回归

```sh
cmake --build build --target trans_business_test_driver
ctest --test-dir build -R native_business --output-on-failure
```

`tests/test_native_business.py` 启动本机临时 HTTP 服务，调用实际 C++/libcurl 和临时配置文件，覆盖 INI 转义/Unicode/迁移、JSON 优先级及损坏保护、提供商协议、完整译文、模型列表、输入边界、受管头/参数、响应限长（含 gzip）、错误脱敏、超时及请求发出后的取消。没有调用真实付费服务。

OCR 本地回归目前覆盖无效 PNG 和尺寸的发送前拒绝；**不等于有效图片的真实 OAuth、token 缓存/刷新、百度额度或端到端识别已经验收**。TLS 证书链、系统代理、Windows 原生交互和 Wayland Portal 也需要独立场景。旧 QtTest/QML 测试通过记录不能沿用为本实现的证据。

### 真实窗口冒烟

在可用的桌面会话中执行：

```sh
./build/trans --smoke-test
```

冒烟模式打开真实翻译和设置窗口，运行事件循环后输出 `TRANS_SMOKE_PASS` 并退出；不注册正常实例或全局快捷键，不读写用户配置，也不调用翻译 API。它需要实际显示服务，不支持旧的 Qt offscreen 环境变量。

无桌面的 Linux CI 可安装 Xvfb/Mesa 后执行：

```sh
sudo pacman -S --needed xorg-server-xvfb mesa
xvfb-run -a env SLINT_BACKEND=winit-femtovg LIBGL_ALWAYS_SOFTWARE=1 ./build/trans --smoke-test
```

这里的软件 OpenGL 是 Mesa llvmpipe，不是已禁用的 Slint software renderer。Xvfb 不提供真实 KDE 快捷键、托盘宿主或 Wayland Portal；不得把这条命令的成功扩展为这些服务已验收。

### 命令行及安装

```sh
./build/trans --help
./build/trans --version
./build/trans --settings
./build/trans --translate
./build/trans --ocr
cmake --install build --prefix "$HOME/.local" --component Runtime
```

常规重复启动通过平台单实例通道转交命令；更新程序前先从托盘退出旧实例。只安装 `Runtime` 组件，避免把 Slint SDK、头文件和构建工具一起打包。确认 `~/.local/bin` 在桌面会话 PATH 中。

### 发布前桌面验收

Windows 10 22H2、Windows 11、Plasma X11、目标 Wayland 桌面分别记录结果：中文/输入法/混合字体，选区安全限制，托盘重新打开，两快捷键冲突和交换，配置保存失败回滚，多屏负坐标与 100/125/150/200% DPI，截图不含自身窗口，Esc/右键取消，置顶/从属层级/焦点恢复，单实例就绪和跨会话隔离，以及退出后的线程/helper/快捷键释放。

还须检查成品 EXE/DLL/ELF 的直接和传递依赖、动态后端加载和许可证文件；没有 Qt 命名 DLL 不足以证明没有静态或传递 Qt。真实 API 验收会发送数据并可能计费，应使用获授权的测试账户和内容，不把密钥写入日志或截图。

## 打包与许可证

[Arch 工作流](../.github/workflows/package.yml) 使用官方 Arch 容器和系统原生库，不需要 Qt SDK。`packaging/prepare.py` 仍从 Git 提交归档源码，并检查标签与 CMake 项目版本一致。它不会打包未提交的文件或本地密钥。生成打包输入的命令为：

```sh
python3 packaging/prepare.py --output-dir dist/arch
bash -n packaging/PKGBUILD packaging/build-arch.sh packaging/verify-arch.sh
python3 -m unittest discover -s tests -p test_packaging.py -v
```

`packaging/build-arch.sh` 和 `packaging/verify-arch.sh` 面向一次性容器，会安装依赖、创建构建/验收用户；不要直接在日常宿主上以 root 试跑。发布版本和产物状态以对应 Actions 记录为准，文档不承诺尚未执行的 Windows 或容器流程通过。

Slint 的 [Royalty-free Desktop, Mobile, and Web Applications License 2.0](https://github.com/slint-ui/slint/blob/v1.18.1/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md)第 2(b) 条允许在易于找到的公开网页展示官方归属标识，优先选择应用下载页面。当前采用这一外部标识方式：README 下载说明附近展示固定到 v1.18.1 的官方 Made with Slint 标识，发布页面也应保留它；应用侧栏仅显示版本，不放置 Slint 标识或网站入口。分发包仍保留 Slint 和其他依赖的声明。维护者须核对其余授权要求，项目整体许可证尚未声明，不能把第三方许可证替代为项目许可证。

协议参考：[OpenAI Responses](https://developers.openai.com/api/reference/resources/responses/methods/create)、[OpenAI Chat Completions](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create)、[DeepSeek Chat Completion](https://api-docs.deepseek.com/api/create-chat-completion)。迁移边界和尚未取得的验收证据见[迁移记录](slint-refactor-plan.md)。
