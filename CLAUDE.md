# CLAUDE.md

自研 valkey proxy。C++23，追求高性能与高可维护。
（注意：仓库目录名是 `gemini-proxy`，但项目与 Gemini 无关。）

## 先读这两份文档

- [docs/architecture-decisions.md](docs/architecture-decisions.md) — 三个核心架构决策的调研与结论。**动手前必读**，不要重新论证已定的技术选型。
- [docs/development-plan.md](docs/development-plan.md) — M0~M6 里程碑与任务分解，含每个里程碑的验收标准和已完成部分的实施记录。**当前进度以此文档中的状态标记为准。**

已定的架构（细节见上述文档）：shared-nothing thread-per-core；完成式（proactor）IO 抽象，Linux 上 io_uring 为主、epoll 为强制 fallback，macOS 上 kqueue 仅供开发；自研薄 C++20 无栈协程层 + 直接用 liburing；轻依赖精选。

## 构建与测试

```sh
cmake --preset debug          # 预设：debug / release / asan-ubsan / tsan
cmake --build --preset debug
ctest --preset debug
./build/debug/src/proxyd --version
./build/debug/benchmarks/vkp_bench
```

**首次 configure 需要下载全部依赖源码，耗时约 8 分钟**（CPM 逐个 git clone），不是卡死。设置 `CPM_SOURCE_CACHE` 可在多个构建目录间复用。

依赖版本统一锁在 [cmake/Dependencies.cmake](cmake/Dependencies.cmake)。CMake 选项一律用 `VKP_` 前缀。`-Werror` 等警告选项挂在 `vkp_options` interface target 上，只作用于自有代码。

## 格式化

```sh
./scripts/format.sh           # 就地格式化；--check 仅校验（CI 用这个）
```

clang-format **必须是 20.1.8**——跨大版本输出会变，脚本会校验主版本号并在不符时报错。本机已装在 `~/Library/Python/3.9/bin/clang-format`（不在默认 PATH，脚本会自己找到）。重装：`python3 -m pip install --user "clang-format==20.1.8"`。

## Fuzz

```sh
VKP_APT_MIRROR=http://mirrors.aliyun.com/ubuntu-ports/ ./scripts/fuzz.sh [秒数]   # 默认 60
```

Apple Clang 没有 libFuzzer 运行时，macOS 上脚本自动经 Docker 跑 Linux 原生 fuzz（构建缓存在
`vkp-fuzz-cache` 卷里，首跑慢后续快）。触碰解析器后至少短跑一次。新语料会写回
`fuzz/corpus/`，定期用 `-merge=1` 精简后提交。

## 本机环境

- macOS（arm64），CMake 4.4，Ninja 已装，Apple Clang 21。
- **没有 `gh` CLI**。查 CI 状态用 GitHub API：
  ```sh
  curl -fsSL "https://api.github.com/repos/lostpigxx/gemini-proxy/actions/runs?per_page=1&branch=main" \
    | python3 -c "import json,sys; d=json.load(sys.stdin)['workflow_runs'][0]; print(d['status'], d['conclusion'], d['id'])"
  ```
  该 API 偶发 504，轮询时要容错。取具体 job 结果用 `/actions/runs/<id>/jobs`；**job 日志需要认证，匿名取会 403**，失败时优先在本地复现而不是死磕日志。
- macOS 上只能构建和跑单测，io_uring 相关代码的真实验证需要 Linux。
- **本机网络直连 docker.io 与 Ubuntu 官方 apt 源都极慢/不通**：拉镜像用
  `docker.m.daocloud.io/library/...`（再 `docker tag` 回原名），容器内 apt 用
  `http://mirrors.aliyun.com/ubuntu-ports/`（arm64；须 http，基础镜像没装 ca-certificates）。
  GitHub 直连可用。
- Docker 里可跑 Linux 构建/GCC 交叉验证/fuzz；`vkp-fuzz-cache` 卷内有 CPM 缓存与增量构建目录
  （`/cache/build`、`/cache/build-gcc`）。
- Ubuntu 24.04 的 clang-18 定义 `__cpp_concepts=201907L`，libstdc++ 的 `<expected>` 等
  concepts-gated 头会被 guard 掉——别用 `std::expected`（Clang 19+ 才行，超出项目基线）。

## 工作约定

- 提交信息用 conventional commits（`feat:` / `fix:` / `docs:` / `perf:` / `test:`）。当前单人开发，直接提交到 `main`。
- 新代码必须带测试；触碰 RESP 解析器必须过 fuzz 短跑。
- 设计有分叉时，先在 `docs/design/` 写一页决策记录再动手。
- 每个里程碑完成时更新开发计划文档：标记状态、记录与计划的偏差、回填实测数据（性能基线等）。
- 验证命令的退出码时注意别被管道骗了（`cmd | head; echo $?` 拿到的是 `head` 的退出码）——M0 阶段的格式检查就因此假绿过一次。
- zsh 下多个 glob 只要有一个无匹配，**整条命令直接失败**（`ls crash-* leak-*` 在只有
  crash 文件时什么都不列）——M1 阶段曾因此漏看一个 fuzz crash 现场。查 fuzz 产物用
  `ls crash-* 2>/dev/null; ls oom-* 2>/dev/null` 分开写。
- 长时间后台任务别用 `docker run --rm`：容器异常退出时现场（日志、退出码）全没了。
