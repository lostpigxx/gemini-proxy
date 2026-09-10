# 开发计划

> 日期：2026-08-21
> 前置文档：[架构决策调研报告](architecture-decisions.md)
>
> 总目标：实现一个高性能、高可维护的 valkey proxy。C++23，shared-nothing 线程模型，
> io_uring（epoll fallback）+ 自研 C++20 无栈协程层，轻依赖精选。
>
> 计划按里程碑（M0~M6）组织，每个里程碑结束时都有**能跑、能测、能演示**的产物。
> 里程碑内的任务按建议实施顺序排列；「验收标准」是该里程碑完成的客观判据。

---

## 总览

| 里程碑 | 主题 | 核心产物 | 状态 |
|---|---|---|---|
| M0 | 工程骨架 | 可构建、可测试、CI 全绿的空项目 | ✅ 完成（2026-08-24, `9b19060`） |
| M1 | RESP 协议解析器 | 经 fuzz 验证的零拷贝 RESP2/3 解析与序列化库 | ✅ 完成（2026-08-24） |
| M2 | 事件循环 + 最小可用 proxy | `valkey-cli` 可通过 proxy 访问单个后端 | ✅ 完成（2026-08-31） |
| M3 | 多线程 + 连接池 + pipelining | 可承受多连接压测的生产形态骨架 | ✅ 完成（2026-09-07） |
| M4 | cluster 路由 | 对接 valkey cluster，处理 MOVED/ASK | ✅ 完成（2026-09-10） |
| M5 | 可观测性与配置 | metrics / 日志 / 配置 / 优雅关闭 | |
| M6 | 性能打磨 | 基线固化与针对性优化 | |

依赖关系：M1 与 M2 可部分并行（M1 不依赖任何 IO 代码）；其余按序进行。

---

## M0 — 工程骨架 ✅

**状态**：已完成（2026-08-24，`9b19060`），CI 8 个 job 全绿。

**目标**：定下项目的质量下限。之后所有代码都在这套约束下开发。

任务清单：

1. **目录结构**：
   ```
   src/           # 实现（按模块分子目录：resp/ io/ core/ cluster/ ...）
   tests/         # 单元/集成测试（镜像 src 结构）
   benchmarks/    # nanobench 微基准
   fuzz/          # libFuzzer 目标
   docs/          # 文档
   cmake/         # CPM.cmake、工具链、辅助模块
   scripts/       # 开发脚本（本地 cluster 启停、压测等）
   ```
2. **构建系统**：CMake 3.28+ + Ninja，C++23（`cxx_std_23`），CPM.cmake 引入依赖。
   `CMakePresets.json` 提供 `debug` / `release` / `asan-ubsan` / `tsan` 四个预设。
3. **初始依赖接入**（一次到位，全部 CPM 锁版本）：fmt、quill、Catch2 v3、nanobench、CLI11、toml++、xxhash；liburing 仅 Linux 引入；mimalloc 以 `PROXY_USE_MIMALLOC` 选项接入（默认 Release 开）。
4. **代码规范工具**：`.clang-format`、`.clang-tidy`（含 bugprone/performance/modernize 检查集）、`.editorconfig`；`scripts/format.sh` 一键格式化。
5. **CI（GitHub Actions）**：
   - Linux：Clang 20 与 GCC 15 双矩阵，debug+release 构建 + 全部测试 + asan-ubsan 跑测试。
   - macOS：构建 + 测试（验证 kqueue 路径可编译）。
   - clang-format 检查作为独立 job。
6. **打通验证**：一个最小 `proxyd` 可执行文件（打印版本退出）+ 一个最小 Catch2 测试 + 一个最小 nanobench 基准，证明全链路可用。

**验收标准**：CI 全绿；macOS 本机与 Linux（容器）均可一条命令构建并跑测试。

**实施记录（与计划的偏差）**：

- CMake 选项统一用 `VKP_` 前缀（非计划中的 `PROXY_`）：`VKP_USE_MIMALLOC` / `VKP_SANITIZE` / `VKP_WERROR` / `VKP_BUILD_TESTS` / `VKP_BUILD_BENCHMARKS`。mimalloc 通过 `release` preset 开启。
- CI 的 GCC 用 14 而非计划写的 15（ubuntu-24.04 上 gcc-14 可直接 apt 安装，gcc-15 需额外 PPA；基线本就是 GCC 14+）。
- 警告选项挂在 `vkp_options` INTERFACE target 上，只作用于自有代码，第三方依赖不受 `-Werror` 影响。
- xxHash 以 header-only（`XXH_INLINE_ALL`）方式消费，绕过其滞后的构建系统。
- clang-format 版本 pin 到 20.1.8，本地与 CI 均通过 PyPI 包安装；`scripts/format.sh` 会校验二进制存在性与主版本号（早期版本会在工具缺失时静默通过，已修）。

---

## M1 — RESP 协议解析器 ✅

**状态**：已完成（2026-08-24）。

**目标**：全系统的性能核心与第一个真正模块。纯函数式（bytes in → 消息 out），不含任何 IO，可 100% 测试。

任务清单：

1. **buffer 管理设计**（先行设计，出一页设计说明放入 docs/design/）：
   连接读缓冲的所有权模型、增量数据的追加与消费、解析结果如何以 `std::span`/视图形式指回缓冲（零拷贝）、缓冲扩容与搬移时机。这一步定错后面全错，值得慢。
2. **RESP 增量解析器**：
   - RESP2 全类型（simple string / error / integer / bulk / array）+ RESP3 全类型（double / boolean / big number / map / set / push / verbatim / null / attribute）。
   - 流式：输入可在任意字节处截断，返回「需要更多数据」；恢复解析不重扫已扫过的字节。
   - 防御：嵌套深度上限、单元素与总消息大小上限（可配置），恶意输入不 OOM。
   - 面向 proxy 的关键特性：**解析层只需要「取出第一个 bulk（命令名）+ key 的位置 + 整条消息的边界」**，不必完整物化整棵消息树——转发时原样透传字节。API 设计要体现这一点（浅解析与深解析分开）。
3. **序列化器**：错误回复、内联小回复（`+OK\r\n` 等）的生成；proxy 自身响应用。
4. **inline command 支持**：可选，低优先级（valkey-cli 不用它，telnet 调试才用），先留 TODO。
5. **测试**：
   - Catch2 单测覆盖全部类型 × 截断位置扫描（每条用例在每个字节处切一刀）。
   - libFuzzer 目标（`fuzz/resp_parser_fuzz.cpp`），CI 中短跑（如 60s），本地可长跑。
   - nanobench 基准：典型 GET/SET/MGET 报文的解析吞吐，建立第一条性能基线并记录在 docs。

**验收标准**：单测全绿且覆盖全部 RESP3 类型；fuzz 本地跑 1 小时无 crash/泄漏；基准数字记录在案。

**实施记录（与计划的偏差）**：

- 设计文档：[docs/design/resp-buffer-and-parser.md](design/resp-buffer-and-parser.md)。核心不变量：
  可读窗口始终从当前消息第 0 字节开始（只在消息边界 consume），解析器内部只存相对偏移，
  缓冲扩容/压缩搬移不影响解析中间状态。
- 浅解析器（`resp::parser`）按计划只定边界不物化树；命令形态（顶层 bulk 数组）时顺带暴露
  全部参数视图（`args[0]`=命令名）。深解析器（`resp::parse_tree`）为控制面服务，非增量
  （控制面消息先攒完整再解析）。
- **深解析器 API 用自研 30 行 `tree_result` 而非 `std::expected`**：Ubuntu 24.04 的
  clang-18 定义 `__cpp_concepts=201907L`，libstdc++ 将 `<expected>` guard 掉——
  `std::expected` 实际要求 Clang 19+，与项目 Clang 18 基线冲突。
- RESP3 streamed/chunked 类型显式报 `streamed_not_supported`（valkey 实际不发）；
  inline command 按计划留 TODO（报 `unknown_type_byte`）。
- attribute（`|`）与被注解值算**同一条**消息边界（FIFO 响应配对正确性所必需），
  浅/深解析器同语义。
- fuzz：Apple Clang 无 libFuzzer 运行时，`scripts/fuzz.sh` 在 macOS 上经 Docker 跑
  Linux 原生 fuzz。harness 交叉验证「一次性浅解析 / 任意分块增量浅解析 / 完整帧深解析」
  三方一致性，分歧即 abort。**上线 90 秒即抓到真 bug**：浅解析器按解析值接受 `*-01`
  为 null 数组、深解析器按文本比较拒绝——两解析器对同一帧分歧（已修 + 回归测试 +
  回归语料）。CI 增加 60 秒 fuzz job（clang-20 + ASan/UBSan）。
- 测试：26 用例 / 2457 断言，全部 RESP2/3 类型 × 每字节截断 × 单 parser 增量重入；
  另有 read_buffer+parser 任意分块端到端。GCC-14（容器）与 ASan/UBSan 均绿。
- **fuzz 长跑（验收项）**：Linux 容器（clang-18 + ASan/UBSan），修复两个分歧后连续
  3601 秒 / **126,985,113 次执行（~35k exec/s）无 crash 无泄漏**；语料经 `-merge=1`
  精简为 477 个文件入库（含 3 个具名回归输入）。
- **性能基线（验收项）**：macOS arm64（Apple Clang 21，release + mimalloc），
  `vkp_resp_bench`，err% ≤ 2.7%：

  | 报文 | 帧大小 | 吞吐 | 单帧耗时 |
  |---|---|---|---|
  | GET | 39 B | 1.44 GB/s | ~27 ns |
  | SET（64 B value） | 110 B | 3.10 GB/s | ~36 ns |
  | SET（1 KiB value） | 1072 B | 28.6 GB/s | ~37 ns |
  | MGET 10 keys | 275 B | 2.30 GB/s | ~121 ns |
  | MGET 100 keys | 2616 B | 2.06 GB/s | ~1.28 µs |
  | 1 KiB bulk 响应 | 1033 B | 90.3 GB/s | ~11 ns |
  | 深解析 MGET 100（对照） | 2616 B | 2.25 GB/s | — |

  解读：大 payload 场景吞吐由「跳过 payload」主导（接近 memcpy 量级）；小帧场景
  ~27ns/帧 ≈ 每秒 3600 万条 GET，解析不会成为 M2/M3 的瓶颈。M6 优化的对照起点。

---

## M2 — 事件循环 + 最小可用 proxy ✅

**状态**：已完成（2026-08-31）。设计文档：[docs/design/io-and-coroutines.md](design/io-and-coroutines.md)。

**目标**：立起 IO 抽象与协程框架，做出第一个可演示的 proxy（单后端、单线程、整条透传）。

任务清单：

1. **完成式 IO 抽象接口**（proactor 语义，见架构决策 1.3）：
   `async_accept / async_recv / async_send / async_connect / sleep_for`，操作与 buffer 一起提交、完成时携带结果。接口先行评审再实现。
2. **io_uring 后端**：单 ring 单线程；multishot accept + multishot recv 起步；`IORING_OP_LINK_TIMEOUT` 支持超时。provided buffer ring 留到 M6。
3. **epoll 后端**：就绪式模拟完成式（就绪→执行非阻塞 syscall→投递完成）。**一等公民**：运行时探测 io_uring 可用性（受 seccomp/RHEL 限制环境自动降级）+ 配置强制开关。
4. **kqueue 后端**：与 epoll 同构，仅保证 macOS 开发可跑，不做性能要求。
5. **自研协程层**（`task<T>` + awaiter，目标 500~800 行）：
   - promise/handle 生命周期、symmetric transfer 续接、异常经 promise 重抛。
   - cancellation：照抄 asio per-operation 语义（cancellation state + `co_await race(op, timeout)`），本里程碑先出骨架。
   - 协程帧 `operator new` 接 thread-local 内存池（先用简单 freelist，M6 再调优）。
   - 单独的单测（无 IO 的调度/异常/取消用例）。
6. **最小 proxy**：单线程事件循环；每客户端连接一个协程：读 → RESP 浅解析出消息边界 → 原样转发到唯一后端 → 读后端响应 → 回写。命令行参数指定监听端口与后端地址。
7. **优雅关闭**：SIGTERM 后停止 accept、等待在途请求、超时强关。

**验收标准**：`valkey-cli -p <proxy> PING/SET/GET/MGET` 全部正确；`valkey-benchmark -c 1` 可完整跑完；ASan/TSan 下压测无报告；macOS 上 kqueue 路径同样通过功能测试。

**实施记录（与计划的偏差）**：

- 分层落地为 `task<T>`（`io/task.hpp`）→ `operation` 完成对象（嵌于协程帧，地址稳定，
  backend 裸存指针、io_uring 直接塞 `user_data`，零额外分配）→ `event_loop`（ready
  队列 + 定时器最小堆，backend 无关）→ `backend` 三实现。完成结果统一 io_uring CQE
  约定（`>=0` 载荷 / `<0` 为 `-errno`），reactor 端向它看齐。
- **multishot 全部推迟到 M6**（计划偏差）：multishot recv 依赖 provided buffer ring
  （`IORING_RECV_MULTISHOT` 必须 `IOSQE_BUFFER_SELECT`），计划里「multishot recv 在
  M2、buffer ring 在 M6」不可拆；multishot accept 一并到 M6 评估。M2 io_uring 为
  单发 op + poll 内 `submit_and_wait` 批量提交。
- **per-op 超时（LINK_TIMEOUT）挪到 M3**：与 M3「超时体系」合并实施；M2 完成
  `sleep_for`（用户态定时器堆）与 `cancel_slot`（asio per-op 取消骨架）。
- 运行时探测按计划：`make_backend()` 构造 io_uring 失败自动降级 epoll，Docker 默认
  seccomp 下实测降级生效；`--io-backend` 可强制。
- 优雅关闭：SIGTERM/SIGINT → socketpair self-pipe（**不用 pipe：io_uring 的 RECV
  仅支持 socket**）→ 取消 accept → drain → 5s grace 强关；再来一次信号立即停。
  实测：drain 期间新连接被拒、5.1s 退出、exit code 0。
- **教训：accept 出的客户端 socket 必须设 TCP_NODELAY**——只给后端侧连接设了会在
  pipeline 客户端下触发 Nagle+delayed-ACK 停顿，容器实测 `-P 8` 只有 ~190 rps，
  修后 ~24k rps（126×）。
- 已知编译器差异：GCC 仅在 `-foptimize-sibling-calls`（-O2+）下才把 symmetric
  transfer 编译成真尾调用（PR 100897），-O0 深链会真实爆栈；深链测试按编译器降深度。
  Clang 全优化级别保证尾调用（生产编译器选 Clang 的又一依据）。
- epoll 后端用 `epoll_pwait2`（内核 5.11+/glibc 2.35+，在 Ubuntu 24.04 基线内）。
- 验收实测：macOS(kqueue，本机 redis-server) 与 Linux 容器(epoll + io_uring，
  clang ASan/UBSan 构建) `redis-cli` PING/SET/GET/MGET/INCR/TYPE/RESP3 HELLO 全部
  正确；`redis-benchmark -c 1 -n 5000`（约 15~18k rps @ ASan 构建）与 `-c 1 -P 8`
  完整跑完，无 sanitizer 报告；TSan 全测试 + 实流量无报告；GCC-14 Debug/Release
  容器全绿。测试规模：40 用例 / 2506 断言（IO 层按 `available_backends()` 参数化，
  Linux 上同套测试跑 epoll 与 io_uring 两遍）。

---

## M3 — 多线程 + 连接池 + pipelining ✅

**状态**：已完成（2026-09-07）。设计文档：[docs/design/m3-workers-pool-pipelining.md](design/m3-workers-pool-pipelining.md)。

**目标**：从玩具变成生产形态骨架：多 worker、后端连接复用、流水线保序。

任务清单：

1. **多 worker**：thread-per-core，每 worker 独立事件循环 + SO_REUSEPORT 独立 listen socket；worker 数可配置（默认 = 物理核数）。CPU 亲和性选项。
2. **per-thread 后端连接池**：每 worker 每后端固定 1~2 条长连接；启动预热、断线指数退避重连、连接健康检查（周期 PING）。
3. **请求-响应配对与 pipelining**：
   - 多个客户端的请求在同一条后端连接上 pipeline，按 FIFO 配对响应（redis 协议保序）。
   - 每后端连接一个 in-flight 队列；后端连接断开时，队列中未决请求统一回错误。
   - 客户端侧同样支持 pipelining（读到多条完整请求就并发转发，按序回写）。
4. **超时体系**：连接超时、请求超时（io_uring 用 LINK_TIMEOUT，epoll 用 timer），超时回 `-ERR proxy timeout` 并标记后端连接需重建。
5. **命令表与拒绝列表**：建立命令元数据表（命令名 → key 位置、是否可转发）。`MULTI/EXEC/WATCH/SUBSCRIBE/PSUBSCRIBE/BLPOP` 等有连接粘性/阻塞语义的命令先明确返回 `-ERR unsupported by proxy`，但表结构为 M4+ 的路由和未来支持留好字段。
6. **背压**：客户端读入速率与后端写出能力挂钩（in-flight 上限，超过暂停读客户端）。

**验收标准**：`valkey-benchmark -c 50 -P 16` 与 memtier 混合读写压测正确通过；对比直连记录延迟损耗（P50/P99）并写入 docs 作为基线；TSan 压测无数据竞争报告。**三项全部达成**，实测见下。

**实施记录（与计划的偏差）**：

- 分层落地：`worker_pool`（线程 + 亲和性 + 关停扇出）→ 每 worker 一个 `event_loop`
  + 一个 `proxy::server`（listen/accept/客户端连接）→ 每 worker N 条
  `backend_conn`（后端长连接 + in-flight FIFO + 三个常驻协程）。worker 之间零共享，
  没有一处跨线程可变状态。
- **超时不用 `IORING_OP_LINK_TIMEOUT`（计划偏差，见设计文档 §3）**：LINK_TIMEOUT 只有
  io_uring 有，reactor 后端仍得写第二套超时路径。改为每条后端连接一个 watchdog 协程，
  睡到「队首请求的 deadline」而不是每请求挂一个定时器——热路径上每请求零额外 op、零
  定时器堆插入，三个后端共用一条代码路径。连接超时、请求超时、空闲健康检查 PING 都由
  这一个协程调度。
- **`worker_pool` 构造期就建好全部 loop/server**，`run()` 只负责起线程。这样
  `--listen :0`（临时端口）能工作：worker 0 先 bind 拿到端口号再共享给其余 worker，
  否则 SO_REUSEPORT 下每个 worker 会各绑一个不同端口。
- **默认 worker 数是 1，不是物理核数（计划偏差）**：M3 还没有多后端/路由，默认多线程
  只会在单条 valkey 上放大竞争，收益不明。`-w 0` 显式取 `hardware_concurrency()`。
  M4 有了 cluster 路由后再考虑改默认值。
- **HELLO/QUIT/SELECT/RESET 改为本地应答（`cmd_policy::local`，改变了 M2 的整条透传
  行为）**：后端连接是所有客户端共用的，任何改变连接状态的命令都不能透传。HELLO 由
  proxy 自己回握手（RESP2 `*14` / RESP3 `%7`，`server` 字段为 `valkey-proxy`），
  `HELLO AUTH` 回 `-ERR proxy: AUTH not supported`，SELECT 只接受 db 0。
- **本地应答与拒绝应答进同一条 FIFO**（`entry.local`）而不是直接写回客户端，否则
  pipeline 里 `GET / SUBSCRIBE / GET` 的错误会插到两个 GET 响应之前。
- **客户端中途死掉用打墓碑（`sink = nullptr`）而不是从队列里摘除**——摘除会打乱后端
  连接上的 FIFO 对齐，后续所有客户端都会收到错位的响应。
- **cancel-before-close 是硬性纪律**：绝不 `close()` 一个还有在途 op 的 fd（reactor
  后端的 slot 会变悬空指针）。后端连接重建时先 cancel → 等 writer 协程从被取消的 send
  里返回并 notify → 才 `fd_.reset()`。
- **`wait_queue`（异步条件变量）必须在 event_loop 上注册**：否则「循环空转 + 只剩挂起
  的 waiter」时，既没人能 notify 它们，`run()` 又不肯退出，结果是协程帧泄漏 + 挂死。
  现在 `stop()` 与「idle 且无任何可完成事件」两处都会把挂起 waiter 统一以
  `-ECANCELED` 唤醒。
- **教训：交给 event_loop 的每一个 fd 都必须是非阻塞的**——worker_pool 测试里
  socketpair 的读端忘了设 `O_NONBLOCK`，kqueue 后端直接阻塞在 `recvfrom` 里，
  表现为 ctest 整体超时挂死（用 macOS `sample` 抓栈才定位到）。
- 测试规模：61 用例 / 2664 断言。新增 `wait_queue`、`byte_queue`、命令表、
  worker_pool 单测，`proxy_test` 重写为可编排的假后端（echo/silent/first_then_close），
  覆盖双客户端交错 pipeline 保序、拒绝应答插入位置、HELLO/SELECT/RESET/QUIT 语义、
  后端不可达、请求超时、pipeline 中途后端猝死、自动重连、`max_inflight=1` 深 pipeline、
  健康检查 PING。

**验收实测**（Linux 容器 aarch64 / OrbStack 内核 7.0 / 18 vCPU，clang-18；后端为
同机 redis-server 7.0.15，`--save "" --appendonly no`）：

- 构建矩阵全绿：clang-18 Debug、GCC-14 Debug、ASan+UBSan、TSan，各 61/61 通过；
  io_uring 与 epoll 两条路径都跑到（IO 层测试按 `available_backends()` 参数化）。
- **TSan 压测无任何报告**（验收项）：4 worker × 2 条后端连接，
  `redis-benchmark -c 50 -P 16 -n 500000` + 11 种命令的非 pipeline 混合压测，
  合计约 210 万次操作，零 ThreadSanitizer 报告，SIGTERM 正常收尾。
- **混沌压测**：压测运行中连杀后端 3 次，在途请求按预期收到
  `-ERR proxy: backend connection lost`，指数退避重连后 PING/SET/GET 全部恢复；
  TSan 同样零报告。
- ASan+UBSan（含 `detect_leaks=1`）实流量压测 + 本地/拒绝命令路径：零报告、零泄漏。

**性能基线（验收项）A：redis-benchmark**。`redis-benchmark -c 50 -d 32 --threads 4`，
`-P 1` 取 n=100 万、`-P 16` 取 n=500 万（跑短了 rps 会被计时精度量化，早先 n=30 万的
一轮四个配置全都读出「1.2M rps」就是这个原因）。延迟单位毫秒。

| 配置 | 命令 | P | rps | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| 直连 | SET | 1 | 399,840 | 0.103 | 0.151 | 0.191 |
| 直连 | GET | 1 | 363,504 | 0.103 | 0.215 | 0.431 |
| 直连 | SET | 16 | 2,855,511 | 0.207 | 0.303 | 0.479 |
| 直连 | GET | 16 | 3,331,112 | 0.191 | 0.303 | 0.423 |
| proxy io_uring w4 c1 | SET | 1 | 307,409 | 0.135 | 0.239 | 0.303 |
| proxy io_uring w4 c1 | GET | 1 | 307,503 | 0.135 | 0.239 | 0.303 |
| proxy io_uring w4 c1 | SET | 16 | 2,498,751 | 0.263 | 0.479 | 0.591 |
| proxy io_uring w4 c1 | GET | 16 | 2,853,881 | 0.239 | 0.439 | 0.543 |
| proxy io_uring w4 c2 | SET | 16 | 2,497,502 | 0.279 | 0.511 | 0.631 |
| proxy io_uring w4 c2 | GET | 16 | 2,498,751 | 0.263 | 0.471 | 0.591 |
| proxy epoll w4 c1 | SET | 16 | 1,998,401 | 0.327 | 0.591 | 0.719 |
| proxy epoll w4 c1 | GET | 16 | 2,220,248 | 0.295 | 0.535 | 0.655 |
| proxy io_uring w1 c1 | SET | 16 | 2,219,263 | 0.311 | 0.375 | 0.447 |
| proxy io_uring w1 c1 | GET | 16 | 1,997,603 | 0.335 | 0.463 | 0.487 |

  解读（以 io_uring / 4 worker / 1 条后端连接为准）：
  - **`-P 16`（验收配置）**：p50 +0.05 ms、p99 +0.11 ms，吞吐为直连的 86~88%。
    相对增幅 p50 +25%、p99 +23%，落在「P99 相对直连增加 ≤ 30%」的区间里。
  - **`-P 1`**：p50 +0.032 ms（一次额外的 loopback 往返 + 一次解析），吞吐为直连的
    77~85%。绝对增量 ~32 µs 是这套架构在无 pipeline 下的固定代价，M6 优化的对照起点。
  - **epoll 比 io_uring 慢 12~22%**（同为 4 worker，`-P 16`），符合预期：reactor
    路径每次 IO 多一次 `epoll_pwait2` 往返 + 一次非阻塞 syscall。
  - **`conns_per_backend=2` 没有收益**（`-P 16` 下反而 p50 +0.02 ms）：后端是单线程
    redis，多开一条连接只是把同样的请求拆成两条队列，还多一份 syscall。默认保持 1，
    该选项留给 M4 多后端/慢命令隔离场景。
  - w1 与 w4 在 `-P 16` 下吞吐接近（2.0~2.2M vs 2.5~2.9M），瓶颈已在单线程后端，
    不是 proxy。

**性能基线（验收项）B：memtier 混合读写**。memtier_benchmark 不在 Ubuntu 源里，
从源码构建（`5694a3d`，libevent 2.1.12）。
`--ratio=1:1 -c 10 -t 5 --data-size=32 --key-pattern=R:R --key-maximum=100000
--test-time=20`（50 条连接），Totals 行，延迟单位毫秒：

| 配置 | pipeline | ops/s | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| 直连 | 1 | 327,908 | 0.135 | 0.343 | 0.855 |
| 直连 | 16 | 1,781,117 | 0.423 | 1.063 | 1.215 |
| proxy io_uring w4 | 1 | 338,622 | 0.143 | 0.311 | 0.399 |
| proxy io_uring w4 | 16 | **2,162,791** | **0.351** | **0.799** | 1.039 |
| proxy epoll w4 | 1 | 291,436 | 0.167 | 0.343 | 0.455 |
| proxy epoll w4 | 16 | 1,774,113 | 0.431 | 0.943 | 1.199 |

  **`pipeline=16` 下走 proxy 反而比直连快（吞吐 +21%、p50 −17%、p99 −25%）**，
  与上面 redis-benchmark 的结论相反。复跑确认（proxy 2.22M / p50 0.343 / p99 0.783
  vs 紧接着复跑的直连 1.87M / 0.383 / 1.047），memtier 无 error 计数，GET 命中率
  正常，不是测量假象。

  原因是两个压测工具对后端的压法不同，而不是 proxy 时快时慢：

  - memtier 用 50 条连接、每条各自 pipeline。单线程 redis-server 每轮事件循环要为
    50 条连接各做一次 read/write syscall。经 proxy 后这 50 条被复用成 4 条后端连接
    （每 worker 1 条），且 proxy 会把同一轮里多个客户端的请求**合并成一次 send**
    （`byte_queue` 双缓冲），后端每轮只需 4 次更大的 syscall——连接复用 + 写合并
    省下的 syscall 超过了多一跳的代价。这正是 proxy 该有的收益。
  - redis-benchmark 是更精简的加载器，每条连接的 pipeline 窗口始终填满，直连时后端
    收到的本来就是大批次，复用与合并没有额外可省的，于是只剩下多一跳的净开销。

  结论：**「相对直连的损耗」强依赖客户端自身的批处理效率**。两组数字都保留：
  redis-benchmark 那组是 proxy 开销的**上界**（客户端已最优批处理），memtier 这组
  更接近真实应用（大量连接、各自浅 pipeline）。M6 的优化目标以 redis-benchmark
  那组为准，因为它才隔离出了 proxy 自身的开销。

---

## M4 — cluster 路由 ✅

**状态**：已完成（2026-09-10）。设计文档：[docs/design/m4-cluster-routing.md](design/m4-cluster-routing.md)。

**目标**：对接 valkey cluster，proxy 屏蔽拓扑细节。

任务清单：

1. **slot 计算**：CRC16/XMODEM（cluster-spec 附录 A 参考实现，自研 ~30 行）+ `{...}` hash tag 规则；单测用 spec 中的测试向量。
2. **拓扑管理**：启动时 `CLUSTER SHARDS` 拉取 slot→节点映射；周期刷新 + 事件触发刷新（收到 MOVED 时）。拓扑对象不可变 + 原子替换（每 worker 持有本地副本，避免锁）。
3. **路由**：按第一个 key 的 slot 选择后端；无 key 命令（PING 等）本地应答或随机节点；multi-key 命令先要求同 slot（跨 slot 返回 `-CROSSSLOT` 语义错误），MGET/MSET 拆分聚合留作 backlog。
4. **重定向处理**：MOVED → 更新拓扑并向新节点重试（有限次数）；ASK → 向目标节点发 `ASKING` + 原命令，不更新拓扑。
5. **节点生命周期**：新节点出现时按需建池；节点从拓扑消失时 drain 并关闭其连接池。
6. **集成测试环境**：`scripts/cluster-up.sh` 用 docker compose 拉起 3 主 3 从本地集群；集成测试脚本覆盖正常读写 + 手动 resharding 场景。

**验收标准**：压测运行中执行 slot 迁移（resharding），客户端无错误（或仅有可解释的瞬时重试延迟）；MOVED/ASK 路径有集成测试覆盖。**两项均达成**，实测见下。

**实施记录（与计划的偏差）**：

- **最大的一处改动计划里没有：客户端有序回复槽位**。M3 的保序依赖「一个客户端固定
  绑定一条后端连接、按该连接的 FIFO 天然有序」，而 cluster 下相邻两条请求会落到不同
  节点，两个节点的响应之间没有任何顺序关系。所以 M4 的第一步不是 slot 计算，而是**把
  定序权从后端连接搬到客户端**：每条请求分配单调递增 token + `deque` 槽位，只刷出
  连续已填充的前缀。其余全部建立在这之上（设计文档 §1）。
  - 连带删除了 M3 的 `entry.local` / `local_reply` / `drain_local_heads` 整套机制：
    客户端自己有槽位定序，本地回复直接填自己的槽即可。`backend_conn` 退化成纯传输层。
  - 落地后复跑 M3 基线 A 确认无回归（`1d88b53`，门槛数据记在设计文档附录）：队头在
    单后端下恒被立即填充，走的还是「直接 append、不落中间 string」那条路径。
- **standalone 建模成「单节点拥有全部 16384 slot 且从不刷新」的退化 cluster**，只有
  一条路由代码路径，不写两套。`may_redirect()` 在 standalone 为 false，据此跳过「保留
  请求字节以备重试」的那次拷贝，M3 的性能特征不受影响。
- **拓扑每 worker 各自持有、各自刷新，零共享（明确不引入 `atomic<shared_ptr>` 换指针）**：
  计划原文的「不可变 + 原子替换」会打破贯穿 M0~M3 的「worker 之间零共享可变状态」不变量，
  为一个每几秒一次的控制面操作破例不划算，热路径还要每请求 load 一次共享指针。代价是
  控制面流量 ×worker 数，可接受——刷新复用已有数据连接（把 `CLUSTER SHARDS` 当普通请求
  pipeline 进去），不额外建控制连接，因此自动继承了 M3 的超时/重连/背压体系。
- **集成测试环境改为单容器内 6 个 server 进程，不用 docker compose（计划偏差）**：
  本机直连 docker.io 极慢，compose 要额外拉/编排 6 个容器并处理 cluster announce-ip；
  proxy、压测、resharding 本来就都在同一个验证容器内跑。见 `scripts/cluster-up.sh`
  与设计文档 §8。
- **无 key 命令分三层，不是计划写的「本地应答或随机节点」**：PING/ECHO 本地应答；
  INFO/CONFIG/COMMAND/TIME 轮询任选 master；**SCAN/KEYS/DBSIZE/RANDOMKEY/FLUSHALL/
  FLUSHDB 与查表未命中的未知命令在 cluster 模式明确拒绝**。宁可报错也不猜——猜错会把
  请求发到错误节点，返回语义上错误但看起来正常的结果，这是最难排查的一类故障。
  key 位置需要解析变参才能确定的 SORT/SORT_RO、GEORADIUS 系列、XREAD/XREADGROUP 同样
  拒绝，进 backlog。
- **`CLUSTER` 命令一律拒绝，proxy 对外始终是 standalone**（与 M3 的 HELLO
  `mode=standalone` 一致）。透传真实拓扑会让 cluster-aware 客户端直连后端、完全绕过
  proxy；伪造 CLUSTER SLOTS 指向 proxy 自己有真实价值但超出 M4 范围，进 backlog。
  拓扑观测留给 M5 的 `/topology`。
- **只支持 `CLUSTER SHARDS`，不做 `CLUSTER SLOTS` 回退**：验证环境的 redis 7.0.15
  支持它，少一条代码路径。只取 `role=master && health=online`，读副本进 backlog。
- **重定向重试允许突破 `max_inflight`（设计文档 §5.1）**：重定向发生在 `deliver()` 里，
  而它跑在*旧*连接的 driver 回调中不能挂起，目标连接没配额时无法像读路径那样 park。
  回软错误就等于「resharding 期间客户端会看到错误」，而这正是本里程碑验收标准要排除的，
  所以选超发；超发量上界是当下在飞的请求数，有界。
- **空 host 的 MOVED（`-MOVED 1 :6380`）原样透传**：`deliver()` 只拿到 token 和帧内容，
  不知道是哪个节点回的（§1 刻意保持的解耦），无从补全，判为「不是有效重定向」交给客户端
  比猜一个地址诚实。
- **cluster 模式下 PING 不再打到后端**（本地应答的后果，设计文档 §7）：
  `redis-benchmark -t ping` 打 cluster 模式的 proxy 只测到 proxy 自身。standalone 保持
  M3 行为，所以 M3 的基线仍然可比。记在这里以免日后误读基线数字。
- **验收工具是自己写的 `scripts/cluster-loadcheck.py`，不是 redis-benchmark**：
  redis-benchmark 7.0.15 根本没有任何错误相关选项，它直接丢弃错误回复——全程回 `-MOVED`
  的一轮照样打印漂亮的 rps，测不了「客户端零错误」这条验收标准。自带的校验客户端两阶段
  交替（先 pipeline SET 收全回复，再 pipeline GET 校验取回原值），任何
  `-ERR/-MOVED/-ASK/-CROSSSLOT` 抵达客户端都算失败。反向验证过它有效：直连裸节点跑，
  如实报出 98 万条 MOVED。
  - 第二阶段等第一阶段回复收全才发，是有意的：proxy 保证的是每客户端的**回复**顺序，
    不是跨节点的执行顺序，同 key 的 GET 完全可能在 SET 还在重定向路上时越过它。
- 测试规模：100 用例 / 8056 断言（M3 为 61 / 2664）。新增 `cluster/slot`、
  `cluster/topology`、`proxy/router` 单测与 `proxy/cluster_routing_test`（可编排的假
  cluster 节点，能返回 CLUSTER SHARDS 与 MOVED/ASK），后者覆盖按 slot 选中正确节点、
  CROSSSLOT、MOVED 重试且拓扑已更新、ASK 发 ASKING+原命令且拓扑不变、重定向死循环封顶、
  standalone 原样透传重定向、**慢节点先回时客户端出向顺序仍与请求顺序一致**、
  刷新协程感知节点离开拓扑、cluster 模式拒绝集。

**验收实测**（Linux 容器 aarch64 / OrbStack 内核 7.0 / 18 vCPU，clang-18；后端为同机
`scripts/cluster-up.sh` 起的 3 主 3 从 redis 7.0.15，127.0.0.1:7000-7005）：

- 构建矩阵全绿：clang-18 Debug、GCC-14 Debug、ASan+UBSan（`detect_leaks=1`）、TSan、
  clang-18 Release，各 100/100（8056 断言）；io_uring 与 epoll 两条路径都跑到
  （IO 层测试按 `available_backends()` 参数化）。
- fuzz 短跑 60s / 268 万次执行，无 crash/leak/oom/timeout；顺带把语料 `-merge=1`
  精简 896 → 470，覆盖率不变（495 edges / 2934 features）。
- **真集群功能验收**（`redis-cli -p 6380`，**普通模式不加 `-c`**）：7 个分散在不同 slot
  的 key SET/GET 全部正确；hash tag 同 slot 的 MSET/MGET 正常；跨 slot MGET 回
  `CROSSSLOT Keys in request don't hash to the same slot` 且没有任何节点收到该命令；
  DEL/EXISTS 正常；`CLUSTER INFO` → `ERR unsupported by proxy: CLUSTER`；
  SCAN/KEYS/DBSIZE/RANDOMKEY/未知命令 → `ERR proxy: unsupported in cluster mode: ...`；
  PING/ECHO 本地应答（后端零往返）；INFO 正常返回。
- **ASK 路径手工验收**：对 slot 865 手工做 `CLUSTER SETSLOT ... IMPORTING/MIGRATING`，
  源节点上存在的 key 照常由源节点服务，不存在的 key 源节点回 ASK——经 proxy 后
  GET/SET 全部透明成功（客户端看不到 ASK）；期间反复读源节点上仍存在的 key 依然由源节点
  服务，**证明 ASK 没有污染拓扑**。迁移完成 `SETSLOT ... NODE` 后经 proxy 读写继续正确，
  MOVED 路径同样透明。
- **resharding 零错误（验收项）**：`cluster-loadcheck.py -c 50 -P 16` 持续 90s，
  期间（t=5s~26s，完整落在压测窗口内）用 `cluster-reshard.sh 300 --rounds 4` 双向搬迁
  1200 个 slot / 约 118 万个 key。结果 **4863 万次操作，errors=0，mismatches=0**，
  540,398 ops/s。
- **TSan 下重跑无数据竞争报告（验收项）**：4 worker，`-c 30 -P 16` 持续 60s +
  400 slot / 约 59 万 key 的双向 resharding，1984 万次操作，
  **errors=0、mismatches=0、零 ThreadSanitizer 报告**。

**性能基线（cluster 模式）**。命令行与 M3 基线 A 相同
（`redis-benchmark -c 50 -d 32 --threads 4 -r 1000000`，`-P 1` 取 n=100 万、
`-P 16` 取 n=500 万），proxy 4 worker、每节点 1 条连接，后端为 3 主。延迟单位毫秒：

| 配置 | 命令 | P | rps | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| cluster io_uring w4 | SET | 1 | 249,875 | 0.167 | 0.319 | 0.415 |
| cluster io_uring w4 | GET | 1 | 266,525 | 0.151 | 0.287 | 0.367 |
| cluster io_uring w4 | SET | 16 | 1,427,348 | 0.367 | 0.775 | 1.023 |
| cluster io_uring w4 | GET | 16 | 1,665,556 | 0.311 | 0.639 | 0.831 |
| cluster epoll w4 | SET | 16 | 1,175,641 | 0.447 | 0.919 | 1.191 |
| cluster epoll w4 | GET | 16 | 1,427,348 | 0.383 | 0.767 | 0.975 |

  解读：

  - **cluster 模式比 M3 的 standalone 模式慢，尽管后端从 1 个 redis 进程变成了 3 个**
    （`-P 16`：SET 1.43M vs 2.50M、GET 1.67M vs 2.85M，约 57~58%）。后端容量翻了三倍
    吞吐反而下降，说明瓶颈明确在 proxy 侧，不在后端。主因是**写合并被打散**：M3 下同一轮
    事件循环里所有请求都进同一条后端连接的 `byte_queue`，合并成一次大 send；cluster 下
    随机 key 均匀散到 3 个节点，每条连接只攒到约 1/3 的量，同样的请求数要发 3 倍次数的
    syscall。每请求多出的 slot 计算与槽位查表是次要项（standalone 复跑基线 A 无回归已经
    证明槽位机制本身不贵）。
  - 这条正是 M6 的头号优化目标：**恢复散开后的批处理效率**（例如按目标节点聚合同一轮的
    请求后再统一提交、或每节点多条连接配合更激进的合并窗口）。
  - epoll 比 io_uring 慢 14~18%（`-P 16`），与 M3 观察到的 12~22% 一致。
  - 上表不含直连对照组：M3 记录过直连 `-P 16` 在这台机器上会在 2.0M 与 3.33M 之间双峰
    跳变（单线程 redis-server 的核放置随机），横向比值不可靠。cluster 与 standalone 的
    纵向对比在同一轮内完成，可比。

---

## M5 — 可观测性与配置

**目标**：让它可以被真正运维。

任务清单：

1. **日志**：quill 接入；连接生命周期、后端异常、拓扑变更等关键事件结构化输出；数据面热路径零日志（或 trace 级）。
2. **metrics**：自研 Prometheus 文本输出（~200 行）：QPS（按命令类别）、延迟直方图（proxy 侧与后端侧分开）、活跃连接数、后端池状态、重定向计数、错误计数。每 worker 无锁累加，抓取时聚合。
3. **管理接口**：独立管理端口（HTTP）：`/metrics`、`/health`、`/topology`（当前 slot 映射）。
4. **配置**：TOML 配置文件（监听、后端、线程数、超时、上限等）+ CLI11 命令行覆盖；启动时校验并打印生效配置。热重载留作 backlog。
5. **完善优雅关闭**：drain 模式（停止 accept、在途完成、连接逐个关闭）、超时兜底。

**验收标准**：Prometheus 抓取 + Grafana 能画出 QPS/延迟/连接数核心面板；kill -TERM 在压测中不产生客户端错误。

---

## M6 — 性能打磨

**目标**：用数据驱动优化，逼近直连性能。

任务清单：

1. **基准环境固化**：`scripts/bench.sh` 一键跑 valkey-benchmark/memtier 标准场景集（不同 value 大小 × pipeline 深度 × 连接数），输出对比直连的报告；每次优化前后跑同一套。
2. **profile**：perf + 火焰图定位热点；重点检查：syscall 次数/请求、内存分配次数/请求、跨核 cache miss。
3. **候选优化项**（按预期收益排序，逐项用数据验证取舍）：
   - **恢复 cluster 模式下被打散的写合并（M4 移交，头号目标）**：M4 实测 cluster 模式
     `-P 16` 只有 standalone 的 57~58%，而后端进程数还多了两倍——随机 key 均匀散到 3 个
     节点后，每条后端连接只攒到约 1/3 的量，同样的请求数要发 3 倍次数的 syscall。
     候选做法：按目标节点聚合同一轮事件循环内的请求后统一提交、每节点多条连接配合更
     激进的合并窗口。
   - provided buffer ring + multishot recv/accept（M2 移交：multishot recv 依赖
     buffer ring，两者必须一起做）
   - 批量 submit（一次 `io_uring_submit` 提交多个 SQE）与回写合并（多条响应一次 send）
   - 协程帧内存池调优（复用率统计、尺寸分级）
   - RESP 解析器热点优化（memchr 向量化查找 CRLF 等）
   - recv bundle（内核 6.10+，探测启用）
4. **目标设定**（以 M3 章「性能基线 A：redis-benchmark」为对照——那组隔离出的是
   proxy 自身的开销；基线 B（memtier）里 proxy 靠连接复用+写合并反而快过直连，不适合
   当优化标尺。同一环境同一命令行复跑）：
   - `-c 50 -P 16`：M3 实测 p50 +25% / p99 +23%、吞吐为直连 86~88%。目标 p99 相对
     直连增加 **≤ 15%**，吞吐 **≥ 直连 92%**。
   - `-c 50 -P 1`：M3 实测每请求固定增量 ~32 µs（p50 0.103 → 0.135 ms），吞吐为直连
     77~85%。目标固定增量 **≤ 20 µs**，吞吐 **≥ 直连 90%**。
   - epoll 路径：M3 实测比 io_uring 慢 12~22%（M4 cluster 模式下 14~18%），不设优化
     目标，只要求不退化。
   - cluster 模式另立一档：M4 实测 `-P 16` 为 standalone 的 57~58%，目标
     **≥ 80%**（同一轮内纵向对比，不与直连比）。

**验收标准**：报告展示每项优化的前后对比；最终数字写入 README。

---

## Backlog（有意识推迟，不排期）

- `SUBSCRIBE`/`PSUBSCRIBE`（RESP3 push 转发，连接粘性）
- `MULTI/EXEC`（同 slot 事务透传）、`WATCH`
- 阻塞命令（BLPOP 等，专用后端连接）
- MGET/MSET 跨 slot 拆分聚合
- TLS（客户端侧与后端侧）、AUTH 透传/代持
- 读写分离（读走副本）、就近路由
- 配置热重载、慢查询日志
- inline command 支持
- cluster 模式下明确拒绝的命令（M4 定的，宁可报错也不猜）：
  - 单节点语义、集群下会静默给出错答案的 SCAN/KEYS/DBSIZE/RANDOMKEY/FLUSHALL/FLUSHDB
  - key 位置需解析变参才能确定的 SORT/SORT_RO（`STORE`）、GEORADIUS 系列（`STORE`）、
    XREAD/XREADGROUP（`STREAMS`）
- 伪造 `CLUSTER SLOTS`（全部 slot 指向 proxy 自己），让 cluster-aware 客户端库也能直接用
- `CLUSTER SLOTS` 回退（M4 只实现了 `CLUSTER SHARDS`）

## 工作约定

- 每个里程碑在 main 上以小步提交推进（当前单人开发，暂不强制分支/PR 流程）。
- 提交信息用 conventional commits（`feat:` / `fix:` / `docs:` / `perf:` / `test:` ...）。
- 新代码必须带测试；触碰解析器必须过 fuzz 短跑。
- 设计有分叉时先在 `docs/design/` 写一页决策记录再动手。
- 每个里程碑完成时更新本文档：勾掉任务、回填实际数据（基线数字等）。
