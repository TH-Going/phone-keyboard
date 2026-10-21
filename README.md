# 手键 · 纯 C 版（Win32 / GDI）

手键让手机浏览器充当 Windows 电脑的远程键盘和鼠标：电脑端显示配对二维码，手机扫描后打开控制页面，输入事件通过 WebSocket 发送到电脑。

当前仓库是独立的 Win32 原生实现，网页资源在构建时嵌入可执行文件。电脑端不加载 WebView2，也不需要安装浏览器内核。

- **紧凑主窗口**：二维码窗口为 282×338 逻辑像素，右上角提供设置和关闭按钮；关闭后程序继续在通知区域运行。
- **原生绘制**：GDI 直接绘制二维码和界面，二维码模块按整数像素放大并保留静区。
- **手机页面**：内置 HTTP 服务提供 `controller.html`、`controller.css` 和 `controller.js`，支持横屏、竖屏、浅色和深色模式。
- **单控制端**：同一时间只保留一个手机 WebSocket 连接；新连接建立后会断开旧连接并清理旧连接的输入状态。

## 构建

需要 Windows 10/11（当前按 x64 构建并在 Windows 10/11 验证）、PowerShell，以及带以下组件的 Visual Studio Build Tools：

- Desktop development with C++ 工作负载；
- MSVC x64 工具集和 Windows SDK；
- `vswhere.exe`、`vcvars64.bat`、`cl.exe` 和 `rc.exe`。

脚本会通过 `vswhere.exe` 定位已安装的 MSVC 工具链，执行：

```powershell
.\build.ps1
```

产物为 `release\PhoneKeyboardC.exe`。构建过程还会生成 `build\web_assets.h` 和 MSVC 中间文件，这些文件已加入 `.gitignore`。

构建步骤如下：

1. `tools\embed.ps1` 以 UTF-8 字节数组嵌入 `web\controller.*`；
2. `rc.exe` 编译 `res\app.rc`，包含图标和 DPI manifest；
3. `cl.exe` 以 C17、x64、GUI 子系统编译 `src\*.c` 并链接可执行文件。

发布文件是单个 EXE，运行时不依赖 WebView2；构建使用 MSVC 的动态运行库，目标机若没有对应的 Visual C++ 运行库，需先安装 Microsoft Visual C++ Redistributable。

## 使用

1. 运行 `release\PhoneKeyboardC.exe`。程序启动 HTTP/WebSocket 服务，显示「手键」二维码窗口，并添加托盘图标。
2. 手机与电脑连接到同一个局域网，用手机扫描二维码打开控制页面。二维码中的地址形如 `http://<局域网 IPv4>:<端口>/c/<32 个十六进制字符令牌>`。
3. 手机页面默认显示键盘和触控板。拖动中间细条可以在鼠标模式、键鼠同屏和键盘模式之间切换。
4. 关闭按钮或 Alt+F4 只隐藏二维码窗口，连接和托盘继续运行；单击托盘图标或再次运行程序可以重新打开已有窗口。
5. 托盘右键菜单提供：打开主界面、重新生成二维码、复制链接、退出。

服务器只监听 IPv4。程序会优先选择可路由的本机地址，找不到时回退到 `127.0.0.1`；如果二维码显示回环地址，手机无法从局域网连接，应检查网卡、防火墙和网络隔离设置。首次监听局域网时，Windows 防火墙可能会询问是否允许专用网络入站。

### 电脑端窗口

| 操作 | 行为 |
| --- | --- |
| 悬停或移开托盘图标 | 不改变主窗口的显示状态 |
| 左键单击托盘图标 | 打开窗口并置于前台 |
| 关闭按钮 / Alt+F4 | 隐藏窗口，手机连接和托盘继续运行 |
| 拖动顶部标题区域 | 移动窗口；本次运行期间重新打开时保留位置 |
| 设置按钮 | 打开鼠标灵敏度和滚动倍率滑条 |
| 返回按钮 / 完成按钮 | 保存设置并回到二维码页 |
| 重新生成二维码 | 生成新令牌并断开当前手机连接 |

设置页提供两个独立倍率：鼠标灵敏度和滚动倍率，范围 `0.5×～3.0×`，步长 `0.1×`，显示一位小数。拖动时立即作用于已连接的手机，鼠标倍率也适用于长按拖拽；松手、键盘调整结束、离开设置页或关闭窗口时保存到当前 Windows 用户的 `HKCU\Software\PhoneKeyboardC`。旧配置中的 `0.75×` 和 `1.25×` 会分别按最近档位恢复为 `0.8×` 和 `1.3×`。设置不会修改系统鼠标速度。

### 手机页面

页面使用 Pointer Events、`setPointerCapture`、WebSocket 和 `requestAnimationFrame`，建议使用较新的 Android Chrome 或 iOS Safari。IE 和不支持这些 API 的旧 WebView 不在支持范围内。页面会根据系统偏好切换浅色/深色，并为刘海屏使用安全区域；`navigator.vibrate` 仅用于可选的触感反馈。

| 操作 | 行为 |
| --- | --- |
| 分隔条向左拖动 | 每次向鼠标方向切换一档：键盘 → 同屏 → 鼠标 |
| 分隔条向右拖动 | 每次向键盘方向切换一档：鼠标 → 同屏 → 键盘；垂直拖动不会切换 |
| 分隔条左右方向键 | 切换模式；Home 进入鼠标模式，End 进入键盘模式 |
| 单指在触控板滑动 | 移动电脑鼠标 |
| 单指轻点 | 左键单击 |
| 双指快速轻点 | 右键单击 |
| 双指滑动 | 按 120 刻度累计后发送滚轮增量 |
| 稳定长按约 450 毫秒后移动 | 按住左键拖动，抬手释放 |
| 长按触发前移动超过 6px | 取消长按，改为普通鼠标移动 |
| 拖拽中加入第二根手指 | 先释放左键，并抑制本次手势后续点击和滚动 |
| Shift / Ctrl / Alt / Win | 本地选中，应用于下一次按键或鼠标操作后释放 |
| Caps Lock | 独立切换大写；与 Shift 同时选中时临时小写 |
| 中/英 | 按钮固定显示「中/英」，发送一次独立的 Shift 按键，使用电脑输入法已有的切换行为 |
| 123 / ABC | 切换字母与数字符号层；两层均保留 F1–F12、Shift 和 Caps Lock |

键盘层包含 F1–F12、Esc、Tab、Caps Lock、Ctrl、Alt、Win、字母、Shift、Backspace、逗号、句号、空格和回车。数字层的符号按标准美式物理键和 Shift 组合发送。切换模式、触摸取消、失焦、切后台、窗口尺寸/方向变化、离线或 WebSocket 断开时都会释放输入状态。

横屏同屏布局中键盘与触控板约为 7:3；窄竖屏会改为上下排列，较短的屏幕可以滚动查看。触屏设备上的分隔条操作区为 24px，鼠标设备为 18px。

## 连接和协议

配对令牌为每次启动随机生成的 32 个十六进制字符（128 位）字符串，也可以通过命令行固定。页面和 WebSocket 都校验令牌：

```text
GET /c/<token>       手机控制页面
GET /assets/controller.css
GET /assets/controller.js
GET /ws/<token>      WebSocket 升级
```

控制页面响应带有同源 CSP、`Referrer-Policy: no-referrer`、`X-Content-Type-Options: nosniff` 和 `Cache-Control: no-store`。

控制消息是 JSON 文本帧，服务端支持 `move`、`click`、`button`、`wheel`、`key` 和 `release`。服务端每 15 秒发送一次 ping，45 秒没有收到 pong 会断开连接并释放输入；手机端断线后会自动重连，并先清理本地按键和触控状态。

服务使用局域网明文 HTTP/WS，不提供 TLS。令牌应视为配对凭据，只在可信网络中使用，并避免把二维码或链接分享给不需要控制权限的人。

## 命令行参数

正常使用不需要参数。以下选项主要用于测试、预览和排查：

| 参数 | 说明 |
| --- | --- |
| `--help`、`-h`、`/?` | 显示帮助并退出 |
| `--port <n>` | 固定监听端口；省略时由系统分配空闲端口 |
| `--token <32 hex>` | 使用指定的 32 个十六进制字符（128 位）令牌；无效值会被忽略 |
| `--log <file>` | 追加诊断日志（当前内容为 ASCII 兼容文本） |
| `--state-file <file>` | 原子写出测试状态：`rect`、`rect_ok`、`visible`、`settings`、`mouse_speed`、`scroll_speed` |
| `--preview <ms>` | 兼容旧参数；启动时显示主窗口，不再按毫秒数自动隐藏 |
| `--render-card <bmp>` | 离屏渲染二维码窗口为 BMP 后退出，不启动 HTTP 服务、不监听端口和不建托盘 |
| `--qr-dump <txt>` | 导出二维码 URL、尺寸和模块矩阵后退出 |
| `--no-tray` | 不创建托盘图标；默认不显示窗口。与 `--preview` 一起使用时，关闭窗口会退出 |
| `--dry-run` | 运行协议但不调用真实键鼠注入 |
| `--quit-after <ms>` | 延时自动退出 |
| `--allow-multi` | 跳过单实例限制；正常再次启动会唤起已有实例并退出 |

## 代码结构

```text
src/app.h        共享类型、常量和模块接口
src/main.c       入口、托盘图标、菜单、倍率持久化、命令行和消息循环
src/card.c       GDI 二维码窗口、图标按钮、原生滑条、DPI 适配
src/qrcodegen.c  Nayuki QR Code 生成库（MIT，见下方许可）
src/server.c     HTTP + WebSocket 服务、令牌和单控制端管理
src/input.c      SendInput 键鼠注入、104 项按键表和 JSON 输入处理
src/motion.c     鼠标移动平滑（4ms 步进、16ms 窗口）
src/net.c        局域网 IPv4 地址选择
src/sha1.c       SHA-1 + Base64（WebSocket 握手）
src/util.c       日志、UTF-8 转换、令牌、JSON 取值和时间工具
res/             图标、manifest、资源脚本
web/             手机端 HTML/CSS/JavaScript 页面
tests/           协议、输入状态、窗口托盘和前端行为测试
tools/embed.ps1  网页资源内嵌脚本
```

主窗口使用普通 Win32 弹出窗口、GDI 双缓冲和原生按钮/Trackbar，不置顶、不穿透鼠标。应用通过 manifest 和运行时设置启用 Per-Monitor V2 DPI 感知；二维码目标区域为 230×230 逻辑像素。每次点击、滚轮或按键操作前都会先冲刷待处理的鼠标移动，保证移动和后续操作的顺序。

## 验证

先运行构建，再执行与环境匹配的测试：

```powershell
node --test .\tests\controller-test.js
.\tests\protocol-test.ps1
.\tests\input-state-test.ps1
.\tests\hover-state-test.ps1
.\tests\hover-test.ps1
```

- `controller-test.js` 使用 Node.js 内置 `node:test`，建议 Node.js 18 或更高版本；不需要额外依赖。
- `protocol-test.ps1` 和 `hover-test.ps1` 使用 `release\PhoneKeyboardC.exe`，前者检查 HTTP/CSP/令牌/WebSocket，后者启动真实进程检查窗口、托盘和关闭后服务。`hover-test.ps1` 支持 `-Executable` 指定程序，支持 `-SnapshotDir` 保存窗口截图，并需要可交互的 Windows 桌面会话。
- `input-state-test.ps1` 和 `hover-state-test.ps1` 使用构建生成的 `.obj`，分别替换键鼠注入/移动模块和注册表访问，不会移动真实光标或修改真实设置。
- `tests\keymap-test.ps1` 是可选的历史交叉核对脚本，会读取仓库外的 `..\code\src-tauri\src\input.rs`。当前仓库没有该 Rust 源文件；没有外部原实现时应跳过此脚本。
- `--qr-dump` 可用于把当前令牌对应的二维码矩阵导出给外部实现核对，仓库不包含额外的 Rust 参考实现。

## 已知边界

- 只支持 Windows；当前发布目标为 x64，已在 Windows 10/11 验证。
- 服务只使用 IPv4 和明文 HTTP/WS；无可用局域网 IPv4 时会回退到 `127.0.0.1`，手机无法远程连接。
- 登录界面、UAC 提权窗口等受 Windows 输入隔离保护的界面，可能收不到普通 `SendInput`。
- 未实现按键长按连发；触控板长按用于左键拖拽。
- 「中/英」、标点显示和 Caps Lock 高亮记录手机页面的本地状态，不读取电脑当前输入法或锁定键状态；电脑端手动切换或重新加载页面后可能不同步。
- 数字符号发送美式物理键和 Shift，中文标点由电脑当前输入法处理。
- 网络无声中断依赖 15 秒 ping 和 45 秒 pong 超时，不能保证立即松开输入。
- 同一时间只允许一个活动手机控制端，新连接会断开旧连接。

## 许可

`src/qrcodegen.c` / `src/qrcodegen.h` 来自 [Project Nayuki 的 QR Code generator](https://www.nayuki.io/page/qr-code-generator-library)（MIT License），文件头保留了原始版权声明。
