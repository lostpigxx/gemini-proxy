# M5 设计记录：可观测性与配置

> 日期：2026-09-14
> 状态：定稿（M5 实施依据）
> 前置：[m4-cluster-routing.md](m4-cluster-routing.md)、开发计划 M5 任务清单

## 0. 出发点：数据面齐了，运维面是零

M4 结束时 proxy 的数据面已经完整（多 worker、连接池、pipelining、cluster 路由、
MOVED/ASK），但整个进程**没有任何可观测手段**：

- 全进程 3 条日志，都在 `src/main.cpp`；`vkp_lib` 压根没链接 quill。数据面唯一的
  错误输出是 `server.cpp` 里一句 `fmt::print(stderr, "accept failed")`。
- 没有指标、没有管理接口。M4 明确把「拓扑观测」推给了本里程碑的 `/topology`。
- 配置只有十来个命令行开关，没有文件，也不打印生效配置。

M5 补齐这一面。它同时是 M6 的前置——M6 要「用数据驱动优化」，而 M4 已经留下一个
待解释的性能缺口（cluster 模式只有 standalone 的 57~58%，怀疑是写合并被打散）。
没有延迟直方图，这个怀疑就只能停留在怀疑。

## 1. 指标怎么跨线程读：松弛原子 + 单写者

这是 M5 唯一动摇既有不变量的地方，所以先说清楚。

M4 为了保住「worker 之间零共享可变状态」，宁可让拓扑刷新流量 ×N。M5 面对同类问题：
admin 线程抓取 `/metrics` 时要读到每个 worker 的计数器。两条路：

| 方案 | 代价 |
|---|---|
| **(A) 松弛原子 + 单写者** | 引入跨线程共享的可变状态；实现 ~30 行 |
| (B) 快照信箱：admin 戳 self-pipe，worker 在自己 loop 里 memcpy 一份出来 | 严格零共享；~100 行跨线程请求管线，抓取要等一次 loop 迭代 |

**选 (A)。** 理由：计数器是**单写者**的——每块计数器只有它的 owner worker 会写。
于是自增可以写成 relaxed load + relaxed store，而不是 `fetch_add`：

```cpp
struct alignas(64) counter {              // 每 worker 一块，cache line 独占
  std::atomic<std::uint64_t> v{0};
  void bump(std::uint64_t n = 1) noexcept {        // 只有 owner 线程调用
    v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  std::uint64_t read() const noexcept {            // admin 线程
    return v.load(std::memory_order_relaxed);
  }
};
```

arm64 上这编译成普通 `LDR` + `STR`，没有 `LDADD`，没有屏障——与非原子变量的代价
相同，但 TSan 干净、语义有定义。cache line 对齐保证 worker 之间不会互相 false sharing。

代价是抓取到的一组计数器**不是同一时刻的原子快照**（worker A 的 requests 可能比
worker B 的新几微秒）。对 Prometheus 这种按 `rate()` 消费的系统完全无所谓。

方案 (B) 严格更纯，但它买到的那点纯度，换来的是一条只在抓取时才走的跨线程请求管线
——多出来的状态机比它消灭的共享还多。记在 backlog，不做。

## 2. 延迟打点怎么做到零 syscall

两条直方图：proxy 侧（读到请求帧 → 该请求的槽位被填充）与后端侧（入队 → FIFO 配对）。
朴素做法是每请求 2~4 次 `steady_clock::now()`，每次约 20 ns 的 vDSO 调用，相对
~2 µs 的每请求预算是 2~4%。M6 正要去抠这些，M5 不能先在这里加回来。

两处观察让它降到零：

**后端侧本来就有现成的时间戳。** `backend_conn::push()` 已经算了
`deadline = now + request_timeout`，`deliver_head()` 已经为 `last_activity_` 算了一次
`now()`。于是后端时延 = `now - (deadline - cfg_.request_timeout)`，两个值都在手上，
一次新的时钟读取都不需要。

**proxy 侧改用 loop 缓存的时间。** `event_loop::run()` 每轮迭代都会在 `expire_timers()`
里调一次 `steady_clock::now()`。把它存进成员并公开：

```cpp
[[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept { return now_; }
```

精度就是一轮迭代——重负载下是微秒级，而直方图最小的桶是 100 µs，绰绰有余。
请求时间戳存进 `client_conn::reply_slot`（每槽 8 字节）。

**只给 metrics 用。** `backend_conn` 的超时判定继续用真实 `steady_clock::now()`：
一次 ready 队列的批量 drain 可能 resume 上千个协程，用缓存时间会让队尾的超时判定
偏早。既有超时语义不为了指标而改。

## 3. 直方图形状

Prometheus 的 histogram 要累积桶（`le`）+ `_sum` + `_count`。固定 17 个边界：

```
100µs 250µs 500µs 1ms 2.5ms 5ms 10ms 25ms 50ms 100ms 250ms 500ms 1s 2.5s 5s 10s +Inf
```

覆盖「本机回环 100 µs」到「超时上限 10 s」。桶定位从低位线性扫——正常流量落在前
2~3 次比较内，比二分快且无分支预测惩罚。`_sum` 用纳秒累加成 `uint64`（10 s × 2^64 ns
不会溢出），渲染时再转成秒。

## 4. 指标清单

全部 `vkp_` 前缀。默认**跨 worker 聚合**；另外加两条带 `{worker="N"}` 的小序列
（请求数、连接数），用来看 SO_REUSEPORT 的分配是否均衡——这是 thread-per-core 架构
最容易悄悄退化的地方，多 N×2 条序列值得。

| 指标 | 类型 | 标签 |
|---|---|---|
| `vkp_requests_total` | counter | `class`=read/write/admin/connection/other |
| `vkp_responses_total` | counter | `outcome`=ok/error/timeout/unavailable |
| `vkp_errors_total` | counter | `kind`=crossslot/unsupported/no_topology/too_many_redirects/backend_unavailable/protocol/outbuf_limit |
| `vkp_redirects_total` | counter | `kind`=moved/ask |
| `vkp_request_duration_seconds` | histogram | —（proxy 侧） |
| `vkp_backend_duration_seconds` | histogram | —（后端侧） |
| `vkp_client_connections` | gauge | — |
| `vkp_client_connections_total` | counter | — |
| `vkp_client_bytes_total` | counter | `dir`=in/out |
| `vkp_backend_connections` | gauge | `state`=connected/connecting/down |
| `vkp_backend_inflight` | gauge | — |
| `vkp_topology_refresh_total` | counter | `result`=ok/fail |
| `vkp_topology_nodes` | gauge | — |
| `vkp_topology_slots_assigned` | gauge | — |
| `vkp_uptime_seconds` | gauge | — |
| `vkp_build_info` | gauge | `version` |

**命令类别而不是命令名。** 按命令名打标签是 ~200 条序列 × N worker，对一个代理没有
相称的诊断价值。`command_info` 加一个 `cmd_class` 字段（read/write/admin/connection/
other），逐行标注。

## 5. admin 接口

**跑在独立线程的独立 `event_loop` 上。** 渲染一次 `/metrics` 要拼几 KB 字符串，
放到某个 worker 的 loop 上就是往数据面注入一个周期性的毛刺——而 M6 正要拿 p99 说话。
独立线程完全复用现有的 `io::listen_tcp` / `io::task` / `send_all`，不新造轮子；关停
沿用 worker 那套 self-pipe，`main` 把它并进已有的信号扇出数组。

**极简 HTTP/1.1**：只解析请求行，丢弃头部直到 `\r\n\r\n`，请求上限 8 KB，仅支持
GET，响应固定带 `Content-Length` 与 `Connection: close`。不做 keep-alive——Prometheus
的抓取频率是 15 s 级，省下的那次握手不值得多一份状态机。

| 路径 | 行为 |
|---|---|
| `GET /metrics` | `text/plain; version=0.0.4; charset=utf-8`，聚合输出 |
| `GET /health` | 200 `{"status":"ok",...}`；drain 开始后立刻 503 `{"status":"draining"}` |
| `GET /topology` | worker 0 的完整 slot 区间映射 + 每 worker 一行摘要 |
| 其他路径 | 404 |
| 非 GET | 405 |

**默认开启但只绑 127.0.0.1**（`--admin-listen 127.0.0.1:9180`，传空串关闭）。
`/topology` 会暴露后端集群的真实地址，默认对外监听是不负责任的。

`/health` 只表达**自身**是否在服务：drain 中返回 503，让 LB 先摘流量；后端全挂时
仍返回 200——所有 proxy 实例共享同一个后端集群，把它们一起摘掉毫无意义，那属于
后端的告警而不是 proxy 的健康。

**新解析器就要有 fuzz。** admin 的 HTTP 请求解析是本项目第二个吃外部输入的解析器，
按既有约定加 `fuzz/admin_http_fuzz.cpp`。

### `/topology` 的快照怎么发布

拓扑是每 worker 各自持有、各自刷新的（M4 §4.2），admin 线程不能直接读。做法：每个
worker 在 `router::adopt()` 成功后，把拓扑**预渲染成字符串**写进自己 stats 块里的一个
槽位，用一把普通 `std::mutex` 保护。

用互斥锁而不是 `std::atomic<std::shared_ptr<>>`：后者在项目的编译器基线上可用性存疑
（同 `std::expected` 那类坑，见 CLAUDE.md），而这把锁的争用频率是**刷新周期 0.2 Hz
对抓取 0.067 Hz**，数据面永远不碰它。为了这种地方引入基线风险不划算。

M4 的决策让各 worker 的拓扑可能短暂不一致。`/topology` 因此渲染 worker 0 的完整
slot 映射 + **每 worker 一行摘要**（节点数 / 已分配 slot 数 / 快照年龄）——不一致的
时候一眼就能看见，而不是被一个「代表性」视图掩盖过去。

## 6. 配置：toml++ 解析，CLI 覆盖

架构决策文档同时列了 CLI11 和 toml++，但配置读取二选一。选 **toml++ 自己解析**：

- 真正的分节 TOML（`[listen]` / `[cluster]` / `[limits]` / `[admin]` / `[log]`），
  比 CLI11 内置格式要求的「键名必须逐一对应命令行选项名」可读得多。
- 报错带行列号（`proxy.toml:9:14: expected integer, got string`）。
- toml++ 此前只在 smoke test 里出现过，这正是当初把它选进来的用途。

优先级链：**默认值 → TOML 文件 → 命令行显式项**。「显式」用 CLI11 的
`opt->count() > 0` 判断，而不是「值不等于默认值」——后者无法区分「没给」和
「给了一个恰好等于默认值的值」。

新增 `src/config/settings.{hpp,cpp}`：`settings` 结构体、`load_toml(path)`、
`to_string(settings)`。启动时校验并打印生效配置，这样线上出问题时第一条日志就能
排除掉「配置没生效」这种假设。

## 7. 优雅关闭的缺口

M3 已经有 drain 骨架（停 accept → 等在途 → 排空后端池 → 停 loop，5 s grace 兜底）。
缺的是：

**空闲客户端连接会把 drain 拖满 grace。** 现状是干等 `active_ == 0`。一个 idle 的
长连客户端（连接池里趴着的那种，生产环境遍地都是）不会主动断开，于是必然走到 5 s
超时硬停——而硬停会打断那一刻真正在途的请求。修法：停 accept 之后主动关闭
`slots_` 为空的连接，有在途请求的让它跑完。

**`/health` 要在 drain 一开始就翻 503**，给 LB 摘流量的时间窗。

**验收判据要诚实。** `scripts/cluster-loadcheck.py` 现在把连接被关当错误计。优雅
关闭下「在相位边界收到干净 EOF」是正确行为，「有在途请求却被丢」才是错误。加一个
`--expect-close` 模式区分这两者，否则这条验收标准要么必然失败、要么只能靠人眼放水。

## 8. 与计划文档的偏差

计划文档 M5 的验收标准写的是「Prometheus 抓取 + Grafana 能画出核心面板」。Grafana
部分改为：仓库内提供 `deploy/grafana-dashboard.json`，并用 Prometheus 的
`/api/v1/query` 逐条证明面板的数据源可查。理由是本机拉 Grafana 镜像的代价（见
CLAUDE.md 的网络说明）与它带来的验证价值不相称——真正要证明的是**指标存在、格式
合规、随负载变化**，这三件 `promtool` + PromQL 查询能证明得更硬。
