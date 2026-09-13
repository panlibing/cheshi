# portrelay —— TCP / UDP 端口无损转发工具（C++17 单文件）

版本 `1.1.0`｜Windows / Linux / macOS｜单文件、无第三方依赖

一个把本地某个端口收到的流量**原样、不解析、不修改**地转发到远端
`目标主机:目标端口` 的小工具，TCP 与 UDP 双协议。

## 特性

| 项目 | 说明 |
|---|---|
| TCP 透传 | 字节流逐字节透传（不解析协议），正确处理 TCP **半关闭**：一端收到 EOF 会向另一端传播 FIN，与直连行为一致，文件/长连接类应用可正常结束 |
| TCP 并发 | 每个客户端独立线程，多客户端同时可用 |
| UDP 无损语义 | 每个数据报**原样转发，保持报文边界**（不粘包/拆包、不改字节）；回包按“源客户端会话”准确路由，多客户端互不串扰 |
| UDP 会话管理 | 空闲会话超时自动回收（`-u`）；会话数上限 + LRU 驱逐（`-m`），防资源耗尽 |
| 内核缓冲可调 | `-b KB` 调大 SO_RCVBUF/SO_SNDBUF，降低突发流量下的用户态丢包 |
| 回包地址透明 | UDP 回包经监听套接字发回，客户端看到的数据来源恒为“监听地址:监听端口” |
| 双栈 | 监听/目标支持 IPv4、IPv6 字面量或域名（自动 getaddrinfo） |
| 优雅退出 | Ctrl+C 在 1 秒内完成清理退出（Windows 控制台处理器 / POSIX 信号） |

> “无损”的含义：转发层不改动任何载荷字节、TCP 保持可靠流语义、UDP 保持报文
> 边界不合并拆分。UDP 本身是尽力而为协议，跨网络丢包由传输层决定；本工具通过
> 放大内核缓冲、非阻塞收发避免在转发层引入额外丢包。

## 构建

### Windows（MSVC，需 VS2022 及以后）

```bat
build.bat
```

或手动（任意 VS 开发者命令行）：

```bat
cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 portrelay.cpp ws2_32.lib /Fe:portrelay.exe
```

### Linux / macOS（g++ / clang++）

```sh
./build.sh          :: 生成 ./portrelay
```

构建产物（`portrelay`、`portrelay.exe`、`*.obj`、`tests/echo`、`tests/probe`
等）已由仓库根目录的 `.gitignore` 忽略，不会入库。

## 用法

四种等价写法可混用，且一次可以启动多条转发规则：

1. **位置写法（兼容旧版）**

   ```
   portrelay <tcp|udp> <listen_port> <target_host> <target_port> [listen_host] [选项]
   ```

2. **命名写法**（顺序任意，也支持 `--key=value` 形式）

   ```
   portrelay --mode tcp --listen-port 9000 --target 192.168.1.10:3389
   ```

3. **规则写法**（`--rule` 可重复出现，一次启动多条规则）

   ```
   portrelay --rule tcp:9000->192.168.1.10:3389 --rule udp:5353->8.8.8.8:53
   ```

4. **配置文件**（`-f`；不带任何参数启动时，自动读取可执行文件同目录的 `portrelay.conf`）

   ```
   portrelay -f portrelay.conf
   ```

### 选项（与 `portrelay -h` 输出保持一致）

| 选项 | 说明 |
|---|---|
| `-M`, `--mode` tcp\|udp | 转发模式 |
| `-l`, `--listen-port` N | 监听端口 |
| `-L`, `--listen` [HOST:]PORT | 监听地址（默认 `0.0.0.0` / `::`，即全部网卡） |
| `-lh`, `--listen-host` HOST | 监听地址（等价于 `--listen` 的 HOST 部分） |
| `-t`, `--target` HOST:PORT | 目标地址 |
| `-th`, `--target-host` HOST | 目标主机 |
| `-tp`, `--target-port` N | 目标端口 |
| `-r`, `--rule` SPEC | 一条完整规则（可重复出现） |
| `-f`, `--config` FILE | 从配置文件读规则（每行一条，可多行） |
| `-b`, `--buf-kb` KB | 内核收发缓冲大小（KB），默认 256 |
| `-u`, `--udp-timeout` sec | UDP 空闲会话超时秒数，默认 60 |
| `-m`, `--udp-max` max | UDP 最大并发会话数，默认 1024（超限按 LRU 驱逐） |
| `-v`, `--verbose` | 详细日志（新连接/会话开关等） |
| `-h`, `--help` | 显示帮助 |
| `-V`, `--version` | 显示版本 |

### 地址写法

- `HOST:PORT` —— 主机 + 端口，如 `192.168.1.10:3389`；IPv6 用 `[::1]:9000`
- `PORT` —— 仅端口（作为监听地址时表示监听全部网卡）
- 规则串 —— `<模式>:<监听地址>-><目标地址>`，如 `tcp:9000->10.0.0.1:80`、
  `udp:127.0.0.1:5353->8.8.8.8:53`；模式省略时默认 `tcp`

### 配置文件格式

每行一条与命令行等价的规则（位置写法最直观），`#` 或 `;` 起始为注释；
也识别 `key=value` 键：`mode` / `listen` / `listen-port` / `listen-host` /
`target` / `target-host` / `target-port` / `buf-kb` / `udp-timeout` /
`udp-max` / `verbose`。

注意：**配置文件按行独立结算**——每读到一行就立即生成一条规则，因此
`mode=tcp`、`listen=9000` 这样把一条规则拆到多行的写法会报
“转发规则不完整”；请把一条规则写在同一行。配置出错时会打印 `文件:行号:`。

### 示例

```bat
:: 把本机所有网卡 9000 端口的 TCP 流量转发到 192.168.1.10:3389(远程桌面)
portrelay tcp 9000 192.168.1.10 3389

:: 同上, 命名写法; 缓冲调大到 512KB 并开详细日志
portrelay --mode tcp --listen-port 9000 --target 192.168.1.10:3389 --buf-kb 512 --verbose

:: 仅监听回环地址
portrelay -M tcp -l 9000 -t 192.168.1.10:3389 -lh 127.0.0.1

:: UDP: 本地 5353 -> 公共 DNS 8.8.8.8:53, 会话超时 120 秒
portrelay --mode udp --listen 0.0.0.0:5353 --target 8.8.8.8:53 --udp-timeout 120

:: 一次启动多条规则
portrelay --rule tcp:9000->192.168.1.10:3389 --rule tcp:9001->192.168.1.11:3389

:: 从配置文件启动
portrelay -f portrelay.conf
```

## 设计说明（UDP 为何“会话级”转发）

UDP 无连接、多客户端可共用同一监听端口。若简单地把收到的所有报文都从同一个
套接字发给目标，目标回包到达后**无法分辨该回给哪个客户端**。
因此本工具采用内核 NAT 式设计：

1. 监听套接字收到客户端 A 的第一个数据报时，为该客户端创建一条独立“上游会话”：
   新建一个 UDP 套接字 `connect` 到目标；
2. 客户端 → 目标：经该会话套接字直发（数据报原样）；
3. 目标 → 客户端：回包落在会话套接字上，读回后**经监听套接字** `sendto` 给
   客户端 A —— 所以客户端看到的对端永远是“监听地址:监听端口”，透明无感；
4. 会话按源地址+源端口标识，空闲 `-u` 秒自动回收。

已知限制：会话套接字使用随机源端口，因此在目标服务器上看到的来源端口是随机的
（而不是监听端口）。对绝大多数场景（转发到目标的服务端口，服务端不关心客户端源
端口）无影响；如需“源端口也保持不变”的完全透明代理，需系统级 IP 层方案
（如 Windows WFP / Linux TPROXY），不在本工具范围。

## 自测

### Linux / macOS / CI（bash，推荐）

脚本自带客户端探针（`tests/probe.cpp`，纯 C++17，不依赖 python/nc），会自行
编译 `portrelay`、`tests/echo`、`tests/probe`：

```sh
bash port_relay/tests/run_tests.sh              # 退出码 0 = 全过, 1 = 有失败
FORCE_BUILD=1 bash port_relay/tests/run_tests.sh   # 强制重新编译
CXX=clang++ bash port_relay/tests/run_tests.sh     # 指定编译器
```

覆盖范围：

- **A) 数据面** —— TCP 128KB 伪随机负载逐字节比对；UDP 多个数据报逐包比对（长度 + 内容）
- **B) 启动写法** —— 位置写法 / 命名参数（含 `--key=value`）/ `--rule`（可重复）/ `-f` 配置文件
- **C) 默认配置** —— 不带参数启动时自动加载可执行文件同目录的 `portrelay.conf`
- **D) 帮助与错误分支** —— `-h` / `-V` 输出；非法 mode、端口越界、坏规则串、配置文件不存在等必须被拒绝并给出可读错误
- **E) 文档同步** —— README 与 `-h` 的长选项集合双向比对

只做文档同步检查（不需要编译器，直接用已构建的二进制）：

```sh
bash port_relay/tests/check_docs_sync.sh                        # 默认 ../portrelay
bash port_relay/tests/check_docs_sync.sh port_relay/portrelay.exe
```

### Windows（PowerShell）

```bat
cd port_relay
powershell -NoProfile -ExecutionPolicy Bypass -File tests\build.bat
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_tests.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_args_tests.ps1
```

### 持续集成

`.github/workflows/port_relay-ci.yml` 在 `ubuntu-latest` / `windows-latest` /
`macos-latest` 上编译并执行自测（Linux/macOS 跑 `tests/run_tests.sh`，
Windows 跑 PowerShell 自测并附加 MSVC 构建检查）。

## 文件

```
port_relay/
├─ portrelay.cpp            主程序(单文件, 1166 行)
├─ build.bat                Windows MSVC 构建脚本
├─ build.sh                 Linux/macOS 构建脚本
├─ README.md                本说明
└─ tests/
   ├─ echo.cpp              自测用 TCP/UDP 回声服务器(88 行)
   ├─ probe.cpp             跨平台自测客户端探针(231 行)
   ├─ build.bat             tests 下辅助程序的构建脚本
   ├─ run_tests.sh          端到端自测: Linux/macOS/CI(231 行)
   ├─ check_docs_sync.sh    README 与 -h 长选项双向一致性检查(59 行)
   ├─ run_tests.ps1         Windows 端到端自测(PowerShell)
   ├─ run_args_tests.ps1    Windows 参数/错误分支自测(PowerShell)
   ├─ diag.ps1              Windows 快速诊断(PowerShell)
   └─ multi_rule_demo.ps1   多规则演示(PowerShell)
```

仓库根目录另有 `.gitignore`（忽略构建产物与本机 `portrelay.conf`）与
`.github/workflows/port_relay-ci.yml`（CI 配置）。
