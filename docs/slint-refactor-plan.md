# C++ + Slint 去 Qt 迁移记录与验收清单

状态：当前源码已经切换为 C++20 + Slint 的原生实现；本文不再是“只提出方案、不修改代码”的旧计划。对照版本为 Trans v1.2.0。**实现已落地、局部验证已通过，不代表全部平台的正式发布验收已经完成。**

## 一、目标与当前结论

用户要求 Windows 与 Linux 共用 Slint 界面，业务继续使用 C++，主程序、取词 helper、业务库、测试及打包工具链不依赖 Qt/KDE Frameworks。不保留混合 GUI、Qt fallback 或外部 Qt 配置迁移程序。

当前构建入口为根 `CMakeLists.txt`，业务和平台源码位于 `src/native/`，共享界面位于 `ui/app.slint`。Slint 1.18.1 使用 Winit + FemtoVG，在编译时关闭 Qt 后端；Rust 1.98.1 用于构建 Slint，不改变应用业务语言。

“去 Qt”指 Trans 自身的构建、链接及分发依赖。KDE KGlobalAccel、Portal 等桌面服务可能由 Qt 实现，它们是用户系统服务，不属于 Trans 的链接库或待重写代码。

### 已取得的证据

- Linux 原生主程序构建成功，真实 Slint 翻译窗口和设置窗口的 `--smoke-test` 已输出 `TRANS_SMOKE_PASS`。
- `native_business` CTest 已通过：使用实际 C++/libcurl、本地 HTTP 服务及临时配置，验证协议、配置导入、边界和取消。
- 实际 UI 已连接本地 HTTP 服务发起请求；X11 PRIMARY 读取与第二实例命令转发已经在运行路径中执行验证。
- Windows 原生后端及 helper 通过 MinGW 交叉语法检查。**这不是 MSVC 构建、Windows 桌面执行或成品包验收。**

这些记录不覆盖真实外部翻译/OCR 服务、全部窗口层级/焦点交互、目标 Wayland Portal、Windows 10/11、多屏 DPI 或最终包体积与资源占用。不得沿用旧 Qt 版本的截图、测试通过记录或体积数据作为新实现的验收证据。

## 二、已落地结构

| 能力 | 当前实现 | 边界 |
| --- | --- | --- |
| 翻译/设置/托盘/框选 | `ui/app.slint`，Slint 生成的 C++ 绑定 | 一个 Slint GUI 事件循环 |
| 启动、草稿、任务代次 | `src/native/main.cpp` | 主线程更新 UI，后台任务显式投递结果 |
| 翻译/模型/OCR | `src/native/business.h/.cpp` | 同步可取消接口、libcurl multi、nlohmann/json |
| 配置读取与校验 | 业务模块，版本化 JSON、旧 INI 导入 | 不链接 Qt，不在读取函数内写入文件 |
| 安全配置写入 | 平台实现 | 同目录替换、权限/ACL，不回退为无保护保存 |
| Linux 取词/窗口/截图 | XCB/RandR、EWMH、libdbus Portal | 不读取旧剪贴板冒充选区 |
| Linux 快捷键/单实例 | 直接 D-Bus 客户端 | 保留 KDE action ID 和现有四个实例命令 |
| Windows 取词 | Win32 父进程 + COM MTA/UIA helper | 版本化限长协议、Job Object、来源身份校验 |
| Windows 快捷键/截图/窗口 | Win32 消息线程、GDI/DWM、原生 HWND | 不依赖 Qt 原生事件过滤 |
| Windows 单实例 | 命名互斥、受限命名管道 | 当前用户/登录会话隔离 |
| 图像编码/解码 | libpng 与 RGBA 缓冲区 | 明确尺寸、坐标和字节数边界 |
| 回归 | C++ 原生驱动 + Python 标准库 HTTP/文件夹具 | 不运行 QtTest 或 QML |

主要构建目标为 `trans_business`、`trans`、Windows 的 `trans_selection_helper`，以及启用测试时的 `trans_business_test_driver`。详细依赖和真实构建命令见[开发指南](development.md)。

## 三、Slint SDK 的明确维护成本

默认源码构建锁定 Slint **1.18.1**、Rust **1.98.1** 和 Corrosion 提交，启用 Winit/FemtoVG、可访问性与系统托盘，禁用 Qt、Skia、Vello、LinuxKMS 和 Slint 软件渲染器。不能仅用运行时环境变量声称禁用了 Qt。

原设计优先要求上游公开窗口接口。实际遇到的边界是：1.18.1 C++ 接口没有所需 X11 XID 访问器。当前采用明确可见的 [`cmake/slint-x11-window.patch`](../cmake/slint-x11-window.patch)，向 C++ 窗口对象增加 `x11_window_id()`，由同一 Slint adapter 的 raw-window-handle 返回 Xlib/XCB ID，Wayland 或未创建窗口返回 0。

这是一项**本仓库维护的 SDK 源码补丁**，不能伪称无需修改上游，也不能把升级维护成本藏在打包脚本中。补丁不扫描全局窗口、不猜测 PID/标题，也不强转 Slint 内部对象。自备 SDK 必须使用匹配的补丁头文件、生成绑定及运行库，不能混搭官方未修改的 Linux SDK。升级 Slint 时必须检查补丁和 ABI、重跑真实窗口验收；移除补丁的前提是上游提供等价的可用接口。

系统托盘由 Slint 提供，Linux 使用桌面 SNI/菜单服务，Windows 使用原生托盘路径。应用侧栏仅保留版本信息；Slint 归属标识采用 [Royalty-free 2.0 第 2(b) 条](https://github.com/slint-ui/slint/blob/v1.18.1/LICENSES/LicenseRef-Slint-Royalty-free-2.0.md#2-license-conditions---attribution)的外部网页方式，在 README 下载说明和公开发布页面展示易于找到的官方 Made with Slint 标识。打包仍保留第三方声明，其余授权条件仍须遵守；项目整体许可证未声明，不因引入 Slint 而自动获得某个项目许可证。

## 四、业务与安全契约

### 并发及结果所有权

Slint 主线程拥有全部窗口状态。翻译、模型查询、OCR、平台 I/O 通过可取消工作任务执行；业务使用 `std::stop_token`，curl multi 的 wakeup 及时唤醒取消。主请求与提供商工具请求分别维护代次，迟到结果不得覆盖新的文本、服务标签或错误。

关闭浮窗隐藏窗口并取消当前请求，托盘可重新打开；退出应先停止命令接收，取消并回收 helper/工作线程，最后释放平台资源和 UI。窗口激活与焦点恢复始终只是系统请求，不模拟 Alt 或强行抢焦点。

### 服务协议

保留 OpenAI Responses/Chat、DeepSeek Chat、自定义地址、模型、API Key、温度开关、输出 token 限制、推理级别、请求头、高级参数、模型发现及草稿试译。Responses 设置 `store=false`，不接受 incomplete；Chat 不发布截断文本，DeepSeek 不显示 reasoning_content。托管字段不允许通过自定义参数或请求头覆盖。

输入按有效 UTF-8 解码并保持原 UTF-16 代码单元上限，Unicode 空白判断不能改成 ASCII-only，不能按字节数限制中文。TLS/代理证书校验开启，自动重定向关闭，错误不回显可能含秘密的服务端内容；响应限长作用于解压后的字节，JSON 嵌套深度也有限制。

百度 OCR 固定使用 HTTPS OAuth 与 accurate_basic。PNG 表单编码、图片尺寸、10 MB 编码上限、2 MiB 响应、token 凭据隔离/过期和最多一次 110/111 刷新已实现。OCR 后只将识别文字交给翻译服务，重试翻译不重复识别；实际百度服务仍需有效凭据验收。

### 平台能力不能假成功

- X11 PRIMARY 使用实际选区协议，支持 TARGETS/UTF8_STRING、INCR、超时、取消和所有者消失，不调用 xclip/wl-copy 作为隐藏依赖。
- KDE 快捷键保留 `trans` 组件和 `translate-selection` / `translate-screenshot`，直接访问服务；不存在服务或注册冲突必须报错，不擅自启动其他快捷键框架。
- Wayland 不支持任意应用选区读取。Portal 只有明确支持版本 3 区域目标时才请求 `target=4`，不能上传整屏冒充区域。
- Windows UIA helper 拒绝密码/高权限/无效来源，验证 HWND/PID/TID 和焦点，限制运行时间与输出；不模拟复制、不覆盖普通剪贴板。
- Windows 命名管道与 Linux D-Bus 保留四个固定实例命令，就绪前后及失败确认语义必须真实，不将“已收到”说成“翻译完成”。
- Slint 逻辑坐标和截图物理像素必须正确映射；屏幕负坐标、缩放、热插拔、自身窗口隐藏以及设置窗口所有者/层级需实机证明。

## 五、用户配置迁移

新格式为现有配置目录内的 **`settings.json`，`schemaVersion=1`**。Linux 遵循 XDG 配置目录；Windows 使用当前用户 LocalAppData。没有把用户目录迁到另一位置。

1. 优先读取 JSON。只有 JSON 不存在时才导入同目录 INI。
2. INI 导入覆盖原应用产生的分组/反斜线键、百分号转义键、UTF-8、引号与换行、十六进制/代理对 Unicode、JSON 字符串、布尔及数字。
3. 旧 OpenAI 有 endpoint 但没有 apiMode 时，保留 Chat Completions，reasoning 使用服务默认；密钥、模型和其他字段保留。
4. `loadSettings` 只读取。完整校验和平台私有原子写入都成功后才启用导入结果，原 INI 保留。
5. JSON 受损、缺字段或未知版本必须显式报错，不读取旧 INI 覆盖新设置，不用空配置覆盖密钥。修复页面草稿不等同于激活无效配置。
6. Linux 目录/文件为 `0700`/`0600`；Windows 为当前用户和 SYSTEM 的受保护 DACL。原文件与临时文件安全失败不允许降级写入。

配置仍是本地明文，而非加密密钥库。删除或分享配置时须同时处理保留的 INI。更早改名前的 `tran/settings.ini` 不在本次导入范围。

## 六、构建、回归与打包入口

```sh
rustup toolchain install 1.98.1 --profile minimal
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build -R native_business --output-on-failure
./build/trans --smoke-test
cmake --install build --prefix "$HOME/.local" --component Runtime
```

`--smoke-test` 需要实际显示环境。Linux CI 可用 Xvfb + Mesa llvmpipe 跑 Winit/FemtoVG，不使用 Qt offscreen，也不把它称为真实 KDE/Wayland 验收。原生回归不注册实际用户快捷键，不访问付费服务。

Runtime 组件只安装应用运行所需文件，不把 Slint SDK 或编译器装入发行包。Linux 包含实际 Slint 动态库及其原生依赖；Windows 包含主程序、helper、Slint、curl/TLS、PNG、CRT 及第三方声明。打包不运行 windeployqt，不打包 QML 模块或 Qt 插件。Arch 和 Windows 构建/分发以各自工作流的实际执行记录为准。

### 本地回归覆盖与空白

已通过的 `native_business` 覆盖真实 HTTP 请求体/路径/头、Responses/Chat/DeepSeek 解析、完整性、模型列表、UTF-16 边界、自定义参数拒绝、重定向阻止、gzip 响应上限、JSON 嵌套、鉴权错误脱敏、超时/取消，以及临时目录中的 INI/JSON 迁移和损坏保护。

尚未由该回归证明：有效图片的百度 OAuth/cache/refresh、真实 CA/代理链、平台 IPC 身份和窗口状态、多屏区域像素正确性、桌面快捷键/托盘/Portal 的服务重启。测试中 OCR 的无效 PNG/尺寸拒绝不应被写成完整 OCR 通过。

## 七、正式发布前仍须完成的验收

### 零 Qt 与分发

- 在无 Qt/KF SDK 的构建环境完成目标平台构建；检查源码 include、CMake 目标、静态链接映射。
- 检查 EXE/ELF 的直接与传递依赖、运行时加载及包内文件，不出现 Qt/KF 或插件。KDE 桌面服务自身依赖 Qt 不等于 Trans 链接 Qt。
- 普通用户干净环境中运行完整包，不依赖开发机 PATH、SDK、插件路径或 `/tmp` 中的构建库。
- 核对 Runtime 安装内容、运行库搜索路径及完整的第三方许可证/通知。

### 功能、安全、桌面

Windows 10 22H2 与 Windows 11 x64、Plasma X11、拟支持的 Wayland 桌面分别记录：

- 中文/日文/韩文/emoji、输入法组合输入、文本选择复制和长文本滚动。
- 选区读取、密码/权限拒绝、清晰区域截图、真实 OCR→翻译、模型查询及草稿试译。
- 两快捷键注册、冲突、交换、外部变更、服务重启、保存失败及回滚失败、退出释放。
- 托盘显示/菜单/重新打开，两个窗口关闭隐藏，Esc、置顶、所有者层级、系统拒绝激活及来源焦点恢复。
- 100/125/150/200% DPI、多个屏幕与负坐标、反向框选、取消/热插拔时序和自身窗口排除。
- 快速重复请求、超时、取消、退出、迟到回调、helper 卡死与回收。
- 单实例并发启动、就绪与确认、主实例失败、不同用户及登录会话隔离。
- 配置路径 Unicode、目录/文件/临时文件权限、原子替换失败不损坏现有内容、秘密不进入日志。

### 体积和效率：目标，不是已测结果

保留原建议目标：完整 Windows 解压包争取不超过 **50 MiB**，同时相对实际 v1.2.0 基线减少至少 **50%**。统计主程序、helper、全部运行库、资源、字体和许可证，调试符号另列；同时报告 ZIP 和解压逻辑字节。不能以空窗口样例或仅 EXE 大小代替完整包。

同机比较托盘隐藏 60 秒 CPU/内存、冷/热启动、重复唤起中位数/P95，主程序与 helper 合并统计。Windows 使用私有提交/工作集，Linux 使用 RSS/PSS。若性能恶化超过 10% 且超出噪声，先分析，不默默放宽目标。当前本文没有这些性能或成品体积的测量结果。

## 八、后续变更原则与参考

本次不是业务语言改为 Rust。未来是否迁移 Rust，应由当前 C++ + Slint 的外观、功能、维护和性能验收决定；不提前增加 FFI/RPC 框架或第二 GUI 路径。

优先修复真实状态机与平台边界，而不是隐藏错误、返回假成功、扫描系统窗口或以默认配置覆盖用户秘密。旧实现只作为历史对照，不作为运行 fallback；文档和截图须随实际实现更新。

参考：[Slint C++ API](https://docs.slint.dev/latest/docs/cpp/)、[窗口 API](https://docs.slint.dev/latest/docs/cpp/api/slint/window/)、[渲染器与后端](https://docs.slint.dev/latest/docs/slint/guide/backends-and-renderers/)、[Slint licensing](https://slint.dev/pricing)、[开发与维护指南](development.md)。版本升级时以锁定版本源码和实际构建输出为准，不用 latest 文档代替兼容性验证。
