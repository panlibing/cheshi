// unit_tests.cpp - portrelay 单元测试(纯函数 + 配置解析鲁棒性/fuzz)
//
// 思路: 定义 PORTRELAY_NO_MAIN 后直接 #include 主源文件, 从而复用其中的
//       static 内部函数(解析/校验/格式化), 无需改动主程序的可见性。
//       - 正常路径: 用确定性的用例逐项断言;
//       - 鲁棒性:   用确定性伪随机字节流(xorshift32)冲刷各解析入口, 只要求
//                   "不崩溃 + 结果自洽", 配合 ASan/UBSan 抓越界/未定义行为。
//
// 构建(与 run_tests.sh 一致):
//   Linux/macOS: g++  -std=c++17 -O2 -Wall -Wextra -pthread -o unit_tests unit_tests.cpp
//   Windows MSVC: cl /nologo /O2 /std:c++17 /EHsc /utf-8 /W3 unit_tests.cpp ws2_32.lib
//   MinGW:        g++ -std=c++17 -O2 -Wall -Wextra -pthread -o unit_tests.exe unit_tests.cpp -lws2_32
//
// 退出码: 0 = 全部通过; 1 = 有失败。

#define PORTRELAY_NO_MAIN
#include "../portrelay.cpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) { ++g_pass; }                                                    \
    else {                                                                     \
      ++g_fail;                                                                \
      std::printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                          \
  } while (0)

#define CHECK_STR(actual, expected)                                            \
  do {                                                                         \
    std::string a_ = (actual);                                                 \
    std::string e_ = (expected);                                               \
    if (a_ == e_) { ++g_pass; }                                                \
    else {                                                                     \
      ++g_fail;                                                                \
      std::printf("[FAIL] %s:%d: got \"%s\" want \"%s\"\n", __FILE__,          \
                  __LINE__, a_.c_str(), e_.c_str());                           \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// 基础字符串工具
// ---------------------------------------------------------------------------
static void test_string_utils() {
  CHECK_STR(trim_str("  abc  "), "abc");
  CHECK_STR(trim_str("\t a \r\n"), "a");
  CHECK_STR(trim_str(""), "");
  CHECK_STR(trim_str("   "), "");
  CHECK_STR(lower_str("TCP"), "tcp");
  CHECK_STR(lower_str("Udp"), "udp");

  {
    std::vector<std::string> v = split_ws("a  b\tc\r\nd");
    CHECK(v.size() == 4);
    CHECK(v.size() == 4 && v[0] == "a" && v[1] == "b" && v[2] == "c" && v[3] == "d");
  }
  CHECK(split_ws("   ").empty());
}

// ---------------------------------------------------------------------------
// 整数 / 日志级别解析
// ---------------------------------------------------------------------------
static void test_int_and_level() {
  int v = 0;
  CHECK(parse_int_val("9000", v) && v == 9000);
  CHECK(parse_int_val("  42 ", v) && v == 42);
  CHECK(parse_int_val("-5", v) && v == -5);
  CHECK(!parse_int_val("", v));
  CHECK(!parse_int_val("12x", v));
  CHECK(!parse_int_val("1.5", v));

  int lv = -1;
  CHECK(parse_log_level("error", lv) && lv == LOG_ERROR);
  CHECK(parse_log_level("WARN", lv) && lv == LOG_WARN);
  CHECK(parse_log_level(" Info ", lv) && lv == LOG_INFO);
  CHECK(parse_log_level("debug", lv) && lv == LOG_DEBUG);
  CHECK(parse_log_level("3", lv) && lv == LOG_DEBUG);
  CHECK(!parse_log_level("trace", lv));
  CHECK(!parse_log_level("", lv));

  CHECK(truthy("on"));
  CHECK(truthy("TRUE"));
  CHECK(truthy("1"));
  CHECK(truthy(""));           // 空值视为真(与 key 无取值的写法兼容)
  CHECK(!truthy("off"));
  CHECK(!truthy("false"));
  CHECK(!truthy("0"));
}

// ---------------------------------------------------------------------------
// host:port 解析
// ---------------------------------------------------------------------------
static void test_parse_hostport() {
  std::string h, e;
  int p = 0;

  CHECK(parse_hostport("192.168.1.10:3389", h, p, e) && h == "192.168.1.10" && p == 3389);
  CHECK(parse_hostport("9000", h, p, e) && h.empty() && p == 9000);
  CHECK(parse_hostport("[::1]:9000", h, p, e) && h == "::1" && p == 9000);
  CHECK(parse_hostport(" [2001:db8::1]:80 ", h, p, e) && h == "2001:db8::1" && p == 80);

  CHECK(!parse_hostport("", h, p, e));
  CHECK(!parse_hostport("[::1", h, p, e));            // 缺 ']'
  CHECK(!parse_hostport("[::1]9000", h, p, e));       // 缺 ':'
  CHECK(!parse_hostport("::1:9000", h, p, e));        // 裸 IPv6 需带方括号
  CHECK(!parse_hostport("host:abc", h, p, e));        // 端口非数字
}

// ---------------------------------------------------------------------------
// 规则串解析
// ---------------------------------------------------------------------------
static void test_rule_spec() {
  std::vector<std::string> out;
  std::string e;

  CHECK(rule_spec_to_tokens("tcp:9000->192.168.1.10:3389", out, e));
  CHECK(out.size() == 4 && out[0] == "tcp" && out[1] == "9000" &&
        out[2] == "192.168.1.10" && out[3] == "3389");

  CHECK(rule_spec_to_tokens("9000->10.0.0.1:80", out, e));      // 省略模式 => tcp
  CHECK(out.size() == 4 && out[0] == "tcp");

  CHECK(rule_spec_to_tokens("udp:127.0.0.1:5353->8.8.8.8:53", out, e));
  CHECK(out.size() == 5 && out[0] == "udp" && out[1] == "5353" &&
        out[2] == "8.8.8.8" && out[3] == "53" && out[4] == "127.0.0.1");

  CHECK(!rule_spec_to_tokens("bad-spec", out, e));              // 缺 '->'
  CHECK(!rule_spec_to_tokens("tcp:9000->:53", out, e));         // 目标主机为空
}

// ---------------------------------------------------------------------------
// 配置文件行 -> tokens
// ---------------------------------------------------------------------------
static void test_config_line() {
  std::vector<std::string> toks;
  std::string e;

  CHECK(config_line_to_tokens("mode = tcp", toks, e));
  CHECK(toks.size() == 2 && toks[0] == "--mode" && toks[1] == "tcp");

  CHECK(config_line_to_tokens("listen_port = 9000", toks, e));  // 下划线归一为 '-'
  CHECK(toks.size() == 2 && toks[0] == "--listen-port" && toks[1] == "9000");

  CHECK(config_line_to_tokens("target = 10.0.0.1:80", toks, e));
  CHECK(toks.size() == 2 && toks[0] == "--target" && toks[1] == "10.0.0.1:80");

  CHECK(config_line_to_tokens("rule = tcp:9000->10.0.0.1:80", toks, e));
  CHECK(toks.size() == 2 && toks[0] == "--rule" && toks[1] == "tcp:9000->10.0.0.1:80");

  CHECK(config_line_to_tokens("verbose = off", toks, e));
  CHECK(toks.empty());                                          // verbose=off 直接忽略

  CHECK(config_line_to_tokens("verbose = on", toks, e));
  CHECK(toks.size() == 1 && toks[0] == "--verbose");

  CHECK(config_line_to_tokens("tcp 9000 127.0.0.1 80 127.0.0.1", toks, e));
  CHECK(toks.size() == 5);                                      // 位置写法按空白切分

  CHECK(!config_line_to_tokens("no_such_key = 1", toks, e));    // 未知键被拒
  CHECK(!config_line_to_tokens("mode =", toks, e));             // 缺取值
}

// ---------------------------------------------------------------------------
// 规则规范化(热重载比较用)
// ---------------------------------------------------------------------------
static void test_rule_spec_canonical() {
  Options a; a.mode = "tcp"; a.listen_port = 9000; a.target_host = "10.0.0.1";
  a.target_port = 80; a.max_conns = 100;
  Options b = a;
  CHECK_STR(rule_spec(a), rule_spec(b));        // 相同规则 => 相同规范化串

  Options c = a; c.max_conns = 101;
  CHECK(rule_spec(a) != rule_spec(c));          // 任一字段不同 => 不同

  Options d = a; d.listen_host = "*";           // 空监听地址与 '*' 视为等价
  CHECK_STR(rule_spec(a), rule_spec(d));
}

// ---------------------------------------------------------------------------
// client_key: 源地址+源端口 => 稳定键
// ---------------------------------------------------------------------------
static void test_client_key() {
  sockaddr_in sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(12345);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  sockaddr_storage ss;
  std::memset(&ss, 0, sizeof(ss));
  std::memcpy(&ss, &sa, sizeof(sa));
  std::string k = client_key(ss, (socklen_t)sizeof(sa));
  CHECK_STR(k, "127.0.0.1|12345");
}

// ---------------------------------------------------------------------------
// fuzz: 用确定性伪随机字节流冲刷各解析入口, 只要求不崩溃且结果自洽
// ---------------------------------------------------------------------------
static void test_fuzz_parsers() {
  uint32_t x = 0x9E3779B9u;
  auto next = [&x]() {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
  };
  const char* alphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                         ".:->/=[]#;, \t\"'\\%$@!*()+_";

  for (int iter = 0; iter < 20000; ++iter) {
    int len = (int)(next() % 48);
    std::string s;
    for (int i = 0; i < len; ++i) {
      if ((next() & 3) == 0) s.push_back((char)(next() & 0xFF));         // 原始字节
      else                   s.push_back(alphabet[next() % 70]);         // 结构化字符
    }

    std::string h, e;
    int p = 0;
    (void)parse_hostport(s, h, p, e);

    std::vector<std::string> toks;
    (void)rule_spec_to_tokens(s, toks, e);
    (void)config_line_to_tokens(s, toks, e);

    int iv = 0;
    (void)parse_int_val(s, iv);
    (void)parse_log_level(s, iv);
    (void)truthy(s);

    // 完整走一遍"输入 -> 规则解析 -> 规则落地", 不得抛异常/崩溃
    ParseState st;
    std::string e2;
    std::vector<std::string> one{s};
    if (feed_tokens(one, st, e2)) {
      (void)flush_rule(st, e2);
    }
    // 单条规则若成功落地, 其端口必须落在合法区间(自洽性检查)
    for (const Options& o : st.rules) {
      CHECK(o.listen_port >= 0 && o.listen_port <= 65535);
      CHECK(o.target_port >= 0 && o.target_port <= 65535);
    }
  }
}

int main() {
#ifdef _WIN32
  WSADATA wsa;                 // client_key() 依赖 getnameinfo, 需先初始化 Winsock
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  std::printf("== portrelay unit tests ==\n");
  test_string_utils();
  test_int_and_level();
  test_parse_hostport();
  test_rule_spec();
  test_config_line();
  test_rule_spec_canonical();
  test_client_key();
  test_fuzz_parsers();

  std::printf("== %d passed / %d failed ==\n", g_pass, g_fail);
#ifdef _WIN32
  WSACleanup();
#endif
  return g_fail == 0 ? 0 : 1;
}
