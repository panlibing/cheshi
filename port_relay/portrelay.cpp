// ============================================================================
// portrelay.cpp -  TCP/UDP 端口无损转发工具  (C++17, 单文件)
//
//  特性:
//   * TCP  : 透传字节流, 不解析/不修改任何数据; 正确处理半关闭(半开连接),
//            一端 EOF 会向另一端传播 FIN, 保持与直连一致的语义;
//            每连接独立线程, 支持多并发客户端。
//   * UDP  : 按"客户端端点"建立独立上游会话(内核 NAT 式), 保持数据报边界,
//            每个数据报原样转发(不改字节、不粘包/拆包);
//            多客户端各自独立会话, 回包准确路由回对应客户端;
//            空闲会话超时自动回收, 会话数可限流(超限按 LRU 驱逐)。
//   * 双平台: Windows(Winsock2) / Linux & macOS(POSIX) 自动适配;
//   * 可选增大收发内核缓冲区(-b), 减少突发流量下的丢包;
//   * 回包经监听套接字发回, 客户端看到的对端地址始终是"监听地址:端口"。
//
//  用法(三种写法等价, 可混用; 支持一次启动多条规则):
//     1) 位置写法(兼容旧版):
//          portrelay <tcp|udp> <listen_port> <target_host> <target_port>
//                    [listen_host] [-b KB] [-u sec] [-m max] [-v] [-h]
//     2) 命名写法(顺序任意):
//          portrelay --mode tcp --listen-port 9000 --target 192.168.1.10:3389
//     3) 规则写法(可重复):
//          portrelay --rule tcp:9000->192.168.1.10:3389 --rule udp:5353->8.8.8.8:53
//     4) 配置文件:
//          portrelay -f portrelay.conf
//          (不带任何参数启动时, 自动读取 exe 同目录的 portrelay.conf)
//   例:
//     portrelay tcp 9000 192.168.1.10 3389          # 本机所有网卡:9000 -> 远程:3389
//     portrelay tcp 9000 192.168.1.10 3389 127.0.0.1  # 仅回环
//     portrelay udp 5353 8.8.8.8 53
//     portrelay -M udp -l 5353 -t 8.8.8.8:53 -u 120 -b 512 -v
//
//  编译(Windows MSVC):
//     cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 portrelay.cpp ws2_32.lib
//  编译(Linux/macOS):
//     g++ -std=c++17 -O2 -Wall -Wextra -pthread -o portrelay portrelay.cpp
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <climits>
#include <fstream>
#include <cctype>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  const sock_t kInvalid = INVALID_SOCKET;
  #define SOCK_ERRNO()  WSAGetLastError()
  #define E_WOULDBLOCK  WSAEWOULDBLOCK
  #define E_CONNRESET   WSAECONNRESET
  #define E_INTR        WSAEINTR
  #define SHUT_SEND     SD_SEND
  inline int closesock(sock_t s) { return closesocket(s); }
  inline bool set_nonblock(sock_t s) { u_long m = 1; return ioctlsocket(s, FIONBIO, &m) == 0; }
  inline int xpoll(pollfd* fds, unsigned long n, int t) { return WSAPoll(fds, n, (INT)t); }
  #define SELECT_NFDS(a,b)  0
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <csignal>
  using sock_t = int;
  const sock_t kInvalid = -1;
  #define SOCK_ERRNO()  errno
  #define E_WOULDBLOCK  EWOULDBLOCK
  #define E_CONNRESET   ECONNRESET
  #define E_INTR        EINTR
  #define SHUT_SEND     SHUT_WR
  inline int closesock(sock_t s) { return ::close(s); }
  inline bool set_nonblock(sock_t s) {
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
  }
  inline int xpoll(pollfd* fds, unsigned long n, int t) { return ::poll(fds, (nfds_t)n, t); }
  #define SELECT_NFDS(a,b)  ((a)>(b)?(a):(b)) + 1
#endif

// ----------------------------------------------------------------------------
// 全局配置 / 工具函数
// ----------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_active_tcp{0};

struct Options {
  std::string mode;        // tcp | udp
  int  listen_port  = 0;
  std::string target_host;
  int  target_port  = 0;
  std::string listen_host; // 空 = 0.0.0.0
  int  sockbuf_kb   = 256; // 内核收发缓冲 (KB)
  int  udp_timeout  = 60;  // UDP 空闲会话超时(秒)
  int  udp_max      = 1024;// UDP 最大并发会话
  bool verbose      = false;
};

struct AddrInfo {
  sockaddr_storage sa{};
  socklen_t        len = 0;
  int              family = AF_UNSPEC;
};

static std::mutex g_log_mu;
static void logline(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  va_list ap; va_start(ap, fmt);
  std::fputs("[relay] ", stdout);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}
static void logv(const Options& o, const char* fmt, ...) {
  if (!o.verbose) return;
  std::lock_guard<std::mutex> lk(g_log_mu);
  std::fputs("[relay] ", stdout);
  va_list ap; va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

static bool sockerr_is(int e, int kind) { return e == kind; }

static std::string sockerr_str() {
#ifdef _WIN32
  int e = WSAGetLastError();
  char buf[256] = {0};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, (DWORD)e, 0, buf, sizeof(buf), nullptr);
  std::string s(buf);
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
  return "WSAError " + std::to_string(e) + ": " + s;
#else
  return std::string(strerror(errno));
#endif
}

// ----------------------------------------------------------------------------
// 地址解析 / 套接字创建
// ----------------------------------------------------------------------------
static bool resolve(const std::string& host, int port, int socktype,
                    bool passive, std::vector<AddrInfo>& out, std::string& err) {
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_protocol = (socktype == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP;
  hints.ai_flags    = passive ? AI_PASSIVE : 0;

  const char* node = nullptr;
  std::string hostcopy;
  if (host.empty()) {
    if (!passive) { err = "empty host"; return false; }
  } else if (host == "*") {
    if (!passive) { err = "invalid host *"; return false; }
  } else {
    hostcopy = host;
    node = hostcopy.c_str();
  }
  std::string svc = std::to_string(port);
  addrinfo* res = nullptr;
  int rc = getaddrinfo(node, svc.c_str(), &hints, &res);
  if (rc != 0) {
#ifdef _WIN32
    err = "getaddrinfo: " + std::string(gai_strerrorA(rc));
#else
    err = "getaddrinfo: " + std::string(gai_strerror(rc));
#endif
    return false;
  }
  for (addrinfo* p = res; p; p = p->ai_next) {
    AddrInfo ai;
    ai.family = p->ai_family;
    ai.len    = (socklen_t)p->ai_addrlen;
    std::memcpy(&ai.sa, p->ai_addr, p->ai_addrlen);
    out.push_back(ai);
  }
  freeaddrinfo(res);
  return !out.empty();
}

static sock_t make_socket(int family, int socktype) {
  return socket(family, socktype, (socktype == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP);
}

static void set_sockbufs(sock_t s, int bytes) {
  if (bytes > 0) {
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bytes, sizeof(bytes));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bytes, sizeof(bytes));
  }
}

static void set_nodelay(sock_t s) {
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

// 创建并绑定监听套接字(TCP/UDP 通用)
static sock_t create_listener(const Options& o, int socktype, std::string& err) {
  std::vector<AddrInfo> adds;
  bool wildcard = o.listen_host.empty() || o.listen_host == "*";
  std::string host = wildcard ? std::string("*") : o.listen_host;
  if (!resolve(host, o.listen_port, socktype, true, adds, err)) return kInvalid;
  if (wildcard) {
    // 通配绑定时优先尝试 IPv6(::)——配合下面的 IPV6_V6ONLY=0 可同时覆盖
    // IPv4 与 IPv6 客户端; 若该系统不支持则自然回退到 IPv4(0.0.0.0)。
    std::stable_sort(adds.begin(), adds.end(),
                     [](const AddrInfo& a, const AddrInfo& b) {
                       return a.family == AF_INET6 && b.family != AF_INET6;
                     });
  }

  for (const AddrInfo& ai : adds) {
    sock_t s = make_socket(ai.family, socktype);
    if (s == kInvalid) continue;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    if (ai.family == AF_INET6) {
      // 关键修复: Windows 默认 IPV6_V6ONLY=1, 只绑 :: 会拒绝 IPv4 连接
      // (127.0.0.1 等)。置 0 后该套接字为 dual-stack, v4/v6 通吃。
      int v6only = 0;
      setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
    }
    set_sockbufs(s, o.sockbuf_kb * 1024);
    if (bind(s, (const sockaddr*)&ai.sa, ai.len) == 0) {
      if (socktype == SOCK_STREAM) {
        if (listen(s, SOMAXCONN) != 0) { closesock(s); continue; }
      }
      set_nonblock(s);
      return s;
    }
    closesock(s);
  }
  err = "bind/listen failed: " + sockerr_str();
  return kInvalid;
}

// ----------------------------------------------------------------------------
// TCP: 每连接透传(阻塞 select + 半关闭传播)
// ----------------------------------------------------------------------------
static bool send_all(sock_t s, const char* data, size_t n) {
  while (n > 0) {
    int chunk = (int)std::min<size_t>(n, (size_t)INT_MAX);
    int w = (int)send(s, data, chunk, 0);
    if (w > 0) { data += w; n -= (size_t)w; continue; }
    if (w < 0) {
      int e = SOCK_ERRNO();
      if (e == E_INTR) continue;
      return false;   // 连接被重置等
    }
    return false;
  }
  return true;
}

static void tcp_worker(sock_t c, const Options& o, const std::vector<AddrInfo>& targets) {
  g_active_tcp.fetch_add(1);

  // 连接目标(依次尝试解析出的各地址)
  sock_t u = kInvalid;
  for (const AddrInfo& ai : targets) {
    u = make_socket(ai.family, SOCK_STREAM);
    if (u == kInvalid) continue;
    if (connect(u, (const sockaddr*)&ai.sa, ai.len) == 0) break;
    closesock(u);
    u = kInvalid;
  }
  if (u == kInvalid) {
    logv(o, "tcp: upstream connect to %s:%d failed (%s)",
         o.target_host.c_str(), o.target_port, sockerr_str().c_str());
    closesock(c);
    g_active_tcp.fetch_sub(1);
    return;
  }

  set_sockbufs(c, o.sockbuf_kb * 1024);
  set_sockbufs(u, o.sockbuf_kb * 1024);
  set_nodelay(c);
  set_nodelay(u);

  logv(o, "tcp: new connection relay started");
  const size_t chunk = std::min<size_t>((size_t)o.sockbuf_kb * 1024, 65536);
  std::vector<char> bA(chunk), bB(chunk);
  bool eofC = false, eofU = false;   // C=客户端 EOF, U=上游 EOF

  while (!(eofC && eofU)) {
    fd_set rf; FD_ZERO(&rf);
    if (!eofC) FD_SET(c, &rf);
    if (!eofU) FD_SET(u, &rf);
    int nfds = SELECT_NFDS(c, u);
    int sel = select(nfds, &rf, nullptr, nullptr, nullptr);
    if (sel < 0) {
      if (SOCK_ERRNO() == E_INTR) continue;
      break;
    }
    if (sel == 0) continue;

    bool rc = (!eofC) && FD_ISSET(c, &rf);
    bool ru = (!eofU) && FD_ISSET(u, &rf);

    if (rc) {
      int n = (int)recv(c, bA.data(), (int)bA.size(), 0);
      if (n > 0) {
        if (!send_all(u, bA.data(), (size_t)n)) break;   // 上游已断
      } else if (n == 0) {
        eofC = true;
        shutdown(u, SHUT_SEND);          // 向目标传播 EOF(半关闭)
      } else {
        int e = SOCK_ERRNO();
        if (e == E_CONNRESET) { eofC = true; shutdown(u, SHUT_SEND); }
        else break;
      }
    }
    if (ru) {
      int n = (int)recv(u, bB.data(), (int)bB.size(), 0);
      if (n > 0) {
        if (!send_all(c, bB.data(), (size_t)n)) break;   // 客户端已断
      } else if (n == 0) {
        eofU = true;
        shutdown(c, SHUT_SEND);          // 向客户端传播 EOF
      } else {
        int e = SOCK_ERRNO();
        if (e == E_CONNRESET) { eofU = true; shutdown(c, SHUT_SEND); }
        else break;
      }
    }
  }
  closesock(u);
  closesock(c);
  g_active_tcp.fetch_sub(1);
  logv(o, "tcp: connection closed");
}

static int run_tcp(const Options& o) {
  std::string err;
  std::vector<AddrInfo> targets;
  if (!resolve(o.target_host, o.target_port, SOCK_STREAM, false, targets, err)) {
    logline("error: cannot resolve target %s:%d : %s",
            o.target_host.c_str(), o.target_port, err.c_str());
    return 1;
  }
  sock_t listener = create_listener(o, SOCK_STREAM, err);
  if (listener == kInvalid) { logline("error: %s", err.c_str()); return 1; }
  logline("TCP relay  %s:%d  ->  %s:%d   (Ctrl+C 停止)",
          o.listen_host.empty() ? "0.0.0.0" : o.listen_host.c_str(), o.listen_port,
          o.target_host.c_str(), o.target_port);

  while (!g_stop.load()) {
    pollfd p{ listener, POLLIN, 0 };
    if (xpoll(&p, 1, 1000) > 0 && (p.revents & (POLLIN | POLLERR | POLLHUP))) {
      for (;;) {
        sock_t c = accept(listener, nullptr, nullptr);
        if (c == kInvalid) break;
        std::thread(tcp_worker, c, o, targets).detach();
      }
    }
  }
  closesock(listener);
  logline("TCP relay stopped (active connections: %d)", g_active_tcp.load());
  return 0;
}

// ----------------------------------------------------------------------------
// UDP: 会话级透传(poll 多路复用, 保数据报边界)
// ----------------------------------------------------------------------------
struct UdpSession {
  sock_t    s = kInvalid;
  sockaddr_storage client{};
  socklen_t clen = 0;
  std::chrono::steady_clock::time_point last;
};

static std::string client_key(const sockaddr_storage& sa, socklen_t len) {
  char host[NI_MAXHOST] = {0}, svc[NI_MAXSERV] = {0};
  if (getnameinfo((const sockaddr*)&sa, len, host, sizeof(host), svc, sizeof(svc),
                  NI_NUMERICHOST | NI_NUMERICSERV) == 0)
    return std::string(host) + "|" + svc;
  return "?";
}

static int run_udp(const Options& o) {
  std::string err;
  std::vector<AddrInfo> targets;
  if (!resolve(o.target_host, o.target_port, SOCK_DGRAM, false, targets, err)) {
    logline("error: cannot resolve target %s:%d : %s",
            o.target_host.c_str(), o.target_port, err.c_str());
    return 1;
  }
  sock_t listener = create_listener(o, SOCK_DGRAM, err);
  if (listener == kInvalid) { logline("error: %s", err.c_str()); return 1; }
  logline("UDP relay  %s:%d  ->  %s:%d   (会话超时 %ds, 上限 %d, Ctrl+C 停止)",
          o.listen_host.empty() ? "0.0.0.0" : o.listen_host.c_str(), o.listen_port,
          o.target_host.c_str(), o.target_port, o.udp_timeout, o.udp_max);

  const AddrInfo& tgt = targets[0];
  std::unordered_map<std::string, UdpSession> byKey;   // client key -> session
  std::unordered_map<sock_t, std::string>     bySock;  // session socket -> key
  std::vector<char> buf(65535);

  const auto erase_session = [&](const std::string& key, const char* why) {
    auto it = byKey.find(key);
    if (it == byKey.end()) return;
    closesock(it->second.s);
    bySock.erase(it->second.s);
    byKey.erase(it);
    logv(o, "udp: session %s closed (%s)", key.c_str(), why);
  };

  // 新会话: 创建连接目标的独立 UDP 套接字(源端口随机, 内核过滤回包来源)
  const auto make_session = [&](const sockaddr_storage& cli, socklen_t clen,
                                const std::string& key) -> bool {
    if (byKey.size() >= (size_t)o.udp_max) {
      // LRU 驱逐最久未活动的会话
      std::string victim;
      auto oldest = std::chrono::steady_clock::now();
      for (auto& kv : byKey)
        if (kv.second.last < oldest) { oldest = kv.second.last; victim = kv.first; }
      if (!victim.empty()) erase_session(victim, "LRU evict");
      if (byKey.size() >= (size_t)o.udp_max) return false;
    }
    sock_t s = make_socket(tgt.family, SOCK_DGRAM);
    if (s == kInvalid) return false;
    set_nonblock(s);
    set_sockbufs(s, o.sockbuf_kb * 1024);
    if (connect(s, (const sockaddr*)&tgt.sa, tgt.len) != 0) {
      closesock(s);
      return false;
    }
    UdpSession ses;
    ses.s = s; ses.client = cli; ses.clen = clen;
    ses.last = std::chrono::steady_clock::now();
    byKey.emplace(key, std::move(ses));
    bySock.emplace(s, key);
    logv(o, "udp: new session %s -> %s:%d", key.c_str(),
         o.target_host.c_str(), o.target_port);
    return true;
  };

  while (!g_stop.load()) {
    // 空闲超时清扫
    auto now = std::chrono::steady_clock::now();
    for (auto it = byKey.begin(); it != byKey.end();) {
      auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last).count();
      if (age > o.udp_timeout) {
        logv(o, "udp: session %s idle timeout", it->first.c_str());
        closesock(it->second.s);
        bySock.erase(it->second.s);
        it = byKey.erase(it);
      } else ++it;
    }

    std::vector<pollfd> pfds;
    pfds.reserve(1 + bySock.size());
    pfds.push_back(pollfd{ listener, POLLIN, 0 });
    for (auto& kv : bySock) pfds.push_back(pollfd{ kv.first, POLLIN, 0 });

    int r = xpoll(pfds.data(), (unsigned long)pfds.size(), 1000);
    if (g_stop.load()) break;
    if (r < 0) {
      if (SOCK_ERRNO() == E_INTR) continue;
      logline("udp: poll error: %s", sockerr_str().c_str());
      break;
    }
    if (r == 0) continue;

    // ---- 监听口有数据: 客户端 -> 目标 ----
    if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
      for (;;) {
        sockaddr_storage src{};
        socklen_t sl = sizeof(src);
        int n = (int)recvfrom(listener, buf.data(), (int)buf.size(), 0,
                              (sockaddr*)&src, &sl);
        if (n < 0) {
          if (SOCK_ERRNO() == E_WOULDBLOCK) break;
          if (SOCK_ERRNO() == E_INTR) continue;
          break;
        }
        std::string key = client_key(src, sl);
        auto it = byKey.find(key);
        if (it == byKey.end()) {
          if (!make_session(src, sl, key)) {
            logv(o, "udp: drop datagram from %s (session create failed/full)", key.c_str());
            continue;
          }
          it = byKey.find(key);
          if (it == byKey.end()) continue;
        }
        int w = (int)send(it->second.s, buf.data(), n, 0);   // 已 connect, 直发目标
        if (w < 0) {
          int e = SOCK_ERRNO();
          if (e == E_WOULDBLOCK) { /* 内核缓冲满, 丢包(无损下界由 -b 缓冲兜底) */ }
          else { erase_session(key, "send error"); }
          continue;
        }
        it->second.last = std::chrono::steady_clock::now();
      }
    }

    // ---- 各会话口有数据: 目标 -> 客户端 ----
    for (size_t i = 1; i < pfds.size(); ++i) {
      if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
      sock_t ss = pfds[i].fd;
      auto sk = bySock.find(ss);
      if (sk == bySock.end()) continue;          // 本迭代内已回收
      const std::string& key = sk->second;
      auto it = byKey.find(key);
      if (it == byKey.end()) continue;
      for (;;) {
        int n = (int)recv(ss, buf.data(), (int)buf.size(), 0);
        if (n < 0) {
          if (SOCK_ERRNO() == E_WOULDBLOCK) break;
          erase_session(key, "recv error");       // 含对端 ICMP 端口不可达等
          break;
        }
        // 经监听套接字发回客户端: 客户端看到源地址恒为 监听地址:监听端口
        int w = (int)sendto(listener, buf.data(), n, 0,
                            (const sockaddr*)&it->second.client, it->second.clen);
        if (w < 0) {
          if (SOCK_ERRNO() != E_WOULDBLOCK) break;
          /* 缓冲满丢弃该回包 */
        } else {
          it->second.last = std::chrono::steady_clock::now();
        }
      }
    }
  }

  for (auto& kv : byKey) closesock(kv.second.s);
  byKey.clear(); bySock.clear();
  closesock(listener);
  logline("UDP relay stopped");
  return 0;
}

// ----------------------------------------------------------------------------
// 启动参数解析: 位置写法 / 命名写法 / 规则写法 / 配置文件, 可混用且支持多规则
// ----------------------------------------------------------------------------
static const char* kVersion = "1.1.0";

struct ParseState {
  std::vector<Options>     rules;    // 已解析完成的转发规则(可多条)
  std::vector<std::string> configs;  // 待加载的配置文件(按出现顺序)
  bool help = false, version = false;

  // 选项默认值: 写在规则前的选项也作为其后规则的默认值
  int  sockbuf_kb = 256;
  int  udp_timeout = 60;
  int  udp_max = 1024;
  bool verbose = false;

  // 当前正在累积的规则
  bool has_mode = false, has_lport = false, has_thost = false, has_tport = false, has_lhost = false;
  std::string mode, thost, lhost;
  int  lport = 0, tport = 0;
  int  pos_slot = 0;   // 位置参数填充到第几槽(0:模式 1:监听端口 2:目标主机 3:目标端口 4:监听地址)
};

static std::string trim_str(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (unsigned char)s[b] <= ' ') ++b;
  while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
  return s.substr(b, e - b);
}

static std::string lower_str(const std::string& s) {
  std::string r = s;
  for (char& c : r) c = (char)std::tolower((unsigned char)c);
  return r;
}

static std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static bool file_exists_(const std::string& p) {
  std::ifstream f(p.c_str(), std::ios::binary);
  return f.good();
}

// 取可执行文件所在目录(用于自动加载同目录 portrelay.conf)
static std::string exe_dir(const char* argv0) {
  std::string p;
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) p.assign(buf, n);
#else
  char buf[4096] = {0};
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) p.assign(buf, (size_t)n);
#endif
  if (p.empty() && argv0) p = argv0;
  size_t pos = p.find_last_of("/\\");
  if (pos == std::string::npos) return std::string(".");
  std::string dir = p.substr(0, pos);
  return dir.empty() ? std::string("/") : dir;
}

static bool parse_int_val(const std::string& s, int& out) {
  std::string t = trim_str(s);
  if (t.empty()) return false;
  char* end = nullptr;
  long v = std::strtol(t.c_str(), &end, 10);
  if (!end || *end != '\0') return false;
  out = (int)v;
  return true;
}

// 解析 "host:port" / "port" / "[IPv6]:port"
static bool parse_hostport(const std::string& in, std::string& host, int& port, std::string& err) {
  std::string s = trim_str(in);
  if (s.empty()) { err = "地址为空"; return false; }
  if (s[0] == '[') {
    size_t rb = s.find(']');
    if (rb == std::string::npos) { err = "IPv6 地址缺少 ']': " + s; return false; }
    host = s.substr(1, rb - 1);
    std::string rest = s.substr(rb + 1);
    if (rest.empty() || rest[0] != ':') { err = "缺少端口: " + s; return false; }
    if (!parse_int_val(rest.substr(1), port)) { err = "端口非法: " + s; return false; }
    return true;
  }
  size_t c1 = s.find(':');
  if (c1 == std::string::npos) {                 // 只写了端口
    if (!parse_int_val(s, port)) { err = "既不是端口也不是 host:port: " + s; return false; }
    host.clear();
    return true;
  }
  if (s.find(':', c1 + 1) != std::string::npos) {
    err = "IPv6 地址请写成 [地址]:端口 : " + s;
    return false;
  }
  host = trim_str(s.substr(0, c1));
  if (!parse_int_val(s.substr(c1 + 1), port)) { err = "端口非法: " + s; return false; }
  return true;
}

// "tcp:9000->192.168.1.10:3389"  =>  {tcp,9000,192.168.1.10,3389}
static bool rule_spec_to_tokens(const std::string& spec, std::vector<std::string>& out, std::string& err) {
  std::string s = trim_str(spec);
  size_t arrow = s.find("->");
  if (arrow == std::string::npos) {
    err = "规则格式应为 <模式>:<监听地址>-><目标主机>:<目标端口>, 例如 tcp:9000->10.0.0.1:80 : " + spec;
    return false;
  }
  std::string lhs = trim_str(s.substr(0, arrow));
  std::string rhs = trim_str(s.substr(arrow + 2));
  std::string mode = "tcp";
  if (!lhs.empty() && lhs[0] != '[') {
    size_t colon = lhs.find(':');
    if (colon != std::string::npos) {
      std::string head = lower_str(lhs.substr(0, colon));
      if (head == "tcp" || head == "udp") { mode = head; lhs = trim_str(lhs.substr(colon + 1)); }
    }
  }
  std::string lhost, thost, e;
  int lport = 0, tport = 0;
  if (!parse_hostport(lhs, lhost, lport, e)) { err = "监听地址非法(" + e + "): " + spec; return false; }
  if (!parse_hostport(rhs, thost, tport, e)) { err = "目标地址非法(" + e + "): " + spec; return false; }
  if (thost.empty()) { err = "目标必须包含主机: " + spec; return false; }
  out.clear();
  out.push_back(mode);
  out.push_back(std::to_string(lport));
  out.push_back(thost);
  out.push_back(std::to_string(tport));
  if (!lhost.empty()) out.push_back(lhost);       // 第 5 个位置参数 = 监听地址
  return true;
}

static bool feed_tokens(const std::vector<std::string>& in, ParseState& st, std::string& err);

// 把当前累积的规则字段落地为一条规则(字段不全则报错)
static bool flush_rule(ParseState& st, std::string& err) {
  if (!(st.has_mode || st.has_lport || st.has_thost || st.has_tport || st.has_lhost)) return true;
  std::string missing;
  if (!st.has_mode)  missing += " --mode (tcp|udp)";
  if (!st.has_lport) missing += " --listen-port";
  if (!st.has_thost) missing += " --target-host";
  if (!st.has_tport) missing += " --target-port";
  if (!missing.empty()) { err = "转发规则不完整, 缺少:" + missing; return false; }
  Options o;
  o.mode        = st.mode;
  o.listen_port = st.lport;
  o.target_host = st.thost;
  o.target_port = st.tport;
  o.listen_host = st.lhost;
  o.sockbuf_kb  = st.sockbuf_kb;
  o.udp_timeout = st.udp_timeout;
  o.udp_max     = st.udp_max;
  o.verbose     = st.verbose;
  st.rules.push_back(o);
  st.has_mode = st.has_lport = st.has_thost = st.has_tport = st.has_lhost = false;
  st.mode.clear(); st.thost.clear(); st.lhost.clear();
  st.lport = st.tport = 0;
  st.pos_slot = 0;
  return true;
}

// 按旧版顺序填充位置参数; 返回 false 且 err 为空表示 5 个位置槽已用完
static bool set_positional(ParseState& st, const std::string& a, std::string& err) {
  for (int slot = st.pos_slot; slot <= 4; ++slot) {
    bool used = (slot == 0 && st.has_mode) || (slot == 1 && st.has_lport) ||
                (slot == 2 && st.has_thost) || (slot == 3 && st.has_tport) ||
                (slot == 4 && st.has_lhost);
    if (used) { st.pos_slot = slot + 1; continue; }
    switch (slot) {
      case 0: {
        std::string m = lower_str(a);
        if (m != "tcp" && m != "udp") { err = "模式必须为 tcp 或 udp: " + a; return false; }
        st.mode = m; st.has_mode = true; break;
      }
      case 1: {
        int p = 0;
        if (!parse_int_val(a, p)) { err = "监听端口非法: " + a; return false; }
        st.lport = p; st.has_lport = true; break;
      }
      case 2: { st.thost = a; st.has_thost = true; break; }
      case 3: {
        int p = 0;
        if (!parse_int_val(a, p)) { err = "目标端口非法: " + a; return false; }
        st.tport = p; st.has_tport = true; break;
      }
      case 4: { st.lhost = a; st.has_lhost = true; break; }
    }
    st.pos_slot = slot + 1;
    return true;
  }
  return false;
}

static bool feed_tokens(const std::vector<std::string>& in, ParseState& st, std::string& err) {
  // 归一化: --key=value 拆成两个 token
  std::vector<std::string> t;
  t.reserve(in.size() + 4);
  for (const std::string& a : in) {
    if (a.size() > 3 && a[0] == '-' && a[1] == '-') {
      size_t eq = a.find('=');
      if (eq != std::string::npos && eq > 2) {
        t.push_back(a.substr(0, eq));
        t.push_back(a.substr(eq + 1));
        continue;
      }
    }
    t.push_back(a);
  }

  for (size_t i = 0; i < t.size(); ++i) {
    const std::string a = t[i];
    if (a.empty()) continue;

    auto need_val = [&](const char* what) -> bool {
      if (i + 1 >= t.size()) { err = std::string("选项 ") + what + " 缺少取值"; return false; }
      ++i;
      return true;
    };

    // ---- 位置参数 ----
    if (a[0] != '-') {
      if (!set_positional(st, a, err)) {
        if (!err.empty()) return false;
        if (!flush_rule(st, err)) return false;      // 上一条已完整 -> 位置槽重置, 起新规则
        if (!set_positional(st, a, err)) {
          if (err.empty()) err = "无法解析的参数: " + a;
          return false;
        }
      }
      continue;
    }

    // ---- 选项 ----
    if (a == "--") continue;
    if (a == "-h" || a == "--help" || a == "-?")    { st.help = true; continue; }
    if (a == "-V" || a == "--version")              { st.version = true; continue; }
    if (a == "-v" || a == "--verbose")              { st.verbose = true; continue; }

    if (a == "-b" || a == "--buf-kb" || a == "--buffer" || a == "--buf") {
      if (!need_val("--buf-kb")) return false;
      int v = 0;
      if (!parse_int_val(t[i], v)) { err = "缓冲大小非法: " + t[i]; return false; }
      st.sockbuf_kb = v;
      continue;
    }
    if (a == "-u" || a == "--udp-timeout" || a == "--timeout") {
      if (!need_val("--udp-timeout")) return false;
      int v = 0;
      if (!parse_int_val(t[i], v)) { err = "会话超时非法: " + t[i]; return false; }
      st.udp_timeout = v;
      continue;
    }
    if (a == "-m" || a == "--udp-max" || a == "--max-sessions") {
      if (!need_val("--udp-max")) return false;
      int v = 0;
      if (!parse_int_val(t[i], v)) { err = "会话上限非法: " + t[i]; return false; }
      st.udp_max = v;
      continue;
    }
    if (a == "-f" || a == "--config" || a == "--conf" || a == "--include") {
      if (!need_val("--config")) return false;
      st.configs.push_back(t[i]);
      continue;
    }
    if (a == "-M" || a == "--mode" || a == "--proto" || a == "--protocol") {
      if (!need_val("--mode")) return false;
      std::string m = lower_str(t[i]);
      if (m != "tcp" && m != "udp") { err = "模式必须为 tcp 或 udp: " + t[i]; return false; }
      // 上一条规则已写完 -> 先落地, 本条开始新规则
      if (st.has_mode && (st.has_lport || st.has_thost || st.has_tport || st.has_lhost)) {
        if (!flush_rule(st, err)) return false;
      }
      st.mode = m; st.has_mode = true;
      continue;
    }
    if (a == "-l" || a == "--listen-port" || a == "--lport") {
      if (!need_val("--listen-port")) return false;
      int v = 0;
      if (!parse_int_val(t[i], v)) { err = "监听端口非法: " + t[i]; return false; }
      st.lport = v; st.has_lport = true;
      continue;
    }
    if (a == "-lh" || a == "--listen-host" || a == "--lhost" || a == "--bind") {
      if (!need_val("--listen-host")) return false;
      st.lhost = t[i]; st.has_lhost = true;
      continue;
    }
    if (a == "-L" || a == "--listen" || a == "--local") {
      if (!need_val("--listen")) return false;
      std::string h, e;
      int p = 0;
      if (!parse_hostport(t[i], h, p, e)) { err = "监听地址非法: " + t[i] + " (" + e + ")"; return false; }
      st.lhost = h; st.has_lhost = !h.empty();
      st.lport = p; st.has_lport = true;
      continue;
    }
    if (a == "-th" || a == "--target-host" || a == "--thost") {
      if (!need_val("--target-host")) return false;
      st.thost = t[i]; st.has_thost = true;
      continue;
    }
    if (a == "-tp" || a == "--target-port" || a == "--tport") {
      if (!need_val("--target-port")) return false;
      int v = 0;
      if (!parse_int_val(t[i], v)) { err = "目标端口非法: " + t[i]; return false; }
      st.tport = v; st.has_tport = true;
      continue;
    }
    if (a == "-t" || a == "--target" || a == "--remote") {
      if (!need_val("--target")) return false;
      std::string h, e;
      int p = 0;
      if (t[i].find(':') == std::string::npos) {
        err = "目标需写成 host:port (只给主机用 --target-host, 只给端口用 --target-port): " + t[i];
        return false;
      }
      if (!parse_hostport(t[i], h, p, e)) { err = "目标地址非法: " + t[i] + " (" + e + ")"; return false; }
      if (h.empty()) { err = "目标必须写成 host:port : " + t[i]; return false; }
      st.thost = h; st.has_thost = true;
      st.tport = p; st.has_tport = true;
      continue;
    }
    if (a == "-r" || a == "--rule") {
      if (!need_val("--rule")) return false;
      std::vector<std::string> rt;
      std::string e;
      if (!rule_spec_to_tokens(t[i], rt, e)) { err = e; return false; }
      if (!flush_rule(st, err)) return false;
      if (!feed_tokens(rt, st, err)) return false;
      if (!flush_rule(st, err)) return false;
      continue;
    }

    err = "未知选项: " + a + " (用 -h 查看用法)";
    return false;
  }
  return true;
}

// ----------------------------------------------------------------------------
// 配置文件: 每行一条规则(与命令行等价), 亦支持 key=value
// ----------------------------------------------------------------------------
struct ConfigKey { const char* key; const char* opt; };
static const ConfigKey kConfigKeys[] = {
  {"mode",        "--mode"},        {"proto",       "--mode"},     {"protocol",  "--mode"},
  {"listen",      "--listen"},      {"local",       "--listen"},
  {"listen-port", "--listen-port"}, {"lport",       "--listen-port"}, {"port", "--listen-port"},
  {"listen-host", "--listen-host"}, {"lhost",       "--listen-host"}, {"bind", "--listen-host"},
  {"target",      "--target"},      {"remote",      "--target"},
  {"target-host", "--target-host"}, {"thost",       "--target-host"}, {"host", "--target-host"},
  {"target-port", "--target-port"}, {"tport",       "--target-port"},
  {"buf-kb",      "--buf-kb"},      {"buffer",      "--buf-kb"},    {"buf",  "--buf-kb"},
  {"udp-timeout", "--udp-timeout"}, {"timeout",     "--udp-timeout"},
  {"udp-max",     "--udp-max"},     {"max",         "--udp-max"},   {"max-sessions", "--udp-max"},
  {"verbose",     "--verbose"},
  {"rule",        "--rule"},
  {"config",      "--config"},      {"include",     "--config"},
};

static bool truthy(const std::string& v) {
  std::string s = lower_str(trim_str(v));
  return s.empty() || s == "1" || s == "true" || s == "on" || s == "yes" || s == "y" ||
         s == "是" || s == "开";
}

static bool config_line_to_tokens(const std::string& line, std::vector<std::string>& toks, std::string& err) {
  toks.clear();
  size_t eq = line.find('=');
  if (eq != std::string::npos) {   // rule= 的取值含 "->", 逐键判定见下
    std::string key = lower_str(trim_str(line.substr(0, eq)));
    std::string val = trim_str(line.substr(eq + 1));
    for (char& c : key) if (c == '_') c = '-';
    const char* opt = nullptr;
    for (const ConfigKey& k : kConfigKeys) {
      if (key == k.key) { opt = k.opt; break; }
    }
    // rule = <规则串> 的取值里必然含 "->", 只有该键允许取值带箭头,
    // 其余含 "->" 的行仍按空白切分(位置写法 / 规则行)。
    const bool rule_key = (opt != nullptr && std::strcmp(opt, "--rule") == 0);
    if (line.find("->") == std::string::npos || rule_key) {
      if (opt) {
        if (std::strcmp(opt, "--verbose") == 0) {
          if (truthy(val)) toks.push_back("--verbose");   // verbose=off / =false 直接忽略
          return true;
        }
        if (val.empty()) { err = "配置项 " + key + " 缺少取值"; return false; }
        toks.push_back(opt);
        toks.push_back(val);
        return true;
      }
      err = "未知配置项: " + key;
      return false;
    }
  }
  toks = split_ws(line);
  return true;
}

static bool load_config_file(const std::string& path, ParseState& st, std::string& err) {
  std::ifstream f(path.c_str());
  if (!f) { err = "无法打开配置文件: " + path; return false; }
  logline("reading config file: %s", path.c_str());
  std::string line;
  int lineno = 0;
  while (std::getline(f, line)) {
    ++lineno;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string t = trim_str(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') continue;
    size_t hash = t.find('#');
    if (hash != std::string::npos) t = trim_str(t.substr(0, hash));
    if (t.empty()) continue;

    std::vector<std::string> toks;
    std::string e;
    if (!config_line_to_tokens(t, toks, e)) {
      err = path + ":" + std::to_string(lineno) + ": " + e;
      return false;
    }
    if (!feed_tokens(toks, st, err)) {
      err = path + ":" + std::to_string(lineno) + ": " + err;
      return false;
    }
    if (!flush_rule(st, err)) {
      err = path + ":" + std::to_string(lineno) + ": " + err;
      return false;
    }
    if (st.help || st.version) return true;
  }
  return true;
}

static int run_rule(const Options& o) {
  return (o.mode == "tcp") ? run_tcp(o) : run_udp(o);
}

// ----------------------------------------------------------------------------
// 信号处理 / 主入口
// ----------------------------------------------------------------------------
#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
  (void)type;
  g_stop.store(true);
  return TRUE;   // 阻止默认终止, 由主循环优雅退出
}
#else
static void sig_handler(int) { g_stop.store(true); }
#endif

static void usage(FILE* f) {
  std::fprintf(f,
    "portrelay %s - TCP/UDP 端口无损转发工具 (C++17)\n"
    "\n"
    "用法(以下写法等价, 可混用; 支持一次启动多条转发规则):\n"
    "  1) 位置写法(兼容旧版):\n"
    "       portrelay <tcp|udp> <listen_port> <target_host> <target_port>\n"
    "                 [listen_host] [选项]\n"
    "  2) 命名写法(顺序任意, 也可 --key=value):\n"
    "       portrelay --mode tcp --listen-port 9000 --target 192.168.1.10:3389\n"
    "  3) 规则写法(可重复出现, 一次启动多条):\n"
    "       portrelay --rule tcp:9000->192.168.1.10:3389\n"
    "                 --rule udp:5353->8.8.8.8:53\n"
    "  4) 配置文件:\n"
    "       portrelay -f portrelay.conf\n"
    "       (不带任何参数启动时, 自动读取 exe 同目录的 portrelay.conf)\n"
    "\n"
    "地址写法:\n"
    "  HOST:PORT   主机 + 端口, 如 192.168.1.10:3389 ; IPv6 用 [::1]:9000\n"
    "  PORT        仅端口(监听地址时表示监听全部网卡)\n"
    "  规则串       <模式>:<监听地址>-><目标地址>, 如 tcp:9000->10.0.0.1:80,\n"
    "               udp:127.0.0.1:5353->8.8.8.8:53, tcp:[::1]:9000->[2001:db8::1]:80\n"
    "               (模式省略时默认 tcp)\n"
    "\n"
    "选项:\n"
    "  -M, --mode tcp|udp       转发模式\n"
    "  -l, --listen-port N      监听端口\n"
    "  -L, --listen [HOST:]PORT 监听地址(默认 0.0.0.0 / ::, 即全部网卡)\n"
    "  -lh,--listen-host HOST   监听地址(等价于 --listen 的 HOST 部分)\n"
    "  -t, --target HOST:PORT   目标地址\n"
    "  -th,--target-host HOST   目标主机\n"
    "  -tp,--target-port N      目标端口\n"
    "  -r, --rule SPEC          一条完整规则(可重复, 见上)\n"
    "  -f, --config FILE        从配置文件读规则(每行一条, 可多行)\n"
    "  -b, --buf-kb KB          内核收发缓冲大小(KB), 默认 256\n"
    "  -u, --udp-timeout sec    UDP 空闲会话超时秒数, 默认 60\n"
    "  -m, --udp-max max        UDP 最大并发会话数, 默认 1024(超限按 LRU 驱逐)\n"
    "  -v, --verbose            详细日志(新连接/会话开关等)\n"
    "  -h, --help               显示本帮助\n"
    "  -V, --version            显示版本\n"
    "\n"
    "配置文件格式: 每行一条与命令行等价的规则, '#' 或 ';' 起始为注释;\n"
    "  也支持 key=value: mode / listen / listen-port / listen-host /\n"
    "  target / target-host / target-port / buf-kb / udp-timeout / udp-max / verbose\n"
    "\n"
    "示例:\n"
    "  portrelay tcp 9000 192.168.1.10 3389            # 旧版位置写法\n"
    "  portrelay -M tcp -l 9000 -t 192.168.1.10:3389 -b 512 -v\n"
    "  portrelay --mode udp --listen 0.0.0.0:5353 --target 8.8.8.8:53 -u 120\n"
    "  portrelay --rule tcp:9000->192.168.1.10:3389 --rule tcp:9001->192.168.1.11:3389\n"
    "  portrelay -f portrelay.conf\n",
    kVersion);
}

int main(int argc, char** argv) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    logline("error: WSAStartup failed");
    return 1;
  }
  SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
  std::signal(SIGINT, sig_handler);
  std::signal(SIGTERM, sig_handler);
#endif

  auto bad = [&](const std::string& msg) {
    logline("error: %s", msg.c_str());
    usage(stderr);
#ifdef _WIN32
    WSACleanup();
#endif
    return 1;
  };

  ParseState st;
  std::string err;
  std::vector<std::string> toks;
  for (int i = 1; i < argc; ++i) toks.push_back(argv[i]);

  // 不带任何参数启动时, 尝试读取 exe 同目录的 portrelay.conf (双击启动友好)
  if (toks.empty()) {
    std::string def = exe_dir(argv[0]) + "/portrelay.conf";
    if (file_exists_(def)) {
      logline("no arguments: using default config %s", def.c_str());
      st.configs.push_back(def);
    } else {
      logline("error: 未提供任何参数, 且未找到默认配置文件 %s", def.c_str());
      usage(stderr);
#ifdef _WIN32
      WSACleanup();
#endif
      return 1;
    }
  }

  if (!feed_tokens(toks, st, err)) return bad(err);
  if (st.help) { usage(stdout); return 0; }
  if (st.version) {
    std::printf("portrelay %s\n", kVersion);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
  }
  if (!flush_rule(st, err)) return bad(err);

  // 配置文件队列(按出现顺序加载, 上限 8 个防循环包含)
  for (size_t ci = 0; ci < st.configs.size(); ++ci) {
    if (ci >= 8) { logline("warning: 配置文件层数过多, 已忽略后续文件"); break; }
    if (!load_config_file(st.configs[ci], st, err)) return bad(err);
    if (st.help) { usage(stdout); return 0; }
    if (st.version) {
      std::printf("portrelay %s\n", kVersion);
#ifdef _WIN32
      WSACleanup();
#endif
      return 0;
    }
  }

  if (st.rules.empty()) return bad("未解析到任何转发规则");

  // 规范化 / 校验每条规则
  for (Options& o : st.rules) {
    if (o.mode != "tcp" && o.mode != "udp") return bad("模式必须为 tcp 或 udp: " + o.mode);
    if (o.listen_port <= 0 || o.listen_port > 65535)
      return bad("监听端口非法: " + std::to_string(o.listen_port));
    if (o.target_port <= 0 || o.target_port > 65535)
      return bad("目标端口非法: " + std::to_string(o.target_port));
    if (o.target_host.empty()) return bad("目标主机为空");
    if (o.sockbuf_kb < 8) o.sockbuf_kb = 8;
    if (o.udp_timeout <= 0) o.udp_timeout = 60;
    if (o.udp_max < 1) o.udp_max = 1024;
  }

  int rc = 0;
  if (st.rules.size() == 1) {
    rc = run_rule(st.rules[0]);
  } else {
    logline("启动 %zu 条转发规则 (Ctrl+C 停止):", st.rules.size());
    for (const Options& o : st.rules)
      logline("  %s  %s:%d -> %s:%d", o.mode.c_str(),
              o.listen_host.empty() ? "0.0.0.0" : o.listen_host.c_str(), o.listen_port,
              o.target_host.c_str(), o.target_port);
    std::vector<std::thread> ths;
    ths.reserve(st.rules.size());
    for (const Options& o : st.rules) ths.emplace_back([&o] { run_rule(o); });
    for (std::thread& th : ths) th.join();
  }

#ifdef _WIN32
  WSACleanup();
#endif
  return rc;
}
