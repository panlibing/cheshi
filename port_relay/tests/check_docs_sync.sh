#!/usr/bin/env bash
# ============================================================================
# check_docs_sync.sh - 校验 README.md 与 -h 帮助/源码是否一致
#
#   1) 长选项集合(双向):
#      * README 必须覆盖 -h 中出现的每一个长选项(否则用户按文档写参数会缺项);
#      * README 不得出现 -h 中不存在的长选项(防止文档残留已改名/删除的旧选项)。
#   2) 行数标注: README 文件树里写的「文件名(... N 行)」必须与文件真实行数
#      (wc -l) 一致 —— 源码改完文档静默漂移, 只有这里守得住。
#
# 只做文本层面的集合比对, 不启动任何转发, 因此不需要编译器, 可直接跑在
# 已构建好的二进制上(也可在 CI 里当作"文档漂移"守门员)。
#
# 用法:
#   tests/check_docs_sync.sh [可执行文件路径]
#     默认取 ../portrelay (Windows 下自动回退到 ../portrelay.exe)
#
# 退出码: 0 = 一致 / 1 = 不一致 / 2 = 环境缺失(找不到可执行文件或 README)
# ============================================================================
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
relay="${1:-$root/portrelay}"
readme="$root/README.md"

if [ ! -x "$relay" ] && [ -x "$relay.exe" ]; then relay="$relay.exe"; fi
if [ ! -x "$relay" ]; then
  printf '找不到可执行文件: %s\n请先构建(build.sh / build.bat), 或把路径作为第一个参数传入。\n' "$relay" >&2
  exit 2
fi
if [ ! -f "$readme" ]; then
  printf '找不到 README: %s\n' "$readme" >&2
  exit 2
fi

help_out="$("$relay" -h 2>&1)"
# --key=value 这类只取到 --key, 与文档写法无关
help_opts="$(printf '%s\n' "$help_out" | grep -oE -- '--[a-z][a-z0-9-]*' | sort -u)"
readme_opts="$(grep -oE -- '--[a-z][a-z0-9-]*' "$readme" | sort -u)"
n_help="$(printf '%s\n' "$help_opts" | grep -c . || true)"

fail=0
missing="$(comm -23 <(printf '%s\n' "$help_opts") <(printf '%s\n' "$readme_opts"))" || true
extra="$(comm -13 <(printf '%s\n' "$help_opts") <(printf '%s\n' "$readme_opts"))" || true

if [ -n "$missing" ]; then
  fail=1
  printf 'FAIL README 缺少 -h 中的长选项:\n%s\n' "$missing"
else
  printf 'ok   README 覆盖 -h 的全部 %s 个长选项\n' "$n_help"
fi

if [ -n "$extra" ]; then
  fail=1
  printf 'FAIL README 出现 -h 中不存在的长选项:\n%s\n' "$extra"
else
  printf 'ok   README 未出现 -h 之外的长选项\n'
fi

# --- 行数标注: README 文件树里的「文件名(... N 行)」必须与真实行数一致 ------
names="$(grep -oE -- '[A-Za-z0-9_][A-Za-z0-9_.-]*\.(cpp|hpp|h|cc|sh|ps1|bat|md|yml|txt)' "$readme" | sort -u)"
n_checked=0
for name in $names; do
  n_doc="$(grep -F -- "$name" "$readme" | grep -oE '[0-9]+ 行' | head -n 1 | tr -cd '0-9')"
  [ -n "$n_doc" ] || continue            # README 未标注行数 -> 不检查
  file=""
  for cand in "$root/$name" "$here/$name"; do
    if [ -f "$cand" ]; then file="$cand"; break; fi
  done
  [ -n "$file" ] || continue             # README 提到的名字在仓库里找不到 -> 不检查
  n_real="$(wc -l <"$file" | tr -cd '0-9')"
  n_checked=$((n_checked + 1))
  if [ "$n_doc" = "$n_real" ]; then
    printf 'ok   README 行数标注正确: %s(%s 行)\n' "$name" "$n_real"
  else
    fail=1
    printf 'FAIL README 行数标注漂移: %s 标注 %s 行, 实际 %s 行\n' "$name" "$n_doc" "$n_real"
  fi
done
printf 'ok   行数标注检查完成(%s 个文件, 未标注的文件自动跳过)\n' "$n_checked"

if [ "$fail" -eq 0 ]; then
  printf '结果: README 与 -h 选项/行数标注一致\n'
else
  printf '结果: README 与 -h 选项/行数标注不一致\n'
fi
exit "$fail"
