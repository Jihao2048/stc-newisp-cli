# newisp

STC 全系列单片机跨平台烧录工具（命令行版）。

本仓库是 [New ISP](https://github.com/Jihao2048) 图形版（WinUI 3 / C++/WinRT）的**跨平台移植**：
协议层代码原样搬过来，UI 换成了 CMake 构建的命令行程序，同一个代码库在
**Windows、macOS、Linux** 上都能编译运行。

三个平台**都已在真实硬件上验证通过**。

---

## 目录

- [特性](#特性)
- [支持的芯片](#支持的芯片)
- [两种传输方式](#两种传输方式)
- [编译](#编译)
  - [Windows](#windows)
  - [Linux](#linux)
  - [macOS](#macos)
  - [不带 HID 支持编译](#不带-hid-支持编译)
- [编码注意事项](#编码注意事项)
- [用法](#用法)
  - [命令](#命令)
  - [参数](#参数)
  - [示例](#示例)
- [测试](#测试)
- [代码结构](#代码结构)
- [硬件相关说明](#硬件相关说明)
- [验证状态](#验证状态)
- [已知限制](#已知限制)
- [许可证](#许可证)

---

## 特性

- **一套代码，三个平台**：Windows / macOS / Linux，CMake 构建。
- **两种传输方式**：串口（STC 经典 ISP 监视器）和 USB HID（出厂 USB ISP 接口）。
- **芯片自动识别**：自带 1300 多条芯片 ID 表，探测到就自动选对应协议。
- **串口波特率校准**：实测芯片 IRC 频率后现场算 trim，不是死查表。
- **选项字节安全回写**：把芯片状态包里读回的 MSR 原样写回，不会因填 `0xFF` 而变砖。
- **Windows 零外部依赖**：HID 直接用系统自带的 `hid.dll` + SetupAPI，不需要 hidapi、不需要 vcpkg。
- **可脚本化**：纯命令行工具，退出码规范（成功 0，失败非 0），方便集成到构建流程。

---

## 支持的芯片

| 系列 | 型号 |
|---|---|
| STC89/90 | STC89C52RC、STC89C516RD、STC90C58RD 等 |
| STC10/11/12 | STC12C5A60S2、STC12LE5A60S2、IAP11F06 等（含 12A / 12B 变体）|
| STC15 | STC15F104E、STC15W408AS、IAP15W4K58S4 等（含 15A 变体）|
| STC8 | STC8F、STC8A、STC8C、GX8S 等 |
| STC8G | STC8G1K08、AI8G1K08、JX8G 等 |
| STC8H | STC8H1K08 ~ STC8H8K64U、AI8H 等 |
| STC16 | STC16、AI16 系列 |
| STC32 | STC32G12K128、STC32F、AI32 等 |
| STC8051U | STC8051U34K64、AI8051U34K64 等 |

> 📌 **8051U 系列串口与 USB HID 都能烧录。**
>
> 官方手册把 8051U 列为「仅 USB HID」，早期版本据此在串口模式下直接拒绝。
> 实测（AI8051U34K64，BSL 7.4U）发现该结论不成立：串口 ISP 监视器会应答唤醒
> 字节，校准握手与完整烧录流程均正常。该限制已移除——**芯片应答即说明可用**，
> 由探测本身决定，而非由手册决定。

> 📌 可选 IRC 频率上限为 **45MHz**（厂商验证范围），**没有超频档**。
> 超出范围的 `-F` 会明确报错。

---

## 两种传输方式

### 串口（UART）

STC 经典 ISP 监视器。芯片可能还在跑用户程序，所以工具会：

1. **拉低 DTR 250ms 做复位脉冲**
2. **反复发送 `0x7F` 唤醒字节**（最多 500 轮，每轮 200 次）
3. 芯片应答后**校准波特率**：发 `0x00 0x08` 粗扫测 IRC 频率，再发 `0x00 0x0C` 细扫，
   最后把算出的 trim 通过 `0x01` 下发并切换波特率

支持 2400 ~ 460800 bps（STC89/12 系列因 8 位 BRT 限制，最高 115200）。

### USB HID

STC 出厂 USB ISP 接口（`VID 0x34BF / PID 0x1001`），STC32、STC8H、STC8G、8051U 等型号支持。

- **不需要唤醒**：用户按住 BOOT 上电时芯片已经在 ISP 监视器里了
- **不需要波特率协商**：USB 链路由主控同步，直接写选项字节里的频率和 trim
- **平台实现不同**：
  - Windows：直接调 `hid.dll` + SetupAPI（系统自带，零依赖）
  - macOS / Linux：用 hidapi（IOKit / hidraw 后端）

---

## 编译

需要 **CMake 3.20+** 和 **C++17 编译器**。

### Windows

用 **Visual Studio 2022 x64 开发者命令提示符**。**不需要任何外部依赖**：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build\Release\newisp.exe`

也可以直接用仓库里的脚本：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\win-build.ps1
powershell -ExecutionPolicy Bypass -File scripts\win-smoke.ps1    # 冒烟测试
```

Windows 版只链接 `setupapi` 和 `hid` 两个系统库，**没有要下载或附带的东西**。

### Linux

```sh
sudo apt install build-essential cmake pkg-config libudev-dev libhidapi-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**权限配置**：

```sh
# 串口：把用户加入 dialout 组（需要重新登录）
sudo usermod -aG dialout $USER

# HID：hidraw 节点默认是 root:root 0600，需要 udev 规则
sudo tee /etc/udev/rules.d/60-newisp.rules <<'EOF'
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="34bf", MODE="0666"
SUBSYSTEM=="tty",    ATTRS{idVendor}=="34bf", MODE="0666"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> 💡 **WSL2 默认看不到 USB 设备**。需要用
> [`usbipd-win`](https://github.com/dorssel/usbipd-win) 把设备转发进去：
> ```powershell
> usbipd list                                    # 找到 BUSID
> usbipd bind --busid <BUSID>                    # 需要管理员权限
> usbipd attach --wsl --busid <BUSID>
> ```
> 仓库里的 `scripts/usbipd-attach.ps1` 封装了这个流程。

### macOS

**Apple Silicon（M 系列）**可以用 Homebrew：

```sh
brew install cmake hidapi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**Intel Mac 注意**：Homebrew 7.0.0 起已把 Intel Mac 降级为 "Tier 3"，
安装脚本会直接拒绝运行在非 Apple Silicon 上。Intel 机器需要手动装依赖：

```sh
# 1. CMake（macOS 自带 python3，用 pip 装）
python3 -m pip install --user cmake
echo 'export PATH="$HOME/Library/Python/3.9/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc

# 2. hidapi（从源码编译，走 IOKit 后端）
cd ~
curl -L -o hidapi.tar.gz \
  https://github.com/libusb/hidapi/archive/refs/tags/hidapi-0.14.0.tar.gz
tar xzf hidapi.tar.gz && cd hidapi-hidapi-0.14.0
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j4
sudo cmake --install build

# 3. newisp
cd ~/newisp-cross
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

> `CMAKE_POLICY_VERSION_MINIMUM=3.5` 是必需的：hidapi 0.14.0 要求的 CMake 版本低于 3.5，
> 而 CMake 4.x 会直接拒绝这类工程。

macOS 的串口设备是 **`/dev/cu.*`**。**优先用 `cu` 而不是 `tty`**：
打开 `tty.*` 会等待 DCD 信号，而自供电的开发板永远不会给，会导致 open 卡住。

更详细的 macOS 步骤（含命令行工具安装）见 [`docs/macos-build.md`](docs/macos-build.md)。

### 不带 HID 支持编译

```sh
cmake -S . -B build -DNEWISP_ENABLE_HID=OFF
```

会生成**纯串口版**，运行 `--hid` 或 `list` 时会提示"本版本未编译 USB HID 支持"。

macOS / Linux 上如果没装 hidapi，也会**自动降级**到这种状态。这是**有意设计**的：
缺一个可选依赖不应该变成链接错误，纯串口本身也是能用的工具。

Windows 上 HID 来自系统库，所以除非显式关掉，否则总是可用。

---

## 编码注意事项

**源码文件是 UTF-8 with BOM，且 MSVC 编译时加了 `/utf-8`。**

两个都需要，原因是日志里有中文：不加 BOM 的话，MSVC 会按系统 ANSI 代码页（中文 Windows 上是
936/GBK）读源文件，把中文字符串当成 GBK 解析，直接报 `常量中有换行符` 编译错误。

**请勿去掉 BOM。** 换行符统一用 **LF**（`.gitattributes` 里已强制）。

---

## 用法

### 命令

```sh
newisp                                  列出设备，然后显示帮助
newisp list                             列出串口和 HID 设备
newisp detect   [传输方式] [设备]        探测并识别芯片
newisp burn -f FILE [参数] [设备]        擦除、写入、写选项字节
```

### 参数

| 参数 | 说明 |
|---|---|
| `-s`, `--serial` | 用串口 ISP（默认） |
| `-H`, `--hid` | 用 USB HID ISP |
| `-d`, `--device NAME` | 设备名：`COM7`、`/dev/ttyUSB0`、`/dev/cu.usbserial-*` 或 HID 设备路径。省略时若只有一个候选设备则自动选，多个则报错并列出 |
| `-f`, `--file FILE` | Intel HEX 固件（`burn` 必需）|
| `-b`, `--baud RATE` | 串口波特率，默认 115200。支持 `115200`、`115.2k`、`1M` 写法 |
| `-F`, `--freq HZ` | 目标 IRC 频率，默认 24MHz。支持 `24000000`、`24M`、`11.0592M`。**上限 45MHz**，超出会报错 |
| `-e`, `--eeprom SIZE` | EEPROM 分割，如 `4K`（`0` = 保留芯片设置）|
| `--clock internal\|external` | 时钟源（仅 STC89/12/15 有）|
| `--family NAME` | 探测失败时的兜底系列：`stc89`、`stc12`、`stc15`、`stc8g`、`stc32`、`stc8` |
| `-q`, `--quiet` | 只显示错误和最终结果 |
| `-v`, `--verbose` | 显示原始报文（排查问题时用）|
| `-h`, `--help` | 帮助 |
| `-V`, `--version` | 版本 |

### 示例

**列出设备：**

```sh
newisp list
newisp list -v          # 详细模式，会列出所有 HID 设备（含鼠标键盘）
```

**检测芯片：**

```sh
newisp detect --hid                              # USB HID
newisp detect --device /dev/ttyUSB0              # Linux 串口
newisp detect --device /dev/cu.usbserial-1420    # macOS 串口
newisp detect -d COM13                           # Windows 串口
```

**烧录固件：**

```sh
# USB HID，目标 45MHz（AI8051U 等）
newisp burn -f firmware.hex --hid -F 45M

# 串口，460800 波特率，目标 24MHz
newisp burn -f firmware.hex -d /dev/ttyUSB0 -b 460800

# 带 EEPROM 分割和详细日志
newisp burn -f firmware.hex -d COM13 -F 35M -e 4K -v

# STC12 系列选择外部晶振
newisp burn -f firmware.hex -d COM13 --clock external
```

---

## 测试

### 单元测试（不需要硬件）

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug -DNEWISP_BUILD_TESTS=ON
cmake --build build-tests -j
./build-tests/tests/newisp_tests
```

覆盖内容：

- **Intel HEX 解析器**：扩展线性/段地址、CRLF 输入、小写十六进制、截断记录、EOF 记录、
  空隙填 `0xFF`
- **芯片表**：已知 ID、未知 ID 占位符、无空条目
- **芯片系列判定与协议选择**：频率上限、legacy 115200 限制、哪些系列需要 `0x05` 准备步骤、
  哪些能走 HID
- **报文帧格式**：精确字节和校验和、所有协议的往返、容忍前导噪声、拒绝错误校验和和丢失结束符、
  处理跨两次读到的分包、拒绝错误系列或不合理时钟的状态包

### 伪终端协议测试（不需要硬件）⭐

用 **PTY 对**模拟真实串口，另一端跑一个脚本化的假 STC 芯片，驱动**真实二进制**跑完整烧录流程：

```sh
python3 tests/pty_test.py ./build/newisp
```

**期望输出**：

```
PASS
exit code      : 0
saw wakeup 0x7F: True
erase seen     : True
blocks written : 4
options written: True
```

这个测试**只能跑在 Linux / macOS**（Windows 没有 PTY）。

### 辅助脚本

| 脚本 | 用途 |
|---|---|
| `scripts/wsl-build.sh` | WSL 里构建 |
| `scripts/wsl-tests.sh` | WSL 里跑单元测试 |
| `scripts/wsl-ptytest.sh` | WSL 里跑 PTY 协议测试 |
| `scripts/wsl-burn.sh` | WSL 里真机烧录 |
| `scripts/wsl-detect.sh` | WSL 里真机检测 |
| `scripts/wsl-udev.sh` | 装 udev 规则并修设备权限（需 sudo）|
| `scripts/usbipd-attach.ps1` | 把 USB 设备转发进 WSL（需管理员）|
| `scripts/win-build.ps1` | Windows 构建 |
| `scripts/win-smoke.ps1` | Windows 冒烟测试 |
| `tests/pty_diag.py` | 查看 tty 实际接受哪些 termios 设置 |
| `tests/pty_opts_dump.py` | 打印工具写入的选项字节（含解码后的频率和 trim）|

---

## 代码结构

```
include/newisp/
  IChannel.h            传输接口（Open/Write/Read/Flush/SetBaudRate/DTR/RTS）
  serial_channel.h      串口后端
  hid_channel.h         USB HID 后端
  stc_protocol.h        协议基类：报文封帧、等待/脉冲辅助函数
  protocol_stc89.h      STC89/90（ProtocolStc89 旧版 1 字节校验和，ProtocolStc89A）
  protocol_stc12.h      STC10/11/12（12A / 12B 变体）
  protocol_stc15.h      STC15（15A 变体）
  protocol_stc8_uart.h  STC32/STC8G/STC8 串口实现
  protocol_stc8_hid.h   STC32/STC8G/STC8 USB HID 实现
  protocol_factory.h    芯片名 → 协议
  chip_table.h          芯片 ID（magic）→ 型号名，1300 多条
  chip_logic.h          系列判定、频率/波特率表
  hex_file.h            Intel HEX 解析
  session.h             检测/烧录流程
  options.h             命令行参数
  text.h                UTF-8 文本工具、十六进制格式化、单调时钟

src/core/               平台无关：芯片表、芯片逻辑、HEX 解析
src/transport/          平台相关：串口（Win32 或 termios）、USB HID
src/cli/                参数解析、会话驱动、main
tests/                  单元测试和 PTY 协议测试
```

协议层只依赖 `stcisp::IChannel` 接口，**从不提及具体传输方式**。

STC32/STC8G/STC8 这三个系列**每种传输方式有独立实现**，因为它们的
状态包帧格式和校准流程有实质性差异。其余系列（STC89/12/15）只有串口实现。

### 传输后端

| 平台 | 串口 | USB HID |
|---|---|---|
| Windows | `CreateFile` + `DCB` + `EscapeCommFunction` | `setupapi.dll` + `hid.dll`（系统 HID 类驱动）|
| macOS | `termios` + `TIOCMGET`/`TIOCMSET` | hidapi（IOKit 后端）|
| Linux | `termios` + `TIOCMGET`/`TIOCMSET` | hidapi（hidraw 后端）|

串口枚举也是各平台一套：Windows 走 SetupAPI 端口类，macOS 走 IOKit 的
`IOSerialBSDClient` 服务，Linux 扫 `/dev` 并用 sysfs 交叉验证。

**两个平台无关的细节值得说明**：

1. **打开串口后会立刻清掉 DTR 和 RTS**。多数平台打开串口时默认拉高 DTR，
   而很多 USB 转串口芯片（CH340、CP2102、FT232）的 DTR 就是接在板子复位脚上的，
   不清掉的话复位脉冲还没发，板子就已经被按住复位了。

2. **校验位是尽力而为**。有些 tty 设备（尤其伪终端，以及部分 USB 桥驱动）会用 `EINVAL`
   拒绝 `PARENB`。这时波特率和数据格式照样生效，端口记录**实际生效的**校验位，
   `SetParity` 也如实返回结果而不是谎报成功。老 STC 系列本来就是 8N1。

---

## 硬件相关说明

### 时钟范围

可选 IRC 频率上限为 **45MHz**，即厂商验证范围。**没有超频档**，因为高出来的那些频率
没有可靠依据：

- 46、47MHz 在 AI8051U34K64 上实测通过
- **48MHz 实测会掉频**（不是升频），说明 IRC 物理上限略低于 48MHz
- 49、50MHz 是拟合曲线外推的，从未验证

芯片跑不到的目标频率比「缺少这个选项」更糟：程序会以未知速度运行，症状只是时序微妙不对。
因此超过 45MHz 的 `-F` 会**明确报错**，而不是静默夹取。

STC8G 和 STC15 系列上限为 35MHz，超出会带提示下调。

### 选项字节

选项字节里存着看门狗、复位行为、时钟配置。**写错可能让芯片再也起不来**，所以工具会：

- **回显即将写入的 MSR 字节**，便于事后排查
- 从状态包读回当前 MSR **原样写回**，而不是盲目填 `0xFF`

### 串口自动复位

如果板子**没有自动复位电路**（DTR 没接到复位脚），需要在握手过程中**手动给板子断电重上电**。

### EEPROM 分割

只有 STC32 系列在选项块里有用户可选的 EEPROM/程序区划分。对不支持的系列用
`-e` 会被忽略。

---

## 验证状态

本仓库实际验证过的内容，避免夸大：

| 平台 | 编译 | 单元测试 | 冒烟测试 | 真机验证 |
|---|---|---|---|---|
| Linux（Ubuntu 22.04 / GCC 11 / CMake 3.22）| ✅ | ✅ 全通过 | ✅ | ✅ PTY 协议测试通过；AI8051U34K64 走 USB HID 烧录 45MHz |
| Windows（MSVC 19.44 / CMake 4.4）| ✅ | — | ✅ 枚举到真实串口和 HID 设备 | ✅ AI8051U34K64 走 USB HID 烧录 24/40/45MHz，**并走串口烧录**；STC8H8K64U 串口 460800 |
| macOS（13.1 / AppleClang 14 / CMake 4.4）| ✅ | 待补 | ✅ | ✅ AI8051U34K64 走 USB HID 烧录 45MHz |

### 真机烧录验证

在真实硬件上验证，每次烧录退出码都是 0，全流程芯片应答正常：

| 步骤 | 结果 |
|---|---|
| 探测 | 识别出 `AI8051U34K64`，magic `0x78B4`，BSL 7.4U |
| `0x01` 波特率设置 | 应答 `01` |
| `0x05` 准备编程 | 应答 `05` |
| `0x03` 擦除 | 应答 `03`，返回真实 UID `78 B4 C9 28 09 FA 88` |
| `0x02`/`0x22` 写块 | 810 字节镜像 13 个块全部成功，每块应答 `02 54` |
| `0x07` 结束 | 应答 `07 54` |
| `0x04` 选项字节 | 应答 `04 54` |

三个目标频率都写入后用**重新探测读回**验证（芯片重新加载选项块，状态包里的频率计数器
会报告新时钟）：

| 目标 | `opts[24..26]` 计数 | 写入的 trim | 读回时钟 |
|---|---|---|---|
| 24 MHz | `0x016E36` | `48 20 01` | 24.000 MHz |
| 40 MHz | `0x02625A` | `73 30 01` | — |
| 45 MHz | `0x02AEA5` | `A6 30 01` | 44.999 MHz |

**44.999MHz 是 IRC 的正常误差**，官方 ISP 工具对同样的选项字节也报同样的值。

串口路径另在 **STC8H8K64U** 上验证：460800 波特率下 265 个块全部写入成功，
校准握手实测 trim `67 / 0x20 / 1`。

三个平台**读出的 UID 完全相同**（`78 B4 C9 28 09 FA 88`），证明协议实现一致。

### 8051U 确实有串口 ISP 监视器

官方手册把 8051U 系列列为「仅 USB HID」，早期版本据此在串口模式下直接拒绝：
探测到 8051U 就提示「请切换到 USB HID」。

这条规则是继承来的、从未实测过，而且**它是错的**。在 AI8051U34K64（BSL 7.4U）上，
串口 ISP 监视器会用 `0x50` 前缀的状态包应答 `0x7F` 唤醒字节，校准握手正常完成，
完整流程跑通：

```
[1/7] 握手成功
[2/7] 已切到 115200，trim_adj=90, trim_range=0x20, trim_freq=24031200 Hz
[3/7] 准备编程成功
[4/7] 擦除成功 → UID 78 B4 C9 28 09 FA 88
[5/7] 13/13 块
[6/7] 选项写入成功
[7/7] 烧录完成！
```

烧录后重新探测可以确认选项字节真的生效：状态包报告 24.031MHz，trim 三元组正是刚写入的值。

该限制已移除。**由探测决定，而非由手册决定**——芯片应答就说明能用；不应答就自然超时，
后续步骤也不会执行。

需要注意：串口和 HID 的状态包**布局并不相同**（串口带 `0x50` 前缀，报告的 MSR 字节也不同），
这也是两种传输方式从一开始就有各自协议实现的原因。

### 一个只有真机能发现的 Bug

移植过程中发现了一个**跨平台 HID 差异**，只有跑真机才能暴露：

| 平台 | `hid_read` 返回的首字节 |
|---|---|
| Windows（`hid.dll`）| **report ID**（HID 栈保留首字节），数据从 index 1 开始 |
| Linux（`hidraw`）| **数据本体**，没有 report ID 字节，数据从 index 0 开始 |

原来无脑跳过首字节，导致 Linux 上**整包偏移 1 字节**，`46 B9` 帧头错位 →
所有协议解析失败 → 报"未检测到芯片"。

**修复方式是不依赖编译期假设，直接看数据**：STC 报文一定以 `46 B9` 开头，
哪个位置有帧头就从哪开始。这个办法对三个平台都稳，也能容忍未来 hidapi 行为变化。

---

## 已知限制

- **PTY 协议测试只验证主机侧**的线路协议：封帧、校验和、命令序列、应答处理。
  它的假芯片只应答 STC32 系列的串口协议，所以老系列靠单元测试覆盖而非端到端测试。
- **PTY 测试只能跑在 Linux / macOS**，因为需要真实的终端设备对。Windows 的串口后端
  是完全不同的 API，没有对应实现。
- **只有 HID 传输在真机上验证过**。串口传输在 Windows（COM13，STC8H8K64U，460800）和
  Linux（WSL + usbipd，PTY 测试）上验证过，但**没有在实体 Linux 主机的串口上烧录过**。
- **macOS 的单元测试还没跑**（编译和真机烧录都已通过）。
- **二进制不能"双击即用"**：这是命令行工具，且没有 Apple 开发者签名。
  发给别人需要去掉隔离属性（`xattr -d com.apple.quarantine newisp`）并确保可执行权限。
  Intel 二进制在 Apple Silicon 上还需要 Rosetta 2。

---

## 许可证

本项目采用 **GNU General Public License v3.0**，详见 [LICENSE](LICENSE)。
