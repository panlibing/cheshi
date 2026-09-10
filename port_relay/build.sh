#!/bin/sh
# 构建 portrelay (Linux/macOS)
set -e
CXX="${CXX:-g++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -pthread -o portrelay portrelay.cpp
echo "[OK] 已生成 ./portrelay"
