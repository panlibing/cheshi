# Git 钩子（版本化）

本目录存放随仓库一起版本化的 Git 钩子，避免"钩子只装在某个人电脑上、clone 后失效"的问题。

## 一次性启用（每次 clone / 换机器后执行一次）

```bash
git config core.hooksPath .githooks
```

验证：

```bash
git config --get core.hooksPath     # 应输出 .githooks
```

## 包含的钩子

| 钩子 | 作用 |
| --- | --- |
| `pre-commit` | 提交前拦截：① 构建产物（`*.exe` `*.obj` `*.pdb` `*.dll` `*.so` `*.o` …）；② 新增文件仍被 `.gitignore` 命中的（本机临时文件被 `git add -f` 强推进来的）；③ 单文件超过 5 MiB 的大文件 |

## 触发时的处理办法

```bash
git restore --staged <文件>        # 只是撤出暂存区，文件仍在磁盘上
git rm --cached <文件>             # 该文件之前已经提交进仓库了
git commit --no-verify             # 确认确实要提交时临时绕过
BIG_LIMIT_BYTES=10485760 git commit -m "..."   # 临时放宽大文件阈值
```

## 为什么还需要 CI 再查一遍

`core.hooksPath` 是本机配置，别人 clone 后不执行上面那行配置、或者直接用 `--no-verify`，钩子就形同虚设。
因此 CI 里另有一道独立检查（`.github/workflows/repo-hygiene.yml`），扫描**仓库中已被跟踪**的文件里是否混入构建产物。

两道线合起来才是完整闭环：

- pre-commit：提交前拦住，给即时反馈；
- CI hygiene：合并前拦住，作为最终底线。
