#!/bin/sh
# 构建 portrelay (Linux/macOS)
#
# 环境变量:
#   CXX=g++|clang++    选择编译器(默认 g++)
#   CXXFLAGS="..."     编译选项(默认 "-O2 -Wall -Wextra")
#   WERROR=1           把告警当错误(CI 用)
#   SANITIZE=1         追加 AddressSanitizer + UndefinedBehaviorSanitizer(调试用)
set -e
CXX="${CXX:-g++}"
CXXFLAGS="${CXXFLAGS:--O2 -Wall -Wextra}"

EXTRA=""
[ "${WERROR:-0}" = "1" ] && EXTRA="$EXTRA -Werror"
if [ "${SANITIZE:-0}" = "1" ]; then
  EXTRA="$EXTRA -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1"
fi

# shellcheck disable=SC2086  # CXXFLAGS/EXTRA 需要按空白拆分
"$CXX" -std=c++17 $CXXFLAGS $EXTRA -pthread -o portrelay portrelay.cpp
echo "[OK] 已生成 ./portrelay"
