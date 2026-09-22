# Trans

在浏览器、编辑器或终端中选中文字，一键查看译文；遇到图片、视频字幕或扫描版 PDF，框选文字区域即可识别并翻译。

Trans 常驻系统托盘，支持 OpenAI 和 DeepSeek，默认翻译为简体中文。**已验收环境为 Arch Linux x86_64、KDE Plasma 6 的 X11 会话；新增 Windows 10 22H2 / Windows 11 x64 后端与打包流程，Windows 桌面交互仍待原生环境验收。** 需要自行配置翻译服务的 API 密钥；截图翻译另需百度 OCR 密钥，费用由对应服务计收。

| 选区翻译 | 截图翻译 |
| --- | --- |
| 选中文字，按 **Meta+Shift+T** | 按 **Meta+Shift+O**，鼠标拖动框选 |
| 无需先复制，适用于单词、句子和段落 | 松开鼠标后自动识别并翻译框内文字 |

以上为 Linux 默认快捷键，Meta 通常是键盘上的 Windows 键。Windows 默认使用 **Ctrl+Alt+T**（选区翻译）、**Ctrl+Alt+O**（截图翻译）。两种快捷键都可以在设置中修改。

| 深色模式 | 浅色模式 |
| --- | --- |
| ![Trans 深色翻译窗口](docs/images/translation-dark.png) | ![Trans 浅色翻译窗口](docs/images/translation-light.png) |

## 安装

### Arch Linux

1. 前往 [Releases](https://github.com/BenZinaDaze/trans/releases)，下载同一版本的 `.pkg.tar.zst` 安装包和 `SHA256SUMS`。
2. 在下载目录打开终端，更新系统、校验并安装。例如安装 1.0.0：

   ```sh
   sudo pacman -Syu
   sha256sum --check SHA256SUMS
   sudo pacman -U ./trans-1.0.0-1-x86_64.pkg.tar.zst
   ```

3. 从应用启动器搜索 **Trans** 并打开，或执行 `trans`。

安装包会自动安装 Qt/KDE 依赖，无需自行编译。其他版本请替换命令中的文件名。

不确定当前会话类型时，执行 `echo "$XDG_SESSION_TYPE"`，应输出 `x11`。Wayland 暂不作为支持环境。

### Windows x64

最低目标版本为 Windows 10 22H2，Windows 11 共用同一个 x64 程序包；不提供 ARM64 或 32 位版本。

1. 新 Windows 工作流运行成功后，可从 [Actions](https://github.com/BenZinaDaze/trans/actions) 的 **Windows x64 package** 下载构建产物；对应新版本标签发布成功后，ZIP 和 `SHA256SUMS-windows-x64` 也会出现在 Releases。
2. 用 PowerShell 的 `Get-FileHash -Algorithm SHA256 .\trans-<版本>-windows-x64.zip` 核对校验文件中的值。
3. 将 ZIP 完整解压到可写目录，运行其中的 `trans.exe`。不要只复制 EXE；`trans_selection_helper.exe`、Qt DLL、QML 模块和插件必须保留。程序包包含运行库，无需安装 Qt SDK。

使用普通用户运行即可，不要求管理员权限。程序包未进行代码签名，Windows 可能显示来源警告；请核实下载来源与校验值。Windows 10 的系统安全维护情况另由微软的版本和更新政策决定，程序能运行不代表系统仍受安全支持。

## 首次使用

首次启动会打开配置中心。以后可以从托盘菜单 → **设置…**，或翻译窗口右上角的设置图标进入。

1. 在 **翻译服务** 中选择 OpenAI 或 DeepSeek。
2. 填写 API 密钥。使用官方服务时保留默认基础地址；使用代理服务时填写其提供的地址。
3. 点击 **获取模型**，选择账户可用的模型，也可以直接输入模型 ID。
4. 点击 **测试翻译**，确认成功后点击 **保存全部**。
5. 关闭设置，在其他应用中选中文字，按选区翻译快捷键：Linux **Meta+Shift+T**，Windows **Ctrl+Alt+T**。

![Trans 翻译服务设置](docs/images/settings-dark.png)

测试翻译使用当前填写的配置，会发送一次正常计费请求，**测试成功后仍需保存**。各页共用“保存全部”，关闭设置会放弃未保存的修改。OpenAI 和 DeepSeek 的配置分别保留，当前选中的服务用于选区翻译和截图翻译。

想翻译成其他语言，在 **翻译偏好** 中修改目标语言。需要调整快捷键、字号或置顶行为，打开 **快捷键与窗口**。窗口会随内容自动调整大小，长文本可滚动阅读。

## 使用截图翻译

截图翻译适合无法直接选中文字的内容，例如图片、视频字幕和扫描版 PDF。X11 和 Windows 使用内置框选，无需安装额外截图工具。

1. 在百度智能云开通 **通用文字识别（高精度版）**，获取应用的 API Key 和 Secret Key。
2. 在设置 → **截图 OCR** 填入这两个密钥，点击 **保存全部**。
3. 按截图翻译快捷键：Linux **Meta+Shift+O**，Windows **Ctrl+Alt+O**。屏幕变暗后，按住鼠标左键拖出矩形，松开后自动识别并翻译。

![Trans 百度 OCR 设置](docs/images/ocr-settings-dark.png)

按 **Esc** 或鼠标右键取消框选。可在任意一块屏幕内框选，暂不支持跨屏拖选；选框太小或只是单击时，会继续等待框选。

Windows 使用桌面截图 API；受保护内容可能呈现黑色，程序不能保证检测或捕获 DRM/受保护画面。截图期间若显示器布局或缩放改变，本次框选会取消，需要重新截图。

识别后的翻译窗口会显示 **“来自 OCR 内容”**。点击“重新翻译”会复用识别文字，不重复调用 OCR；需要重新识别时，再次框选即可。OCR 和翻译分别计费，未配置 OCR 不影响选区翻译。

## 常用操作

| 操作 | 效果 |
| --- | --- |
| 选区快捷键 / 托盘 → **选区翻译** | 翻译当前选中文字 |
| 截图快捷键 / 托盘 → **截图翻译** | 框选图片中的文字并翻译 |
| **Esc** / 关闭翻译窗口 | 收起窗口并取消正在进行的请求 |
| 点击托盘图标 | 查看上次原文和译文，不重新请求服务 |
| **复制译文** | 将译文放入剪贴板 |
| **重新翻译** | 使用当前配置再次翻译，会重新计费 |
| 托盘 → **退出 Trans** | 完全退出程序 |

关闭窗口后，Trans 仍在托盘运行。程序不自动配置开机启动；Linux 可在 KDE 系统设置的 **自动启动** 中添加 Trans，Windows 可自行把程序快捷方式放入当前用户的启动文件夹。

## 常见问题

### 快捷键没有反应或读不到文字

Linux 请确认使用 Plasma X11，并在原应用中选中了可复制的文字。Windows 通过 **UI Automation TextPattern** 读取选区，不模拟 Ctrl+C，也不覆盖剪贴板；目标控件必须提供可访问的文字选区。不支持的控件、密码框、受保护窗口或更高权限应用会返回失败；可对允许截图的内容使用截图翻译。Windows 是否接受浮窗激活、焦点恢复由系统决定。

如果快捷键冲突，在 **快捷键与窗口** 点击“录制”，换一个包含 Ctrl、Alt 或 Meta 的组合，然后保存。两种翻译入口不能使用相同快捷键。

### 翻译提示密钥、模型或接口错误

检查密钥和账户可用模型，再点击“测试翻译”。默认模型只是预设，可能不在你的账户可用范围内。

使用代理服务时，基础地址不要包含 `/responses`、`/chat/completions` 或 `/models`；OpenAI 的 API 模式应与服务支持的模式一致。模型不支持推理参数时，将推理级别改为“服务默认”。连接较慢时，可在 **翻译偏好** 中增加超时。

### OCR 失败或识别不准确

确认百度应用已开通高精度文字识别接口，并且额度可用。尽量只框选清晰的文字区域，避免把多栏内容混在一起；提示图片过大时，缩小选框后重试。

### 中文显示为方框

Arch Linux 可安装中文字体后重启 Trans：

```sh
sudo pacman -S --needed noto-fonts-cjk
```

### 怎样更新或卸载

**更新：** 从 [Releases](https://github.com/BenZinaDaze/trans/releases) 下载新版本，校验后先从托盘退出旧实例，再安装或解压新版本并启动。重复启动不会替换正在运行的旧实例。Windows 应整体更换解压目录，不能只替换主 EXE。

**卸载：** 先从托盘退出。Arch Linux 执行 `sudo pacman -R trans`，Windows 删除解压目录。卸载均会保留配置；需要清除密钥时，删除下述配置文件及自行添加的启动项。

<details>
<summary>从旧版 Tran 升级</summary>

项目从 1.0.0 起更名为 Trans，命令和包名为 `trans`。启动前请先退出旧版 Tran，并把自动启动项改为 Trans。新版不读取或导入旧配置，需要重新填写翻译和 OCR 设置；旧配置保持不动。如果快捷键冲突，请在 KDE 系统快捷键设置中清除旧 Tran 的绑定。

</details>

## 配置与隐私

- 选区翻译将选中文字发送给当前翻译服务。截图翻译只将框选区域发送给百度 OCR，再将识别文字发送给翻译服务。
- 截图、最近一次原文和译文仅保留在内存中，退出后清除；不保存翻译历史。除“复制译文”外，不修改普通剪贴板。
- Linux 配置位于 `~/.config/trans/settings.ini`，遵循 `XDG_CONFIG_HOME`；Windows 使用 Qt 的当前用户应用配置目录中的 `settings.ini`，通常为 `%LOCALAPPDATA%\trans\settings.ini`。**API 密钥以明文保存在本地**，不接入系统密钥库。Linux 目录和文件权限分别为 `0700`、`0600`；Windows 使用仅当前用户和 SYSTEM 可访问的受保护 DACL，不能建立权限时保存失败。分享或备份时请留意其中的密钥。

## 开发与反馈

遇到问题或有功能建议，请提交 [Issue](https://github.com/BenZinaDaze/trans/issues)，附上系统环境、软件版本和复现步骤，不要公开 API 密钥。

自行编译、测试和发布说明见 [开发与维护指南](docs/development.md)。界面参考 [Pot Desktop](https://github.com/pot-app/pot-desktop)。
