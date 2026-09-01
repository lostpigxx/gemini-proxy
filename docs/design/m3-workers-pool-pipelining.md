# M3 设计记录：多 worker、后端连接池与 pipelining 保序

> 日期：2026-09-01
> 状态：定稿（M3 实施依据）
> 前置：[io-and-coroutines.md](io-and-coroutines.md)、开发计划 M3 任务清单

## 1. 多 worker 模型

- thread-per-core：每 worker 一个线程，独立 `event_loop` + 独立 `server` 实例 +
  独立 SO_REUSEPORT listen socket。worker 之间**零共享可变状态**（config 只读，
  quill 前端本身线程安全）。`frame_pool` 是 thread-local，协程不跨线程迁移，天然安全。
- worker 数 `--workers`（0 = `hardware_concurrency`），默认 1（proxy 的典型部署是
  sidecar，多核独占不是默认场景；压测时显式给值）。
- 临时端口（port 0）问题：REUSEPORT 下各自 bind 0 会得到不同端口。做法：worker 0
  先 bind（可为 0），其余 worker 用 worker 0 的实际端口再 bind。所有 listener 都在
  bind 前设 SO_REUSEPORT。
- CPU 亲和性 `--cpu-affinity`：Linux 用 `pthread_setaffinity_np` 顺序绑核；macOS
  无等价 API，记一条日志跳过。
- 信号：handler 向**每个 worker** 的 self-pipe 各写一字节（fd 数组在装 handler 前
  就绪，`write` 是 async-signal-safe）；每 worker 自己的 signal_watcher 走 M2 的
  drain 逻辑。主线程 join 全部 worker。

## 2. 后端连接与 pipelining 保序（核心）

### 2.1 结构

每 worker 每后端 `conns_per_backend`（默认 1，可 1~2）条长连接，`backend_conn` 拥有：

- `out_`：出向**双缓冲字节队列**（见 2.4），客户端请求字节 memcpy 进来合并发送
  （天然的写合并）；
- `inflight_`：FIFO 配对队列，`deque<entry>`，entry =
  `{client*, kind(forward|local|internal), local_reply, deadline}`；
- 四个常驻协程：**driver**（重连循环 + 响应读取/配对）、**writer**（刷 `out_`）、
  **watchdog**（连接超时/请求超时/健康检查，见 §3）、由 driver 内联承担 reader 职责。

### 2.2 保序规则

redis 协议按连接 FIFO。两条规则给出全部保序性：

1. **客户端在 accept 时固定绑定一条 backend_conn**（round-robin 挑选），其全部请求
   走这一条连接 → 后端响应顺序 = 该客户端请求顺序（中间夹着别的客户端的响应，
   按队列各归各家）。
2. **本地生成的回复（拒绝命令、HELLO、后端不可用错误等）也作为 entry 进同一条
   in-flight 队列**（kind=local，不占后端字节）。投递规则：队头是 local 就立即投递，
   不消耗后端帧；forward 条目配对一帧后继续排空后续 local 队头。这样
   `GET k / SUBSCRIBE x / GET k2` pipelined 时错误回复严格落在两个 GET 响应之间。

`internal` 条目（健康检查 PING）同队列配对，响应丢弃。

后端断开：队列中全部未决 entry 统一回错误（超时的回 `-ERR proxy timeout`，其余回
`-ERR proxy: backend connection lost`；local 条目照常投递自己的回复）。断开期间新到的
forward 请求不排队等重连，直接回 `-ERR proxy: backend unavailable`（可预期性优先）。

客户端先死：不能从 FIFO 中间抽走 entry（会错位），把该客户端的 entry 打墓碑
（client 置空），响应到达后丢弃。

### 2.3 客户端连接

每客户端两个协程：reader（读帧→查命令表→入队/本地回复）与 writer（刷自己的出向
字节队列）。响应投递 = memcpy 进客户端出向队列 + 唤醒 writer，后端 driver 从不直接
往客户端 fd 写——慢客户端不会阻塞后端读取（head-of-line 只剩内存占用，由高水位
兜底：出向队列超过 `client_outbuf_limit` 直接断开该客户端）。

### 2.4 双缓冲字节队列（`core/byte_queue`）

出向路径的关键坑：writer `co_await async_send` 期间指针必须稳定，而其他协程还要
继续 append。单一 buffer 的 append 可能触发扩容/搬移 → 在途 op 悬垂。用双缓冲：
append 永远进 pending，writer 把 pending 与空闲的 sending 交换后发送 sending，
发送期间 pending 继续收数据。

### 2.5 背压

- 入队前检查绑定连接的 `inflight_.size() >= max_inflight` 或 `out_` 字节超限 →
  client reader `co_await` 等待队列（§4），响应配对/写出后 notify。等待期间该客户端
  的 socket 自然不再被读，TCP 流控向上游传导。
- 慢客户端：出向队列高水位断开（上面已述）。

## 3. 超时体系

**决策：不用 io_uring LINK_TIMEOUT，全部超时统一为「每 backend_conn 一个 watchdog
协程 + cancel_slot 取消」**（与计划文本的偏差，记录理由）：

- LINK_TIMEOUT 是 io_uring 独有，epoll/kqueue 仍要走用户态定时器——两套语义两套代码；
  用户态方案三后端同一条路径。
- 超时是罕见路径。正常完成时用户态方案零额外 SQE（watchdog 睡在「队头 deadline」上，
  队头正常弹出后 watchdog 醒来重算即可，不逐请求挂/摘定时器）；LINK_TIMEOUT 则每个
  op 多一个 SQE + 一次内核定时器挂摘，热路径反而更贵。
- M6 若 profile 显示 watchdog 唤醒成本可见，再评估 LINK_TIMEOUT。

watchdog 状态机（每 backend_conn 一个，常驻）：

- connecting → 睡到 connect deadline，到点仍未连上就 `cancel(connect_slot)`；
- connected 且队列非空 → 睡到队头 deadline，到点仍未弹出 → 判定后端无响应：
  取消 driver/writer 的在途 op，全队列回错误，连接重建；
- 空闲 → 睡 `health_interval`，到点仍空闲则入队 internal PING（其超时由上一条规则
  自动覆盖）；
- 状态迁移方（入队、连上、drain）取消 watchdog 当前的睡眠让它重算。需要给
  `cancel_slot` 加 `reset()`。

**先取消后 close**：fd 上还有在途 op 时不得 close（reactor 后端的槽位会悬空，epoll
自动摘除后 op 永远不完成）。连接销毁顺序固定为：cancel 全部相关 slot → 等协程停到
安全点 → close(fd)。

请求超时的 deadline 从入队时刻起算（含排队时间，界定的是端到端上界）。

## 4. 新 IO 原语

- **`wait_queue`**（`io/wait_queue.hpp`）：单线程 async 条件变量。`wait()` 挂起
  （operation 停在 wait_queue 自己的侵入链表里），`notify_one/all(result)` 把 op 推回
  event_loop ready 队列。wait_queue 在构造时向 event_loop 注册（侵入式双链），两个
  安全网：`stop()` 时全部 parked waiter 以 `-ECANCELED` 完成；`run()` 发现只剩 parked
  waiter（无 backend op、无定时器、ready 空——不可能再有人 notify）时同样全部取消，
  避免挂死或泄漏。约定：`wait()` 返回 `< 0` 一律走退出路径。
- `event_loop::post(operation&)`：把已填好 result 的 op 推入 ready 队列（wait_queue
  的 notify 用）。
- `cancel_slot::reset()`：无在途 op 时清除 requested 标志，允许复用（watchdog 循环用）。
- `listen_tcp` 增加 `reuseport` 参数。

## 5. 命令表（`proxy/command_table`）

静态 constexpr 表（大写命令名排序 + 二分；查询时栈上大写化，超过表内最长名直接判
未知），字段为 M4 路由预留：

```
name, policy(forward|reject|local), first_key, last_key, key_step, reject_reason
```

- **reject**（连接粘性/阻塞/会污染共享连接的状态类命令）：MULTI/EXEC/DISCARD/WATCH/
  UNWATCH、SUBSCRIBE/UNSUBSCRIBE/PSUBSCRIBE/PUNSUBSCRIBE/SSUBSCRIBE/SUNSUBSCRIBE、
  BLPOP/BRPOP/BLMOVE/BLMPOP/BRPOPLPUSH/BZPOPMIN/BZPOPMAX/BZMPOP、WAIT/WAITAOF、
  MONITOR、AUTH、CLIENT。回复 `-ERR unsupported by proxy: <CMD>`。
- **local**（共享后端连接后**必须**本地应答，M2→M3 的行为变化）：
  - `HELLO [2|3]`：本地回自述 map（proto 按请求回 2/3 格式），不再透传——透传会把
    共享连接切到 RESP3，影响所有客户端。后端连接固定 RESP2；RESP2 响应帧是合法的
    RESP3 帧，valkey-cli -3 可用，完整 RESP3 保真（map/null 类型映射）进 backlog。
    带 AUTH 参数回错误；SETNAME 忽略。非 2/3 回 `-NOPROTO`。
  - `QUIT`：本地 +OK，冲刷后关连接（透传会杀共享连接）。
  - `SELECT`：参数为 0 回 +OK，否则报错（共享连接不允许切库）。
  - `RESET`：本地 +RESET（proxy 侧无粘性状态可清）。
- 未知命令 → forward（后端自己会报错，proxy 不做白名单）。
- 客户端发来的非命令形态顶层帧（不是 bulk 数组）→ 本地协议错误回复，不透传。

## 6. 关闭与生命周期

- drain（第一次信号）：撤 accept → 在途客户端自然结束 → 通知全部 backend_conn
  drain（置标志、取消在途 op、唤醒各等待队列）→ driver 退出 → 循环自然排空。
  grace 超时或第二次信号 → `loop.stop()` 硬停（wait_queue 注册机制保证 parked
  waiter 也被取消，无泄漏）。
- 客户端关闭统一路径：取消 reader/writer 的 slot → join 两协程（计数 + wait_queue）
  → 墓碑化 in-flight entry → close(fd)。

## 7. 配置新增（默认值）

| 项 | 默认 | 说明 |
|---|---|---|
| workers | 1 | 0 = 核数 |
| cpu_affinity | off | Linux 生效 |
| conns_per_backend | 1 | 每 worker 每后端 |
| max_inflight | 1024 | 每后端连接，背压阈值 |
| connect_timeout | 1s | |
| request_timeout | 1s | 入队起算 |
| health_interval | 5s | 空闲才发 PING |
| reconnect backoff | 50ms × 2ⁿ，上限 2s | 连上即复位 |
| client_outbuf_limit | 8 MiB | 慢客户端断开水位 |

## 8. 测试策略

- io：wait_queue（notify 顺序、stop 取消、idle 自愈取消）、cancel_slot::reset、
  REUSEPORT 双 listener 分流。
- command_table：大小写、reject/local/forward 分类、key 位置字段。
- proxy 功能（fake backend 换成 ECHO 语义以验证配对归属）：多客户端交错 pipelining
  保序；reject 命令回复插序正确；HELLO/QUIT/SELECT/RESET 本地语义；后端启动前不可用
  →拉起后自动重连；请求超时→连接重建→恢复；后端中途断开→未决请求回错误；
  max_inflight=1 下深 pipeline 正确；多 worker（2 线程）冒烟。
- 验收：valkey-benchmark `-c 50 -P 16`、memtier 混合读写（Linux 容器）；对比直连的
  P50/P99 记入开发计划；ASan/TSan 压测无报告。
