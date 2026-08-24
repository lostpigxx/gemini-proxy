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
| M1 | RESP 协议解析器 | 经 fuzz 验证的零拷贝 RESP2/3 解析与序列化库 |
| M2 | 事件循环 + 最小可用 proxy | `valkey-cli` 可通过 proxy 访问单个后端 |
| M3 | 多线程 + 连接池 + pipelining | 可承受多连接压测的生产形态骨架 |
| M4 | cluster 路由 | 对接 valkey cluster，处理 MOVED/ASK |
| M5 | 可观测性与配置 | metrics / 日志 / 配置 / 优雅关闭 |
| M6 | 性能打磨 | 基线固化与针对性优化 |

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

## M1 — RESP 协议解析器

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

---

## M2 — 事件循环 + 最小可用 proxy

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

---

## M3 — 多线程 + 连接池 + pipelining

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

**验收标准**：`valkey-benchmark -c 50 -P 16` 与 memtier 混合读写压测正确通过；对比直连记录延迟损耗（P50/P99）并写入 docs 作为基线；TSan 压测无数据竞争报告。

---

## M4 — cluster 路由

**目标**：对接 valkey cluster，proxy 屏蔽拓扑细节。

任务清单：

1. **slot 计算**：CRC16/XMODEM（cluster-spec 附录 A 参考实现，自研 ~30 行）+ `{...}` hash tag 规则；单测用 spec 中的测试向量。
2. **拓扑管理**：启动时 `CLUSTER SHARDS` 拉取 slot→节点映射；周期刷新 + 事件触发刷新（收到 MOVED 时）。拓扑对象不可变 + 原子替换（每 worker 持有本地副本，避免锁）。
3. **路由**：按第一个 key 的 slot 选择后端；无 key 命令（PING 等）本地应答或随机节点；multi-key 命令先要求同 slot（跨 slot 返回 `-CROSSSLOT` 语义错误），MGET/MSET 拆分聚合留作 backlog。
4. **重定向处理**：MOVED → 更新拓扑并向新节点重试（有限次数）；ASK → 向目标节点发 `ASKING` + 原命令，不更新拓扑。
5. **节点生命周期**：新节点出现时按需建池；节点从拓扑消失时 drain 并关闭其连接池。
6. **集成测试环境**：`scripts/cluster-up.sh` 用 docker compose 拉起 3 主 3 从本地集群；集成测试脚本覆盖正常读写 + 手动 resharding 场景。

**验收标准**：压测运行中执行 slot 迁移（resharding），客户端无错误（或仅有可解释的瞬时重试延迟）；MOVED/ASK 路径有集成测试覆盖。

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
   - provided buffer ring（io_uring 内核选 buffer，省 per-recv 提交）
   - 批量 submit（一次 `io_uring_submit` 提交多个 SQE）与回写合并（多条响应一次 send）
   - 协程帧内存池调优（复用率统计、尺寸分级）
   - RESP 解析器热点优化（memchr 向量化查找 CRLF 等）
   - recv bundle（内核 6.10+，探测启用）
4. **目标设定**：以 M3 记录的基线为准，定量目标在基线出来后回填本文档（例如：P99 相对直连增加 ≤ 30%，单核 QPS ≥ 直连单实例的 X%）。

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

## 工作约定

- 每个里程碑在 main 上以小步提交推进（当前单人开发，暂不强制分支/PR 流程）。
- 提交信息用 conventional commits（`feat:` / `fix:` / `docs:` / `perf:` / `test:` ...）。
- 新代码必须带测试；触碰解析器必须过 fuzz 短跑。
- 设计有分叉时先在 `docs/design/` 写一页决策记录再动手。
- 每个里程碑完成时更新本文档：勾掉任务、回填实际数据（基线数字等）。
