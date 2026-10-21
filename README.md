# 手键

手键是一款 Windows 端的手机键鼠控制工具。电脑显示二维码，手机扫码后即可在浏览器中控制电脑的鼠标和键盘。

程序使用原生 Win32 窗口，不依赖 WebView2。手机和电脑只需要连接到同一个局域网。

## 主要功能

- 手机触控板控制电脑鼠标。
- 支持鼠标、键鼠同屏、键盘三种模式。
- 支持单击、右键、滚动、长按拖拽和窗口调整。
- 键盘包含字母、数字、符号、F1–F12、Shift、Caps Lock、Ctrl、Alt、Win、方向键等常用按键。
- 「中/英」按钮发送一次独立的 Shift 按键，用于切换电脑当前输入法。
- 电脑端可调整鼠标灵敏度和滚动倍率，范围为 0.5×–3.0×，步长为 0.1×。
- 主窗口关闭后程序继续在系统托盘运行。
- 新手机连接时会自动断开旧连接，并释放旧连接遗留的按键和鼠标状态。

## 使用方法

1. 运行 `release\PhoneKeyboardC.exe`。
2. 确保手机和电脑连接到同一个局域网。
3. 使用手机扫描电脑窗口中的二维码。
4. 在手机页面中选择需要的模式，开始操作电脑。
5. 点击电脑窗口右上角的关闭按钮只会隐藏窗口，程序仍会在托盘运行。
6. 右键托盘图标可以重新打开主界面、刷新二维码、复制连接地址或退出程序。

如果手机无法连接，请检查电脑是否使用了可被手机访问的局域网 IPv4 地址，以及 Windows 防火墙是否允许程序通信。

## 构建

构建环境：

- Windows 10/11
- PowerShell
- Visual Studio Build Tools
- Desktop development with C++ 工作负载
- MSVC x64 工具集和 Windows SDK

在项目根目录执行：

```powershell
.\build.ps1
```

生成文件：

```text
release\PhoneKeyboardC.exe
```

## 测试

构建后可运行：

```powershell
node .\tests\controller-test.js
.\tests\protocol-test.ps1
.\tests\input-state-test.ps1
.\tests\hover-state-test.ps1
```

## 使用限制

- 仅支持 Windows。
- 目前使用局域网明文 HTTP/WebSocket，不适合直接暴露到互联网。
- 同一时间只允许一个手机控制端。
- 「中/英」和 Caps Lock 的状态由手机页面记录，不会读取电脑当前状态。
- 受 Windows 输入隔离保护的界面可能无法接收普通键鼠输入。

## 许可

二维码生成代码来自 [Project Nayuki QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)，使用 MIT License。
