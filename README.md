# 手键

手键是一款 Windows 端的手机键鼠控制工具。电脑显示二维码，手机扫码后即可在浏览器中控制电脑的鼠标和键盘。

程序使用原生 Win32 窗口，不依赖 WebView2。手机和电脑只需要连接到同一个局域网。

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

本项目源代码使用 [MIT License](LICENSE) 开源。

二维码生成代码来自 [Project Nayuki QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)，同样使用 MIT License。
