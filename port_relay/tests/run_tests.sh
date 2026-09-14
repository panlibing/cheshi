#!/usr/bin/env bash
# ============================================================================
# run_tests.sh - portrelay 端到端自测 (Linux / macOS)
#
# 依赖: bash + C++17 编译器(g++ 或 clang++, 可用 CXX=clang++ 覆盖)
#       不需要 python / nc / curl 等外部工具 —— 客户端探针来自 tests/probe.cpp
#
# 覆盖范围(与 tests/run_tests.ps1 + tests/run_args_tests.ps1 对齐):
#   A) 数据面: TCP 128KB 伪随机负载逐字节比对; UDP 3 个数据报逐包比对
#   B) 启动写法: 位置写法 / 命名参数(--key=value) / --rule(可重复) / -f 配置文件
#   C) 无参启动自动加载 exe 同目录 portrelay.conf
#   D) -h / -V 输出; 非法参数必须被拒绝
#   E) README.md 与 -h 的选项集合双向同步检查(防止文档再次漂移)
#   F) 单元测试: 纯函数(解析/校验/规范化) + 配置解析 fuzz(tests/unit_tests.cpp)
#
# 用法:
#   bash tests/run_tests.sh          # 退出码 0 = 全过, 1 = 有失败
#   FORCE_BUILD=1 bash tests/run_tests.sh   # 强制重新编译
# ============================================================================
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"     # port_relay 目录
TESTS="$ROOT/tests"
CXX="${CXX:-g++}"
RELAY="$ROOT/portrelay"
ECHO="$TESTS/echo"
PROBE="$TESTS/probe"
UNIT="$TESTS/unit_tests"
README="$ROOT/README.md"

TMP="$(mktemp -d)"
PASS=0
FAIL=0
PIDS=""

say() { printf '%s\n' "$*"; }
hdr() { printf '\n== %s ==\n' "$*"; }
ok()  { PASS=$((PASS + 1)); printf '[ OK ] %s\n' "$*"; }
bad() { FAIL=$((FAIL + 1)); printf '[FAIL] %s\n' "$*"; }

stop_pids() {
  local p
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  sleep 0.2
  for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
  PIDS=""
}

cleanup() {
  stop_pids
  rm -rf "$TMP"
}
trap cleanup EXIT

# 启动一个后台进程, 日志写入 $1(cmd 之后为命令与其参数)
start_bg() {
  local log="$1"; shift
  "$@" >>"$log" 2>&1 &
  PIDS="$PIDS $!"
  sleep 0.5
}

kill_all() { stop_pids; }

# 等待 TCP 端口可连接(用于确认转发布置完成)
wait_tcp() {
  local port="$1" n="${2:-50}" i=0
  while [ "$i" -lt "$n" ]; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then return 0; fi
    i=$((i + 1))
    sleep 0.1
  done
  return 1
}

# 断言某进程启动时应被拒绝, 并可校验错误输出片段
fail_run() {
  local desc="$1" want="$2"; shift 2
  local out rc
  out="$("$@" 2>&1)"; rc=$?
  if [ "$rc" -eq 0 ]; then bad "$desc (本应失败, 实际退出码 0)"; return; fi
  if [ -n "$want" ] && ! printf '%s\n' "$out" | grep -q -F -- "$want"; then
    bad "$desc (错误信息未包含 '$want')"; return
  fi
  ok "$desc"
}

build_all() {
  if [ "${FORCE_BUILD:-0}" = "1" ] && [ -x "$RELAY" ]; then rm -f "$RELAY"; fi
  if [ ! -x "$RELAY" ]; then
    say "-- 构建 portrelay (CXX=$CXX)"
    ( cd "$ROOT" && CXX="$CXX" sh ./build.sh ) || { say '[FAIL] portrelay 构建失败'; exit 1; }
  fi
  if [ ! -x "$ECHO" ]; then
    say '-- 构建 tests/echo'
    "$CXX" -std=c++17 -O2 -Wall -Wextra -o "$ECHO" "$TESTS/echo.cpp" \
      || { say '[FAIL] echo 构建失败'; exit 1; }
  fi
  if [ ! -x "$PROBE" ]; then
    say '-- 构建 tests/probe'
    "$CXX" -std=c++17 -O2 -Wall -Wextra -pthread -o "$PROBE" "$TESTS/probe.cpp" \
      || { say '[FAIL] probe 构建失败'; exit 1; }
  fi
  if [ ! -x "$UNIT" ]; then
    say '-- 构建 tests/unit_tests'
    # -Wno-unused-function: unit_tests.cpp 通过 #include 复用主文件, 主文件里
    # 未被本测试用到的 static 函数会触发 unused-function, 这里显式豁免。
    "$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-function -pthread \
      -o "$UNIT" "$TESTS/unit_tests.cpp" \
      || { say '[FAIL] unit_tests 构建失败'; exit 1; }
  fi
}

build_all

# ---------------------------------------------------------------------------
hdr 'A) 数据面: TCP 128KB 逐字节比对 / UDP 多数据报比对'
start_bg "$TMP/echo_tcp.log" "$ECHO" tcp 47002
start_bg "$TMP/relay_tcp.log" "$RELAY" tcp 47001 127.0.0.1 47002 127.0.0.1 -b 512
wait_tcp 47001 || bad 'TCP relay(47001) 未在 5s 内监听'
"$PROBE" tcp 47001 131072 42 && ok 'TCP relay(47001)->echo(47002) 128KB 逐字节一致' \
                              || bad 'TCP 128KB 负载比对失败'
"$PROBE" tcp 47001 65536 7 && ok 'TCP 二次连接 64KB 逐字节一致(每客户端独立线程)' \
                           || bad 'TCP 二次连接比对失败'
kill_all

start_bg "$TMP/echo_udp.log" "$ECHO" udp 47003
start_bg "$TMP/relay_udp.log" "$RELAY" udp 47004 127.0.0.1 47003 127.0.0.1 -u 30 -b 512
sleep 0.3
"$PROBE" udp 47004 3 2000 && ok 'UDP relay(47004)->echo(47003) 3 个数据报(2000/2777/3554B)逐包一致' \
                          || bad 'UDP 数据报比对失败'
kill_all

# ---------------------------------------------------------------------------
hdr 'B) 启动写法: 位置 / 命名 / --rule / --key=value / -f 配置文件'
start_bg "$TMP/echo_b_tcp.log" "$ECHO" tcp 47102
start_bg "$TMP/echo_b_udp.log" "$ECHO" udp 47103
sleep 0.3

start_bg "$TMP/relay_pos.log" "$RELAY" tcp 47117 127.0.0.1 47102 127.0.0.1 -b 256
wait_tcp 47117 && "$PROBE" tcp 47117 4096 11 >/dev/null 2>&1 \
  && ok '0) 位置写法(向后兼容)' || bad '0) 位置写法'

start_bg "$TMP/relay_named.log" "$RELAY" -M tcp -l 47101 -t 127.0.0.1:47102 -v
wait_tcp 47101 && "$PROBE" tcp 47101 4096 12 >/dev/null 2>&1 \
  && ok '1) 命名参数 -M/-l/-t' || bad '1) 命名参数 -M/-l/-t'

start_bg "$TMP/relay_rule.log" "$RELAY" --rule 'tcp:47104->127.0.0.1:47102'
wait_tcp 47104 && "$PROBE" tcp 47104 4096 13 >/dev/null 2>&1 \
  && ok '2) --rule 规则串' || bad '2) --rule 规则串'

start_bg "$TMP/relay_multi.log" "$RELAY" \
  --rule 'tcp:47105->127.0.0.1:47102' --rule 'udp:47106->127.0.0.1:47103'
wait_tcp 47105 && "$PROBE" tcp 47105 4096 14 >/dev/null 2>&1 \
  && ok '3) 多规则同进程 #1 (TCP 47105)' || bad '3) 多规则同进程 #1 (TCP 47105)'
"$PROBE" udp 47106 1 512 >/dev/null 2>&1 \
  && ok '3) 多规则同进程 #2 (UDP 47106)' || bad '3) 多规则同进程 #2 (UDP 47106)'

# 配置文件语义: 逐行解析并在每行结束后 flush_rule, 因此每一行都必须是
# "一条完整规则"。单独的 mode=tcp / listen=... 这种 key=value 行会因
# 规则不完整被拒绝(见下面 args_bad.conf 负例)。
cat >"$TMP/args_test.conf" <<'EOF'
# args_test.conf - test only
; 分号注释同样合法
tcp 47107 127.0.0.1 47102 127.0.0.1 -b 256
tcp 47109 127.0.0.1 47102 127.0.0.1
verbose = off
EOF
cat >"$TMP/args_bad.conf" <<'EOF'
mode = tcp
EOF
start_bg "$TMP/relay_conf.log" "$RELAY" -f "$TMP/args_test.conf"
wait_tcp 47107 && "$PROBE" tcp 47107 4096 15 >/dev/null 2>&1 \
  && ok '4) -f 配置文件: 第1条规则(位置写法行)' || bad '4) -f 配置文件: 第1条规则(位置写法行)'
wait_tcp 47109 && "$PROBE" tcp 47109 4096 16 >/dev/null 2>&1 \
  && ok '4) -f 配置文件: 同文件第2条规则' || bad '4) -f 配置文件: 同文件第2条规则'
fail_run '4) -f 配置文件: 单独的 key=value 行因规则不完整被拒' '转发规则不完整' \
  "$RELAY" -f "$TMP/args_bad.conf"

start_bg "$TMP/relay_kv.log" "$RELAY" --mode=tcp --listen-port=47111 --target=127.0.0.1:47102
wait_tcp 47111 && "$PROBE" tcp 47111 4096 17 >/dev/null 2>&1 \
  && ok '5) --key=value 写法' || bad '5) --key=value 写法'

start_bg "$TMP/relay_unamed.log" "$RELAY" -M udp -l 47115 -t 127.0.0.1:47103 -u 30 -m 64
sleep 0.3
"$PROBE" udp 47115 1 512 >/dev/null 2>&1 \
  && ok '6) UDP 命名参数 -M/-l/-t/-u/-m' || bad '6) UDP 命名参数 -M/-l/-t/-u/-m'

# ---------------------------------------------------------------------------
hdr 'C) 无参启动自动加载 exe 同目录 portrelay.conf'
mkdir -p "$TMP/auto"
cp "$RELAY" "$TMP/auto/portrelay"
cat >"$TMP/auto/portrelay.conf" <<'EOF'
; 每行一条完整规则(位置写法)
tcp 47113 127.0.0.1 47102 127.0.0.1
EOF
start_bg "$TMP/relay_auto.log" "$TMP/auto/portrelay"
wait_tcp 47113 && "$PROBE" tcp 47113 4096 18 >/dev/null 2>&1 \
  && ok '7) 无参自动加载 portrelay.conf' || bad '7) 无参自动加载 portrelay.conf'
kill_all

# ---------------------------------------------------------------------------
hdr 'D) -h / -V 输出与非法参数拒绝'
help_out="$("$RELAY" -h 2>&1)"; rc=$?
[ "$rc" -eq 0 ] && ok '-h 退出码 0' || bad "-h 退出码 0 (实际 $rc)"
printf '%s\n' "$help_out" | grep -q -F -- '--rule' \
  && ok '-h 列出 --rule(多规则写法)' || bad '-h 未列出 --rule'
printf '%s\n' "$help_out" | grep -q -F -- '--listen-host' \
  && ok '-h 列出 --listen-host' || bad '-h 未列出 --listen-host'
printf '%s\n' "$help_out" | grep -q -F -- '配置文件格式' \
  && ok '-h 说明配置文件格式' || bad '-h 未说明配置文件格式'

ver_out="$("$RELAY" -V 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && printf '%s\n' "$ver_out" | grep -Eq '[0-9]+\.[0-9]+\.[0-9]+'; then
  ok "-V 输出版本号: $(printf '%s' "$ver_out" | tr -d '\r\n' | tr -s ' ')"
else
  bad '-V 未输出版本号'
fi

fail_run '未知选项被拒 (--badopt)'            'badopt'          "$RELAY" --badopt
fail_run 'target 缺端口被拒'                  'host:port'       "$RELAY" -M tcp -l 9000 -t 10.0.0.1
fail_run '规则不完整被拒'                     '转发规则不完整'   "$RELAY" --mode tcp -l 9000
fail_run '位置写法缺目标端口被拒'             '转发规则不完整'   "$RELAY" tcp 9000 1.2.3.4:80
fail_run '监听端口超范围被拒'                 '90000'           "$RELAY" -M tcp -l 90000 -t 1.2.3.4:80
fail_run '目标端口非数字被拒'                 '1.2.3.4:80'      "$RELAY" -M tcp -l 9000 -tp 1.2.3.4:80
fail_run '非法 mode 被拒'                     'ftp'             "$RELAY" -M ftp -l 9000 -t 1.2.3.4:80
fail_run '坏规则串被拒'                       'bad-spec'        "$RELAY" --rule bad-spec
fail_run '配置文件不存在被拒'                 'no_such_file.conf' "$RELAY" -f no_such_file.conf

mkdir -p "$TMP/noconf"
cp "$RELAY" "$TMP/noconf/portrelay"
fail_run '无参且无默认 conf 被拒'             'portrelay.conf'  "$TMP/noconf/portrelay"

# ---------------------------------------------------------------------------
hdr 'E) README.md 与 -h 选项同步检查'
if sync_out="$("$TESTS/check_docs_sync.sh" "$RELAY" 2>&1)"; then
  ok "README 与 -h 长选项双向一致($(printf '%s\n' "$sync_out" | grep -c '^ok ') 项检查通过)"
else
  bad 'README 与 -h 长选项不一致'
  printf '%s\n' "$sync_out" | sed 's/^/       /'
fi

# ---------------------------------------------------------------------------
hdr 'F) 单元测试: 纯函数 + 配置解析 fuzz (tests/unit_tests)'
if unit_out="$("$UNIT" 2>&1)"; then
  ok "单元测试全部通过 ($(printf '%s\n' "$unit_out" | grep -c '^==') 段输出)"
else
  bad '单元测试失败'
  printf '%s\n' "$unit_out" | sed 's/^/       /'
fi

# ---------------------------------------------------------------------------
printf '\n========== 结果: %d 通过 / %d 失败 ==========\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
