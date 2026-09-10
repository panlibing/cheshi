// echo.cpp - 自测用简易回声服务器
// 用法: echo tcp <port>   /   echo udp <port>
// TCP: 顺序 accept, 每连接把收到的字节原样回写直至对端关闭
// UDP: recvfrom 后原样 sendto 回发送方
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using sock_t = SOCKET;
  const sock_t kInvalid = INVALID_SOCKET;
  inline void closesock(sock_t s) { closesocket(s); }
  inline int  sockerr() { return WSAGetLastError(); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <cstring>
  using sock_t = int;
  const sock_t kInvalid = -1;
  inline void closesock(sock_t s) { ::close(s); }
  inline int  sockerr() { return errno; }
#endif

static sock_t bind_udp_or_tcp(int port, int type) {
  sock_t s = socket(AF_INET, type, 0);
  if (s == kInvalid) return kInvalid;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
  sockaddr_in a; std::memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((unsigned short)port);
  if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) { closesock(s); return kInvalid; }
  if (type == SOCK_STREAM && listen(s, 16) != 0) { closesock(s); return kInvalid; }
  return s;
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: echo tcp|udp <port>\n"); return 1; }
  bool isTcp = std::string(argv[1]) == "tcp";
  int  port  = atoi(argv[2]);
#ifdef _WIN32
  WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  sock_t s = bind_udp_or_tcp(port, isTcp ? SOCK_STREAM : SOCK_DGRAM);
  if (s == kInvalid) { std::fprintf(stderr, "bind failed err=%d\n", sockerr()); return 1; }

  char buf[65536];
  if (isTcp) {
    std::printf("echo tcp listening on 127.0.0.1:%d\n", port); std::fflush(stdout);
    for (;;) {
      sock_t c = accept(s, nullptr, nullptr);
      if (c == kInvalid) break;
      std::printf("echo tcp accepted\n"); std::fflush(stdout);
      for (;;) {
        int n = (int)recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        int off = 0;
        while (off < n) {
          int w = (int)send(c, buf + off, n - off, 0);
          if (w <= 0) { off = n; break; }
          off += w;
        }
      }
      closesock(c);
      std::printf("echo tcp closed\n"); std::fflush(stdout);
    }
  } else {
    std::printf("echo udp listening on 127.0.0.1:%d\n", port); std::fflush(stdout);
    sockaddr_storage src{}; socklen_t sl = sizeof(src);
    for (;;) {
      int n = (int)recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&src, &sl);
      if (n < 0) continue;
      sendto(s, buf, n, 0, (sockaddr*)&src, sl);
      std::printf("echo udp echoed %d bytes\n", n); std::fflush(stdout);
    }
  }
  closesock(s);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}
