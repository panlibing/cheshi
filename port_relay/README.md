# portrelay —— TCP / UDP 端口无损转发工具（C++17 单文件）

一个把本地某个端口收到的流量**原样、不解析、不修改**地转发到远端
`目标主机:目标端口` 的小工具，TCP 与 UDP 双协议、Windows / Linux / macOS 三平台。

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
build.sh          # 生成 ./portrelay
```

## 用法

```
portrelay <tcp|udp> <listen_port> <target_host> <target_port> [listen_host] [选项]

选项:
  -b KB   内核收发缓冲大小，默认 256（KB）
  -u sec  UDP 空闲会话超时，默认 60 秒
  -m max  UDP 最大并发会话，默认 1024（超限按 LRU 驱逐）
  -v      详细日志（新连接/会话开关）
  -h      帮助
```

### 示例

```bat
:: 把本机所有网卡 9000 端口的 TCP 流量转发到 192.168.1.10:3389（远程桌面）
portrelay tcp 9000 192.168.1.10 3389

:: 仅监听回环地址，缓冲调大到 512KB，开详细日志
portrelay tcp 9000 192.168.1.10 3389 127.0.0.1 -b 512 -v

:: UDP：本地 5353 -> 公共 DNS 8.8.8.8:53
portrelay udp 5353 8.8.8.8 53

:: IPv6 监听 :: ，会话超时 120s，上限 2048
portrelay udp 5353 8.8.8.8 53 :: -u 120 -m 2048
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

仓库自带小回声服务器与 PowerShell 自测脚本（TCP 128KB 随机负载逐字节比对、
UDP 多数据报比对）：

```bat
cd port_relay
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

## 文件

```
portrelay/            （本目录）
├─ portrelay.cpp      主程序（单文件，约 700 行）
├─ build.bat          Windows MSVC 构建脚本
├─ build.sh           Linux/macOS 构建脚本
├─ README.md          本说明
└─ tests/
   ├─ echo.cpp        自测用 TCP/UDP 回声服务器
   └─ run_tests.ps1   端到端自测脚本
```
