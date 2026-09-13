// probe.cpp - portrelay 自测客户端探针（与 echo.cpp 配套，纯 C++17，不依赖解释器）
//
// 用法:
//   probe tcp <port> [bytes] [seed]        默认 131072 字节, seed 42
//   probe udp <port> [count] [first_len]   默认 3 个数据报, 首包 2000B, 每包 +777B
//
// 行为:
//   tcp: 连接 127.0.0.1:<port>, 一边发送确定性的伪随机负载, 一边并发接收回包,
//        收发结束后逐字节比对(并发收发可避免大负载下的双向缓冲死锁)
//   udp: 依次发送 count 个不同长度的数据报, 逐包校验「长度 + 内容」完全一致,
//        从而验证转发层保持报文边界、不粘包/拆包、不改字节
// 退出码: 0 全部一致; 1 比对失败/超时; 2 参数错误
//
// 独立编译(与 run_tests.sh 相同的方式):
//   Linux/macOS: g++ -std=c++17 -O2 -pthread -o probe probe.cpp
//   Windows(MSVC): cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 probe.cpp ws2_32.lib
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #if defined(_MSC_VER)
    #pragma comment(lib, "ws2_32.lib")  // MSVC 下自动链接 Winsock, 免手写 .lib
  #endif
  using sock_t = SOCKET;
  using socklen_type = int;
  const sock_t kInvalid = INVALID_SOCKET;
  inline void closesock(sock_t s) { closesocket(s); }
  inline int sockerr() { return WSAGetLastError(); }
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using sock_t = int;
  using socklen_type = socklen_t;
  const sock_t kInvalid = -1;
  inline void closesock(sock_t s) { ::close(s); }
  inline int sockerr() { return errno; }
#endif

typedef unsigned char u8;

static void set_timeout(sock_t s, int sec) {
#ifdef _WIN32
  DWORD ms = (DWORD)sec * 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, (int)sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, (int)sizeof(ms));
#else
  timeval tv;
  tv.tv_sec = sec;
  tv.tv_usec = 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

// 确定性伪随机负载(xorshift32), 便于失败时复现
static void fill_payload(std::vector<u8>& buf, uint32_t seed) {
  uint32_t x = seed ? seed : 0x1234567u;
  for (size_t i = 0; i < buf.size(); ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    buf[i] = (u8)(x & 0xFFu);
  }
}

static bool make_addr(const char* host, int port, sockaddr_in& a) {
  std::memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return false;
  return true;
}

static bool tcp_test(const char* host, int port, size_t bytes, uint32_t seed) {
  sockaddr_in a;
  if (!make_addr(host, port, a)) { std::printf("[FAIL] 目标地址非法: %s\n", host); return false; }
  sock_t s = socket(AF_INET, SOCK_STREAM, 0);
  if (s == kInvalid) { std::printf("[FAIL] socket err=%d\n", sockerr()); return false; }
  set_timeout(s, 10);
  if (connect(s, (sockaddr*)&a, (socklen_type)sizeof(a)) != 0) {
    std::printf("[FAIL] connect %s:%d err=%d\n", host, port, sockerr());
    closesock(s);
    return false;
  }

  std::vector<u8> payload(bytes);
  fill_payload(payload, seed);

  size_t got = 0;
  long long first_bad = -1;
  int recv_err = 0;
  std::thread rx([&]() {
    std::vector<u8> buf(65536);
    while (got < bytes) {
      size_t want = bytes - got;
      if (want > buf.size()) want = buf.size();
      int n = (int)recv(s, (char*)buf.data(), (int)want, 0);
      if (n <= 0) { recv_err = (n < 0) ? sockerr() : 0; break; }
      for (int i = 0; i < n; ++i) {
        if (first_bad < 0 && buf[(size_t)i] != payload[got + (size_t)i])
          first_bad = (long long)(got + (size_t)i);
      }
      got += (size_t)n;
    }
  });

  size_t sent = 0;
  int send_err = 0;
  while (sent < bytes) {
    int n = (int)send(s, (const char*)payload.data() + sent, (int)(bytes - sent), 0);
    if (n <= 0) { send_err = sockerr(); break; }
    sent += (size_t)n;
  }
  rx.join();

  bool same = (first_bad < 0) && (got == bytes);
  std::printf("probe tcp %s:%d: sent %zu/%zu B, got %zu/%zu B, byte-identical: %s\n",
              host, port, sent, bytes, got, bytes, same ? "yes" : "no");
  if (send_err) std::printf("       send error=%d\n", send_err);
  if (recv_err) std::printf("       recv error=%d (超时或对端提前关闭)\n", recv_err);
  if (first_bad >= 0) std::printf("       first mismatch at offset %lld\n", first_bad);

  closesock(s);
  return (send_err == 0) && same;
}

static bool udp_test(const char* host, int port, int count, size_t first_len) {
  sockaddr_in a;
  if (!make_addr(host, port, a)) { std::printf("[FAIL] 目标地址非法: %s\n", host); return false; }
  sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == kInvalid) { std::printf("[FAIL] socket err=%d\n", sockerr()); return false; }

  sockaddr_in me;
  std::memset(&me, 0, sizeof(me));
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = 0;
  if (bind(s, (sockaddr*)&me, (socklen_type)sizeof(me)) != 0) {
    std::printf("[FAIL] bind err=%d\n", sockerr());
    closesock(s);
    return false;
  }
  set_timeout(s, 3);

  bool all_ok = true;
  for (int i = 0; i < count; ++i) {
    size_t len = first_len + (size_t)777 * (size_t)i;
    if (len > 60000) len = 60000;
    std::vector<u8> dgram(len);
    fill_payload(dgram, (uint32_t)(1000 + i));

    int sn = (int)sendto(s, (const char*)dgram.data(), (int)len, 0, (sockaddr*)&a, (socklen_type)sizeof(a));
    if (sn != (int)len) {
      std::printf("[FAIL] udp dgram#%d: send=%d err=%d\n", i, sn, sockerr());
      all_ok = false;
      break;
    }
    std::vector<u8> rbuf(65536);
    sockaddr_in src;
    socklen_type sl = (socklen_type)sizeof(src);
    std::memset(&src, 0, sizeof(src));
    int rn = (int)recvfrom(s, (char*)rbuf.data(), (int)rbuf.size(), 0, (sockaddr*)&src, &sl);
    if (rn < 0) {
      std::printf("[FAIL] udp dgram#%d: recv err=%d (超时或有去无回)\n", i, sockerr());
      all_ok = false;
      break;
    }
    bool same = ((size_t)rn == len) && (std::memcmp(rbuf.data(), dgram.data(), len) == 0);
    std::printf("probe udp dgram#%d: sent %zu B, got %d B, identical: %s (from %s:%u)\n",
                i, len, rn, same ? "yes" : "no", host, (unsigned)ntohs(src.sin_port));
    if (!same) all_ok = false;
  }
  closesock(s);
  return all_ok;
}

static void usage() {
  std::fprintf(stderr,
               "usage: probe tcp <port> [bytes] [seed]\n"
               "       probe udp <port> [count] [first_len]\n");
}

int main(int argc, char** argv) {
  if (argc < 3) { usage(); return 2; }
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { std::fprintf(stderr, "WSAStartup failed\n"); return 2; }
#endif

  const std::string mode = argv[1];
  long port = std::strtol(argv[2], nullptr, 10);
  if (port < 1 || port > 65535) {
    std::fprintf(stderr, "bad port: %s\n", argv[2]);
    usage();
    return 2;
  }

  bool ok = false;
  if (mode == "tcp") {
    size_t bytes = (argc > 3) ? (size_t)std::strtoull(argv[3], nullptr, 10) : (size_t)(128 * 1024);
    uint32_t seed = (argc > 4) ? (uint32_t)std::strtoul(argv[4], nullptr, 10) : 42u;
    if (bytes == 0) bytes = 1;
    ok = tcp_test("127.0.0.1", (int)port, bytes, seed);
  } else if (mode == "udp") {
    int count = (argc > 3) ? std::atoi(argv[3]) : 3;
    size_t first = (argc > 4) ? (size_t)std::strtoull(argv[4], nullptr, 10) : (size_t)2000;
    if (count <= 0) count = 1;
    ok = udp_test("127.0.0.1", (int)port, count, first);
  } else {
    usage();
#ifdef _WIN32
    WSACleanup();
#endif
    return 2;
  }

  std::printf("probe %s: %s\n", mode.c_str(), ok ? "PASS" : "FAIL");
#ifdef _WIN32
  WSACleanup();
#endif
  return ok ? 0 : 1;
}
