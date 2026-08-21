# 架构决策调研报告

> 日期：2026-08-21
> 状态：调研完成，待评审定稿
>
> 本文档针对 valkey proxy 项目启动前的三个核心架构决策做调研对比：
> ① 线程与 IO 模型；② 异步编程风格（协程 vs 回调）；③ 依赖策略。
> 每节末尾给出明确推荐，最后一节是三者组合成的整体架构结论。

---

## 决策一：线程与 IO 模型

### 1.1 线程模型对比

| 模型 | 代表项目 | 优势 | 劣势 |
|---|---|---|---|
| Shared-nothing 每线程独立事件循环（thread-per-core） | Envoy、Dragonfly/helio、memcached | 热路径零锁；cache locality 最佳（连接状态、解析缓冲全在本地 L1/L2） | 长连接场景负载可能倾斜；后端连接数 = 线程数 × 后端节点数 |
| 多进程（每进程单线程） | nginx | 隔离性好 | 连接池按进程分裂无法复用；共享 stats/热更新复杂；阻塞卡死整个 worker（Cloudflare 弃用 nginx 的核心原因） |
| 全局队列 + work-stealing 线程池 | Pingora (tokio) | 可跨线程共享后端连接池、无负载倾斜 | 任务迁移损失 locality；需要全面的同步设计；C++ 手写成本高 |

关键判断：

- **Pingora 选 work-stealing 的动机（共享 HTTP 上游连接池）在 redis/valkey 协议下不成立**——协议支持 pipelining，每线程每后端 1~2 条连接即可打满带宽，per-thread 连接池的"放大"总量完全可控（如 16 线程 × 32 后端 × 2 ≈ 1024 条），远小于 HTTP 代理的痛点。
- **连接分配**：Envoy 默认用 SO_REUSEPORT 每 worker 一个 listen socket，由内核按四元组哈希分发，高连接数下足够均匀。valkey proxy 客户端连接通常量大，SO_REUSEPORT 足够，无需主线程 accept 再分发。
- Valkey 8 自身保持单主线程执行命令 + I/O offload 线程（1.19M RPS），是存储端为保原子性的妥协；proxy 无此约束，不必效仿。

### 1.2 io_uring vs epoll（2026 年现状）

- **性能**：ping-pong（请求-响应）型负载 io_uring 占优；streaming 型 epoll 反而更快。2025-12 的 arXiv 论文结论："简单替换接口收益甚微，围绕它重构才有 2× 级收益"。实证数字：multishot recv 约 +6~8% QPS；SQPOLL 约 +30% 但独占一核；zero-copy recv 最高 2.5×。
- **成熟度 / 受限环境（重要）**：安全顾虑仍在——Linux 6.6 引入 `kernel.io_uring_disabled` sysctl（源自 Google）；RHEL 9 默认禁用、仅 Technology Preview；**Docker 默认 seccomp profile 至今未放行任何 io_uring 系统调用**（已核实 moby 当前 default.json）。
  **结论：epoll fallback 不是可选项，是必需项，必须是一等公民（运行时探测 + 配置开关）。**
- **内核门槛**：multishot accept 需 5.19+，multishot recv 需 6.0+，recv bundle 需 6.10+。

### 1.3 跨平台抽象层的高度

核心矛盾：io_uring 是**完成式**（proactor），kqueue/epoll 是**就绪式**（reactor）。

- asio 的做法：POSIX 上"用 reactor 模拟 proactor"（就绪后立即执行非阻塞 syscall 再回调），io_uring 后端则是真 proactor。
- 反面教材：libuv 因其就绪式 API 与 io_uring 模型不匹配，网络路径至今未能用上 io_uring（libuv #4044）。

**教训明确：抽象层必须定在完成式语义**（提交带 buffer 的 `async_read` / `async_write` / `async_accept`，完成时携带结果），epoll/kqueue 端降级模拟。macOS 只是开发机，kqueue 后端允许慢。反向（就绪式抽象）会永久锁死 io_uring 的收益。

### 1.4 推荐

> **Shared-nothing 每线程独立事件循环 + SO_REUSEPORT 内核分发 + per-thread 后端连接池（每线程每后端 1~2 条 pipelined 连接）。IO 抽象定为完成式（proactor）接口：Linux 上 io_uring（multishot accept/recv + provided buffer ring）为首选后端，epoll 为强制 fallback；macOS 上 kqueue 模拟仅供开发。初期不做 SQPOLL（独占核不划算），zero-copy 留作后续优化。**

---

## 决策二：异步编程风格 —— C++20 协程 vs 回调/状态机

### 2.1 协程 vs 回调的真实权衡

- **性能**：每个协程帧默认堆分配。HALO（帧分配消除）只有 Clang 真正实现且不可靠（Clang 12→13 曾发生退化；Clang 20 的 `[[clang::coro_await_elidable]]` 属性容易被破坏）；**GCC 完全没有 HALO**。对策：不赌 HALO，用 `promise_type::operator new` 接内存池——proxy 的连接协程生命周期长、可复用，帧分配可摊薄到可忽略。
- **切换开销**：SC'25 实测无栈切换比有栈快 3.5x、帧更小；单次 co_await 恢复约为一次间接调用 + 分支的量级（symmetric transfer 保证深链无栈溢出）。对每次 await 都跨一次 io_uring 系统调用的 proxy 而言，这点开销远小于 IO 本身。
- **调试**：明显短板但已可用。Clang 官方 DebuggingCoroutines 文档 2025 年大改版，LLDB 内置 `coroutine_handle` pretty-printer、可打 async backtrace。比回调的"无因果链"调试仍强，比同步代码差。

### 2.2 库生态对比（2026 年中）

| 方案 | 现状 | 判定 |
|---|---|---|
| 自研薄 task/awaiter 层 | 约 500~800 行可行；坑集中在三处：临时量跨 co_await 的悬垂引用、final_awaiter 的 symmetric transfer 续接、异常经 promise 重抛（lewissbaker 系列文章可覆盖） | **推荐** |
| asio awaitable + io_uring 后端 | 1.78 起官方支持，但默认 io_uring 只管文件 IO；即使全量切 uring，也拿不到 multishot accept/recv、buffer ring 等 proxy 最想要的高级特性 | 不满足 |
| boost::cobalt | 已是正式 Boost 库，设计好，但本质是 asio 之上的糖，io_uring 能力受限同上，生产案例少 | 不满足 |
| libcoro | 活跃（0.15.0），但 io_scheduler 只有 epoll/kqueue，io_uring 仍是 open issue | 不合格 |
| stdexec / std::execution (P2300) | 已进 C++26，有 io_uring context，但官方仍标 experimental；**SG14 于 2026-02（P4041R0）明确建议网络库不要建在 P2300 上**（分配模式不适合低延迟网络） | 观望，不做地基 |
| Seastar | 接管 malloc、按核分配内存、要求 app_template 驱动整个进程——"被框架绑架"的典型 | 明确不引入 |

### 2.3 业界路线分歧：有栈 fiber vs 无栈协程

- **有栈阵营**：DragonflyDB/helio 用的是有栈 fiber 而非 C++20 协程（先用 Boost.Fiber 替换调度器接入 io_uring，2023 年起 fork 为自有 fb2）；PhotonLibOS 同路线。有栈的优势是兼容"深调用链里随处阻塞"的同步存量代码。
- **无栈阵营**：ScyllaDB/Seastar 已全面转 C++20 协程；CppCon 近年大量 "coroutines + io_uring" 实践。
- **对本项目的判断**：proxy 的协程链很浅（read → parse → route → forward → write），有栈路线的优势不存在，且要付每 fiber 栈内存的代价。新项目没有存量同步代码，无栈是正确选择。

### 2.4 Cancellation 与超时

- 语义模型照抄 asio 的 per-operation cancellation（cancellation_slot + `co_await (op || timeout)`），这是最成熟的方案。
- **自研 uring 层的独有红利**：io_uring 原生支持 `IORING_OP_ASYNC_CANCEL` 和 `IORING_OP_LINK_TIMEOUT`——后者直接把超时下沉到内核，比任何用户态 timer 方案都干净。

### 2.5 推荐

> **自研薄 C++20 无栈协程层（task/awaiter 约 500~800 行）+ 直接使用 liburing。** 理由：(1) multishot、buffer ring、link timeout 只有裸 uring 给得全，asio/cobalt/libcoro 都够不着；(2) 协程链浅，无栈模型完美匹配，可维护性碾压回调状态机且切换开销比 fiber 更低；(3) stdexec 被 SG14 判了网络场景缓刑，等 C++26 生态成熟再评估。配套决策：生产编译器选 Clang（HALO + 调试工具链最好），协程帧走 per-promise 内存池，cancellation 抄 asio 语义、超时用 LINK_TIMEOUT。

---

## 决策三：依赖策略

### 3.1 三档策略对比

| 档位 | 内容 | 判定 |
|---|---|---|
| (a) 极简自研 | 只有 liburing + 测试框架 | 自研 logger/metrics 会拖慢核心开发数月，且这些轮子无学习价值差异化。不推荐纯极简 |
| (b) 轻依赖精选 | 每个领域挑一个小而精、可 FetchContent 源码引入的库 | 构建复杂度低、可替换性强、长期维护成本最低。**推荐** |
| (c) 重框架 | Boost 全家桶 / folly / seastar | seastar 强制其 reactor 模型、folly 构建极其痛苦；编译时间和升级成本高。明确不推荐 |

同类项目参照：twemproxy/predixy 走零依赖（证明 proxy 核心可以极简，但也因此功能停滞、无 metrics 生态）；**DragonflyDB 走轻依赖精选**（helio + abseil + mimalloc + jsoncons），与本推荐档位一致；Envoy 的重依赖需要专职依赖治理流程支撑，不可复制。

### 3.2 各领域选型（2026 年现状）

| 领域 | 选择 | 理由 |
|---|---|---|
| I/O | liburing（Linux）；抽象层留 epoll/kqueue 后端 | 见决策一 |
| 格式化 | fmt | 仍比 std::format 快约 20~25%，头文件编译开销已优化到接近 stdio.h |
| 日志 | quill | 异步 logger，热路径 ~50ns vs spdlog ~6μs（量级差异可信），SPSC 无锁队列 + 后台线程格式化，正是数据面需要的 |
| 测试 | Catch2 v3 | doctest 作者公开寻找维护者、近乎停滞；其编译速度优势在 Catch2 v3 静态库模式下已基本消失；gtest 仅在需要 gmock 时选 |
| 微基准 | nanobench | 单头文件、运行快、输出 IPC/分支预测数据；CI 回归基准再考虑 google benchmark |
| 分配器 | mimalloc v3（CMake option 可关闭） | **jemalloc 已于 2025-06 上游归档（作者发布 postmortem）**；mimalloc 活跃、小对象低 P99，DragonflyDB 同选；proxy 是小对象高频分配场景，值得换 |
| CLI + 配置 | CLI11 + toml++ | CLI11 活跃且自带 TOML 配置读取；避免 yaml-cpp（YAML 复杂度不值得） |
| metrics | 自研 Prometheus 文本 /metrics（~200 行） | 文本格式极简单；prometheus-cpp 维护平淡还拖入 libcurl/zlib，负资产 |
| slot 哈希 | 自研 CRC16/XMODEM（~30 行） | **已确认** valkey cluster 用 CRC16/XMODEM（poly 0x1021，"123456789"→0x31C3）mod 16384，含 `{...}` hash tag 规则，官方 cluster-spec 附录 A 有参考实现 |
| 通用哈希 | xxhash | 单头文件 |
| RESP 解析 | **自研** | 项目的核心学习价值与性能命门所在 |

### 3.3 包管理与构建

- **包管理：CPM.cmake**（FetchContent 封装）。依赖全是纯 CMake 小库时零外部工具、版本锁在代码里；依赖超过 ~10 个或需要二进制缓存时再迁移 vcpkg/conan 2。
- **C++20 modules：不上。** 命名模块在 CMake 3.28+/Ninja 下可用，但 `import std` 仍是实验特性，clangd/CLion 支持残缺，第三方库几乎无 module 发行。继续头文件，代码按模块化风格组织，留后路。
- **编译器基线**：Clang 18+ / GCC 14+ 为 C++23 最低线；实际开发建议 Clang 19~20、GCC 15。macOS 开发机用 Homebrew LLVM 统一版本。
- **构建**：CMake 3.28+ + Ninja；CI 跑 Clang 20 + GCC 15 双编译器 + ASan/TSan/UBSan 构建。

---

## 整体结论

三个决策组合成的架构画像：

```
┌─ 进程（单进程多线程）─────────────────────────────────┐
│  worker thread × N（shared-nothing，SO_REUSEPORT 分发）│
│  ┌──────────────────────────────────────────────┐    │
│  │ 事件循环（完成式 proactor 抽象）                │    │
│  │   backend: io_uring │ epoll(必需fallback) │ kqueue(dev) │
│  │ C++20 无栈协程（自研 task 层，~500-800 行）      │    │
│  │   一条客户端连接 = 一个协程                      │    │
│  │   read → parse(RESP) → route → forward → write │    │
│  │ per-thread 后端连接池（每后端 1~2 条 pipelined） │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
依赖：liburing / fmt / quill / Catch2 / nanobench /
     mimalloc / CLI11 / toml++ / xxhash（CPM.cmake 管理）
语言：C++23（基线 Clang 18 / GCC 14），不上 modules
```

三个决策互相咬合的地方：

1. **无栈协程 ⇄ 裸 liburing**：只有自研协程层才能吃到 multishot / buffer ring / LINK_TIMEOUT；反过来，只有完成式的 IO 抽象才能让协程 awaiter 写得干净。
2. **shared-nothing ⇄ 协程内存池**：协程帧的 per-promise 内存池做成 thread-local 即可无锁，与线程模型天然契合。
3. **轻依赖 ⇄ 自研核心**：RESP 解析器、协程层、metrics 输出这三个"自研件"正是项目的学习价值与性能命门；其余领域用精选小库避免重复造无差异化的轮子。

## 主要参考资料

- Envoy threading model — https://www.envoyproxy.io/docs/envoy/latest/intro/arch_overview/intro/threading_model
- Cloudflare Pingora — https://blog.cloudflare.com/how-we-built-pingora-the-proxy-that-connects-cloudflare-to-the-internet/
- io_uring vs epoll 讨论 — https://github.com/axboe/liburing/issues/536 ；arXiv 2512.04859
- io_uring multishot — https://lwn.net/Articles/899498/ ；io_uring_disabled — https://lwn.net/Articles/937013/
- libuv 的 io_uring 困境 — https://github.com/libuv/libuv/issues/4044
- Dragonfly shared-nothing — https://github.com/dragonflydb/dragonfly/blob/main/docs/df-share-nothing.md ；helio — https://github.com/romange/helio
- Valkey "Unlock 1M RPS" — https://valkey.io/blog/unlock-one-million-rps/
- Symmetric transfer — https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer
- async_simple 协程性能定量报告 — https://github.com/alibaba/async_simple/blob/main/docs/docs.en/QuantitativeAnalysisReportOfCoroutinePerformance.md
- Clang 协程调试 — https://clang.llvm.org/docs/DebuggingCoroutines.html
- SG14 对 P2300 网络的意见 — https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p4041r0.pdf
- asio per-operation cancellation — https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/core/cancellation.html
- jemalloc postmortem — https://jasone.github.io/2025/06/12/jemalloc-postmortem/
- quill vs spdlog — https://dev.to/odygrd/quill-vs-spdlog-which-c-logger-is-better-for-low-latency-applications-408
- valkey cluster spec（CRC16 + hash tag）— https://valkey.io/topics/cluster-spec/
- C++20 modules 现状 — https://wrocpp.github.io/posts/modules-2026/
- 编译器 C++23 支持 — https://clang.llvm.org/cxx_status.html ；https://gcc.gnu.org/projects/cxx-status.html
