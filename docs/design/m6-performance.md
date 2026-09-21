# 设计：M6 性能打磨的测量口径与优化排序

> 日期：2026-09-21
> 状态：进行中
> 范围：基准环境（`scripts/bench.sh`、`scripts/bench-env.sh`）、M6 验收口径的修正、
> profile 结论与候选优化的重新排序。

这一页记录的是**分叉**，不是流水账：开发计划里 M6 的验收标准和候选优化清单，在真正把仪器
搭起来之后有一半站不住了。下面逐条说明改成了什么、为什么。

## 1. 先修仪器：五个已经产出过错误数字的陷阱

M6 的第一项任务是"基准环境固化"。固化之前先做的事是**证伪已有的基线**——M3/M4/M5 的表格里
有一部分数字测的不是代码。

### 1.1 GET 命中率（"双峰吞吐"的真正根因）

记忆和开发计划里都把直连 `-P 16` 在 2.0M 与 3.3M 之间的跳变归因于"单线程 redis-server 的
核放置随机"。这是错的。真正的原因是 **GET 打没打中**：`redis-benchmark -t get -r 1000000`
打一个没填过的库，全部是 miss，而 `$-1` 的回复字节数只有 `-d 32` 命中回复的四分之一。
同一台机器同一秒内测：

| keyspace | GET rps（直连，`-c 50 -P 16 -d 32`） |
|---|---|
| 空 | 3,996,802 |
| 填满 | 1,537,042 |

**2.6 倍**。前几个里程碑里凡是没说清楚库填没填的 GET 数字，都不能跨轮比较。

处置：`bench.sh` 每轮开跑前用 `KEYSPACE * 4` 次随机 SET 填库（coupon collector，覆盖率
≈98%），跑完 `DBSIZE` 并把结果打进报告；低于 95% 直接告警。命中率从"默认假设"变成"在案记录"。

### 1.2 rps 被计时精度量化

redis-benchmark 报告的 rps 来自它自己 250 ms 的进度 tick，所以一次 3 秒的跑动只能落在
约 8% 的台阶上（实测所有值都落在 1.751 / 2.752 / 3.002 / 3.253 / 3.503 秒这张 0.25 秒的
网格上）——比 M6 想测的任何东西都粗。

处置：`bench.sh` 自己掐表算 wall-clock rps，工具自报的值留在 `results.tsv` 的
`rps_reported` 列里备查。在网格点上两者相差 0.06% 以内，离开网格点时只有 wall-clock 是对的。

### 1.3 压测端才是瓶颈

给 redis-benchmark 四个核时，**每个 arm 都读回同一个 363,504 rps**，直连 `-P 16` 从 2.86M
塌到 1.43M。测的是客户端，不是服务端。给到八个核（4 线程）后 `-P 16` 才能逐轮复现。

### 1.4 没固核 + `--cpu-affinity` 静默失效

`--cpu-affinity` 从 0 开始编号 CPU，于是 `taskset -c 5-8 proxyd --cpu-affinity -w 4` 实际请求
的是 CPU 0..3，每次 `pthread_setaffinity_np` 都返回 EINVAL，返回值又被丢掉——worker 全程在飘，
某些轮次两个 worker 挤一个核，表现为轮次间 8% 的吞吐摆动。已修（`worker_pool.cpp` 现在读
`sched_getaffinity` 拿到的实际掩码，按序取第 i 个可用核）。

固核之后（后端独占一核、worker 一核一个、压测端独占 8 核），高负载下的**轮次间离散度从
7~9% 降到 0.0~0.3%**。固核在这里不是调优手段，它是仪器本身。

### 1.5 跨轮比较

沿用 M5 的结论并写进了 `bench.sh` 的结构：脚本**不拥有任何进程**，只接受重复的
`--arm label=host:port[@pid]`，并在**每个场景内部**交替跑各个 arm，使两个被比较的数字相隔几秒
而不是几分钟。`--baseline` 给出的跨轮 diff 明确标注为 advisory。

## 2. 验收口径：吞吐比不是良定义的，改用 CPU µs/请求

开发计划给 M6 定的目标是"吞吐 ≥ 直连的 92%"。在这套仪器下这个指标测不出它想测的东西：

- `-c 50 -P 1`：redis-benchmark 是闭环压测，吞吐 = 并发数 / 延迟，50 连接时客户端先饱和，
  **所有 arm 都读回 ~363k**，与 proxy 无关。
- `-c 200 -P 1`：proxy 比直连**快 40%**（543k vs 386k），因为它把 200 个客户端 socket 汇聚
  到一条做了写合并的后端连接上。这时候"相对直连的比值"衡量的是**后端单线程 socket 处理能力**，
  不是 proxy 的开销。

也就是说这个比值既可能因为客户端饱和而恒为 1，也可能因为 proxy 帮后端省了活而大于 1。
两种情况下它都不随 proxy 的代码质量单调变化。

**替代指标：proxy 每请求消耗的 CPU 微秒**，由 `/proc/<pid>/stat` 的 utime+stime 在每次跑动
前后各读一次算得（`label=host:port@pid`）。它只计 proxy 自己的开销，不受后端和压测端的
饱和行为影响，且对优化单调响应。吞吐与 p99 仍然照测照报，但**判定优化是否生效以 CPU/请求为准**。

### M6 起始基线

固核、填库、wall-clock rps、3 轮取中位数：

| 场景 | 直连 rps | proxy rps | proxy/直连 | proxy CPU µs/req |
|---|---|---|---|---|
| SET P=1 d=32 c=200 | 386,150 | 543,550 | 140.8% | 6.265 |
| GET P=1 d=32 c=200 | 385,970 | 543,190 | 140.7% | 6.160 |
| SET P=16 d=32 c=50 | 1,426,050 | 1,535,977 | 107.7% | 0.554 |
| GET P=16 d=32 c=50 | 1,535,645 | 1,663,983 | 108.4% | 0.552 |

单机模式下"吞吐 ≥ 直连 92%"这条已经被大幅超过，且如上所述不是它自己的功劳。

## 3. profile：钱花在哪里

### 3.1 用户态 / 内核态划分

容器里的 perf 只能做用户态采样（`task-clock:upppH`），所以先量的是总的用户/内核劈分：

| 场景 | 用户态 | 内核态 | 用户态占比 | 合计 µs/req |
|---|---|---|---|---|
| P=16 c=50 | 1640 ms | 3490 ms | **32%** | 0.513 |
| P=1 c=200 | 1030 ms | 11110 ms | **8%** | 6.070 |

**P=1 下 proxy 有 92% 的 CPU 在内核里。** 用户态微优化在这个工况下最多能动 8%。

### 3.2 syscall/请求：已经不是问题

开发计划把"syscall 次数/请求"列为重点检查项，并据此把 provided buffer ring + multishot recv
排在候选优化的第二位。实测（strace 计数 + 同窗口内后端 `total_commands_processed` 增量）：

| 场景 | 窗口内请求数 | `io_uring_enter` | enter/请求 | **请求/syscall** |
|---|---|---|---|---|
| P=16 c=50 | 8,484,389 | 311,610 | 0.037 | **27** |
| P=1 c=200 | 1,908,561 | 160,322 | 0.084 | **12** |

批量提交（`poll()` 里的 `io_uring_submit_and_wait_timeout`，`submit()` 不单独进内核）已经把
12~27 个请求摊到一次系统调用上。**内核时间不是系统调用入口开销，是 loopback TCP 栈真正在搬
每请求 4 次 socket 操作**（client→proxy、proxy→backend、backend→proxy、proxy→client），
P=1 下约合每次 1.4 µs。

推论：provided buffer ring / multishot recv 攻击的是 SQE 条数和入口次数，这两项都已经摊薄了
一个数量级，**把它们从候选清单的第二位降到末位**。能减内核时间的只有"减少 socket 操作次数
或包数"，也就是写合并——而单机模式的后端方向写合并已经在做了（这正是 c=200 P=1 下 proxy 反超
直连 40% 的原因）。

### 3.3 用户态热点（P=16，仅用户态采样）

`__kernel_clock_gettime` 46.33%、`memcmp` 5.57%、`find_command` 4.32%、`client_conn::reader`
4.11%、`parser::parse` 3.92%、`dispatch_header` 3.53%、liburing 2.25%、`parser::finish` 2.07%、
`toupper` 1.93%、`read_frame` 1.72%、`frame_pool::deallocate` 1.60%、`frame_pool::allocate`
0.91%、`uring_backend::poll` 0.88%、`steady_clock::now` 0.81%、`backend_conn::push` 0.81%。

读这张表时必须乘上 3.1 的占比：46% 是**用户态的** 46%，P=16 下折合总量 15%，P=1 下折合 4%。

## 4. 已实施：每请求的时钟读取改用 loop 缓存值

`backend_conn::push()` / `deliver_head()` / `fail_all()` 里的
`std::chrono::steady_clock::now()` 换成 M5 引入的 `loop_.now()`（每次 loop 迭代缓存一次的
`steady_clock` 时间戳）。

**正确性论证**：这三处的时间戳只有两个用途——请求超时判定，和 `backend_duration` 直方图。
陈旧度上界是一次 loop 迭代，量级是微秒；超时以秒计，直方图的分辨率也远粗于此。并且一个请求
的两个时间戳（`push` 打的 deadline 与 `deliver_head` 读的 now）之间必然隔着至少一次 poll，
所以采样区间不会塌成 0。

**A/B 结果**（同轮交替，3 轮）：

| 场景 | 改前 CPU µs/req | 改后 | |
|---|---|---|---|
| P=16 c=50 | 0.552 | 0.514 | **93.1%** |
| P=1 c=200 | 6.16 | ~6.10 | ~99% |

吞吐不变（后端受限）。P=16 省下 6~7%，远小于 profile 里的 46%，原因见 3.1——那是用户态的 46%。

## 5. 候选优化的重新排序

按"实测能动的总 CPU"排，而不是按计划里的原始顺序：

1. **集群模式的写合并**（计划里的 #1，仍然是 #1）。M4 交接时集群 `-P 16` 只有单机的 57~58%。
   注意这个数字是在 1.1 的命中率混淆存在时测的，**必须用新仪器重测基线**再谈优化。
2. **`find_command` 热路径**（约占用户态 12%：memcmp 5.57 + find_command 4.32 + toupper 1.93）。
   `std::toupper` 是 locale-aware 的 libc 调用、不内联；`std::lower_bound` + `memcmp` 的比较
   链也可以换成 constexpr 完美哈希。P=16 下折合总量约 4%，便宜且确定。
3. **`backend_uring.cpp` 的 `std::unordered_set<operation*> inflight_`**：每个 IO 操作一次
   insert + 一次 erase。换成 `operation` 上的一个标志位 + 计数器。
4. 协程帧内存池调参、RESP 解析器向量化 CRLF 搜索。
5. **provided buffer ring + multishot recv/accept、recv bundle**——降级至此，理由见 3.2。
   若要做，仍须 ring 与 multishot 成对做（M2 交接结论）。

## 6. 与开发计划的偏差汇总

| 计划原文 | 实际 | 原因 |
|---|---|---|
| 目标"吞吐 ≥ 直连 92%"（`-c 50 -P 16`／`-P 1`） | 改判为 proxy CPU µs/请求，吞吐降为参考值 | §2，比值不随代码质量单调变化 |
| "基线 A" `-c 50 -P 1` 作为标准场景 | `quick` 矩阵改用 `c=200`；`full` 保留 `c=50` 以便与 M3/M4 表格对照 | §1.3，c=50 时客户端饱和，轮间摆动 232k~363k |
| 重点检查"syscall 次数/请求" | 已检查，**不是瓶颈**（12~27 请求/次系统调用） | §3.2 |
| provided buffer ring + multishot 排第二 | 降至末位 | §3.2 |
| 双峰吞吐 = 单线程 server 核放置随机 | = GET 命中率 | §1.1 |
