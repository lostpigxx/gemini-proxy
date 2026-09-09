# M4 设计记录：cluster 路由

> 日期：2026-09-08
> 状态：定稿（M4 实施依据）
> 前置：[m3-workers-pool-pipelining.md](m3-workers-pool-pipelining.md)、开发计划 M4 任务清单

## 0. 出发点：M3 的保序前提失效了

M3 的保序建立在一条规则上：**客户端在 accept 时固定绑定一条 `backend_conn`**，其全部
请求走这一条连接，后端按 FIFO 回，顺序天然正确（设计记录 M3 §2.2 规则 1）。

cluster 下这条前提不成立——同一个客户端的相邻两条请求会按 key 的 slot 落到不同节点，
两个节点各自的响应之间没有任何顺序关系。所以 M4 的第一件事不是 slot 计算，而是
**把定序权从「后端连接的 FIFO」搬到「客户端自己」**。其余（slot、拓扑、MOVED/ASK）
都建立在这之上。

## 1. 客户端有序回复槽位（核心改动）

`client_conn` 成为唯一的定序权威：

- 每读到一条请求就分配一个单调递增 token，并在 `std::deque<slot>` 尾部压一个槽位。
  `slot = { filled, reply, request（重试用，见 §5）, redirects }`。
- 后端响应带 token 回来 → 填对应槽位 → 从队头开始**只刷出连续已填充的前缀**。
- 队头恰好是刚填的那个（绝大多数情况）时直接 append 进 `out_` 并弹出，省掉「先存
  `std::string` 再拷进 out_」的第二次拷贝。乱序到达的才落 `reply` 暂存。

`reply_sink` 接口随之改成带 token：

```cpp
virtual void deliver(std::uint64_t token, std::string_view frame) = 0;
```

### 连带简化：删掉 M3 的 `entry.local` 机制

M3 为了让本地回复（HELLO/拒绝/后端不可用错误）在 pipeline 中落到正确位置，把它们
也塞进后端连接的 in-flight FIFO（`kind=local`，见 M3 §2.2 规则 2）。现在客户端自己
有槽位定序了，本地回复直接填自己的槽即可。因此删除：

- `backend_conn::enqueue_local()`、`entry.local`、`entry.local_reply`、
  `drain_local_heads()`，以及 `fail_all()` 里对 local 条目的分支。

`backend_conn` 退化成纯传输层，entry 只剩两种：`forward`（有 sink + token）与
`internal`（`sink == nullptr`，回复丢弃——健康检查 PING 与 §5 的 `ASKING`）。

### 客户端与多条后端连接的关系

一个客户端可能向多条 `backend_conn` 入队，teardown 时必须对**用过的每一条**调用
`detach()` 打墓碑（M3 的教训：绝不能从 FIFO 中间摘除 entry，会打乱后端连接上的对齐）。
用 `std::vector<std::shared_ptr<backend_conn>> used_conns_`（去重，典型 1~3 条）
同时解决两件事：detach 的覆盖面，以及节点被移出拓扑时的生命周期（§4）。

### 背压

入队前按**目标**连接检查 `available()/has_capacity()`，不满足就 park 在该连接的
`capacity_event()` 上，语义与 M3 一致。后果是「A 节点满」会连带阻住排在后面、本该
去 B 节点的请求——这是 RESP 单连接顺序语义的固有代价，不是缺陷。

## 2. slot 计算（`src/cluster/slot.{hpp,cpp}`）

CRC16/XMODEM（poly 0x1021，256 项静态表）+ `{...}` hash tag 规则，
`key_slot(std::string_view) -> std::uint16_t`。纯函数、零 IO、100% 单测。

hash tag 规则（cluster-spec）：找第一个 `{`；从它之后找第一个 `}`；两者之间**非空**
才取该子串，否则用整个 key。边界用例进单测：`{}`（空标签 → 整键）、`foo{}{bar}`
（→ 整键）、`foo{{bar}}`（→ `{bar`）、`foo{bar}{zap}`（→ `bar`）、`{bar`（→ 整键）。

## 3. 命令表升级（`src/proxy/command_table.{hpp,cpp}`）

M3 已经把 `first_key/last_key/key_step` 三个字段铺好了（当时就是为 M4 留的），M4 补两块。

### 3.1 key spec 加 numkeys 形式

现有 range 形式（1-based argv 下标，`last_key = -1` 表示到最后一个参数）不足以覆盖
`EVAL script numkeys key...` 这类命令。加一个 `numkeys_idx`（存放 key 个数的 argv
下标，0 = 无），**与 range 并存**：`ZUNIONSTORE dest numkeys key...` 就是
range(1,1,1) + numkeys_idx=2。

新增两个纯函数：`extract_keys(msg, info)`（返回 key 视图）与
`route_slot(msg, info)`（返回 `slot` / `crossslot` / `no_keys`）。

### 3.2 `cluster_policy`

`cluster_policy { by_key, any_node, local, unsupported }`，**只在 cluster 模式生效**；
standalone 模式完全走 M3 原有的 `cmd_policy` 路径，行为零变化。

- `by_key`：按 §2 的 slot 路由。多 key 必须同 slot，否则回
  `-CROSSSLOT Keys in request don't hash to the same slot`。
  （MGET/MSET 的跨 slot 拆分聚合按计划留在 backlog。）
- `any_node`：无 key 的管理类命令（INFO/CONFIG/COMMAND/TIME 等），在 master 间轮询
  选一个节点转发。
- `local`：PING/ECHO 由 proxy 本地应答。
- `unsupported`：明确拒绝 `-ERR proxy: <CMD> is not supported in cluster mode`。
  宁可报错也不猜——猜错会把请求发到错误节点，返回**语义上错误但看起来正常**的结果，
  这是最难排查的一类故障。
  - 单节点语义、集群下会静默给出错答案的：SCAN/KEYS/DBSIZE/RANDOMKEY/FLUSHALL/FLUSHDB。
  - key 位置需要解析变参才能确定的：SORT/SORT_RO（`STORE`）、GEORADIUS 系列（`STORE`）、
    XREAD/XREADGROUP（`STREAMS`）。
  - 查表未命中的未知命令（standalone 下仍按 M3 转发，让后端给权威错误）。

补齐的表项：EVAL/EVALSHA/EVAL_RO/EVALSHA_RO/FCALL/FCALL_RO（numkeys@2）、
ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE（range+numkeys@2）、
ZUNION/ZINTER/ZDIFF/SINTERCARD/LMPOP/ZMPOP（numkeys@1）、CLUSTER（reject，见 §6）。

## 4. 拓扑与节点池

分层沿用项目习惯：`cluster/` 放纯逻辑（脱离 IO 可单测），`proxy/` 放 IO。

### 4.1 `cluster::topology`（纯数据 + 纯函数）

`std::vector<node{id, host, port}>` + `std::array<std::int16_t, 16384>` slot→节点索引
（-1 = 未分配，32 KB）。`parse_cluster_shards(const resp::value&)` 从 `CLUSTER SHARDS`
的回复里只取 `role == master && health == online` 的节点（读副本是 backlog），
纯函数，直接复用 M1 的 `resp::parse_tree`（deep parser 当初就是为控制面回复准备的）。

只支持 `CLUSTER SHARDS`，不做 `CLUSTER SLOTS` 回退——验证环境的 redis 7.0.15 支持它，
少一条代码路径。

### 4.2 决策：拓扑每 worker 各自持有、各自刷新，零共享

**这是 M4 唯一一处会诱惑人引入跨线程共享状态的地方，明确不引入。**

备选是「worker 0 刷新 + 不可变 topology + `atomic<shared_ptr>` 原子换指针」，控制面
流量最小。不选它的理由：

- 会打破「worker 之间零共享可变状态」——这条不变量贯穿 M0~M3 全部代码，是 TSan 一路
  零报告的根本原因，为一个每几秒一次的控制面操作破例不划算。
- 热路径每请求都要 load 一次共享指针（或者再加一层 generation 缓存来规避），
  引入跨核引用计数流量。
- MOVED 触发的刷新本来就是 per-worker 事件（只有看到 MOVED 的那个 worker 知道），
  共享模型下反而要额外的跨线程通知。

代价是控制面流量 ×worker 数。可接受：刷新周期以秒计，且**复用已有的数据连接**
（把 `CLUSTER SHARDS` 当普通请求 pipeline 进去），不额外建控制连接。

### 4.3 `proxy::router`（每 worker 一个，`server` 持有）

- 节点池：`"host:port"` → `node_conns{ resolved_addr, vector<shared_ptr<backend_conn>> }`
  （每节点 `conns_per_backend` 条）。按需建、按需 drain。
- `mode { standalone, cluster }`。**standalone 建模成「单节点拥有全部 16384 个 slot
  且从不刷新」的退化情形**，只有一条路由代码路径，不写两套。
  `may_redirect()` 在 standalone 为 false，据此跳过 §5「保留请求字节以备重试」的那次
  拷贝——M3 的性能特征因此不受影响。
- `route(msg, info)` → 目标 `backend_conn*` 或错误；`pick_any_node()` 在 master 间轮询。
- **刷新协程**（每 worker 一个）：周期触发 + MOVED 事件触发（去抖）。实现上直接复用
  `reply_sink` 接口——一个内部 sink 对象，把 `CLUSTER SHARDS` 通过
  `enqueue_forward` 发到某条已连上的数据连接，回复走 `parse_tree`。因此它自动继承了
  M3 的超时、重连、背压体系，不需要任何新机制。
- **回收协程**：节点从拓扑消失 → `begin_drain()` → 移进 `retired_`，等
  `use_count() == 1 && finished()` 两个条件都满足才析构。两个条件缺一不可：
  `finished()` 保证协程帧已退完（M3 的 cancel-before-close 纪律），`use_count()`
  保证没有客户端还持有它（客户端的 `used_conns_` 是 `shared_ptr`）。

### 4.4 启动引导

`--cluster-seeds host:port[,host:port...]`，逐个尝试拉 `CLUSTER SHARDS` 直到成功。
**拓扑就绪前到达的请求立即回 `-ERR proxy: cluster topology unavailable`，不排队等**
——延续 M3「后端不可用时不排队等重连，可预期性优先」的既定做法。

## 5. MOVED / ASK

检查放在 `client_conn` 填槽的位置：重试所需的请求字节就存在槽位里，而 `backend_conn`
保持对路由完全无知（纯传输层，§1）。

- `-MOVED <slot> <host>:<port>`：`router.apply_moved()` 立刻改写该 slot 的指向
  （目标节点不在池里就新建），并触发一次去抖的全量刷新；然后用**同一个 token**
  重新入队到新节点。槽位顺序因此不受重试影响。
- `-ASK <slot> <host>:<port>`：**不**更新拓扑（这是迁移中的临时重定向）。向目标节点
  连续入队两条：`ASKING` 作为 internal entry（`sink == nullptr`，`+OK` 丢弃）+ 原命令。
  单线程执行，两次 push 天然相邻，中间不可能插进别的请求。
- 重定向上限 `--max-redirects`（默认 5，与主流客户端一致）。超限回
  `-ERR proxy: too many redirections`。

### 5.1 实施时定下的两个细节

**重试允许突破 `max_inflight`。** 重定向在 `deliver()` 里发生，而 `deliver()` 跑在
*旧*连接的 driver 回调中，不能挂起——所以目标连接没配额时无法像读路径那样 park 等待。
两个选项：回一个软错误，或者超发。选**超发**：回软错误就等于「resharding 期间客户端
会看到错误」，而这正是 M4 的验收标准要排除的。超发量的上界是当下在飞的请求数（它本身
受源连接的 `max_inflight` 约束），所以是有界的。因此 `enqueue_forward()` 的前置条件从
`available() && has_capacity()` 放宽到只要 `available()`；读路径仍然自己先查
`has_capacity()` 并 park，热路径行为不变。

**空 host 的 MOVED 原样透传。** 节点没有已知地址时 valkey 会回 `-MOVED 1 :6380`，
意思是「还是刚才那个 host」。但 `client_conn` 并不知道是哪个节点回的（`deliver()` 只拿到
token 和帧内容，这是 §1 刻意保持的解耦），无从补全。于是把这种帧判为「不是有效重定向」，
直接把后端的原始错误交给客户端——比猜一个地址诚实。

## 6. proxy 对外形象：始终是 standalone

与现有 HELLO 应答（`mode=standalone`，M3 §5）保持一致：**`CLUSTER` 命令一律拒绝**。
客户端用普通非集群模式连接，proxy 屏蔽全部拓扑细节——这正是 proxy 存在的意义。

考虑过但没做的两个备选：

- 透传后端真实 CLUSTER 拓扑：cluster-aware 客户端会据此直连后端节点、完全绕过 proxy，
  等于自废武功。
- 伪造 CLUSTER SLOTS（全部 16384 个 slot 指向 proxy 自己），让 cluster-aware 客户端库
  也能用：有真实价值，但属于 M4 之外的额外范围，且要处理多 worker 下报哪个端口。
  记入 backlog。

拓扑观测能力留给 M5 的 `/topology` 管理接口。

## 7. 一个需要知情的后果：cluster 模式下的 PING

§3.2 让 PING/ECHO 在 cluster 模式本地应答，意味着 `redis-benchmark -t ping` 打
cluster 模式的 proxy 时只测到 proxy 自身，不含后端往返。standalone 模式保持 M3 行为
（PING 转发给后端），所以 M3 章的性能基线 A 仍然可比。写在这里以免日后误读基线数字。

## 8. 集成测试环境：单容器 6 进程，不用 docker compose

开发计划原文写的是「docker compose 拉起 3 主 3 从」。实际改为
`scripts/cluster-up.sh` 在本机 Linux 或现有验证容器内直接 spawn 6 个 server 进程
（127.0.0.1:7000-7005）+ `redis-cli --cluster create`。理由：

- 本机直连 docker.io 极慢（CLAUDE.md 已记录），compose 要额外拉/编排 6 个容器。
- 容器网络下 cluster 需要处理 announce-ip，macOS 上还多一层 VM 网络，纯粹是调试成本。
- proxy、压测工具、resharding 命令本来就都在同一个验证容器内跑，进程模型够用且更快。

`cluster-down.sh` 负责收尾。resharding 验收用 `redis-cli --cluster reshard`。

## 9. 配置新增（默认值）

| 项 | 默认 | 说明 |
|---|---|---|
| `--cluster-seeds` | 空 | 非空即进 cluster 模式，与 `--backend` 互斥 |
| `--cluster-refresh-ms` | 5000 | 周期性拓扑刷新 |
| `--max-redirects` | 5 | MOVED/ASK 重定向上限 |

## 10. 测试策略

- `cluster/slot`：CRC16 向量（`"123456789"` → `0x31C3`）、hash tag 全部边界。
- `cluster/topology`：喂固定的 CLUSTER SHARDS RESP2/RESP3 报文、畸形输入、
  slot 空洞、replica-only shard。
- `command_table`：numkeys 形式的 key 提取、CROSSSLOT 判定、`cluster_policy` 分类。
- `proxy/cluster_routing_test`（假 cluster：多个可编排的假后端，能返回 CLUSTER SHARDS
  与 MOVED/ASK）：
  - 按 slot 选中正确节点；跨 slot 多 key 回 CROSSSLOT。
  - MOVED → 重试成功，且拓扑已更新（同 key 的第二次请求直达新节点）。
  - ASK → 目标节点收到 `ASKING` + 原命令两帧，拓扑未变。
  - 重定向死循环 → 在上限处封顶报错。
  - **B 节点先于 A 节点回复时，客户端出向字节顺序仍与请求顺序一致**（§1 的核心用例）。
  - 节点从拓扑消失 → drain 且不悬垂；刷新协程感知拓扑变化。
  - cluster 模式拒绝集：CLUSTER / SCAN / 未知命令。
- 真集群（容器内）：功能面 + **压测中 resharding 客户端零错误**（M4 验收标准）
  + TSan 压测零报告。

## 附：§1 改造的性能门槛复核

有序回复槽位落地后（commit `1d88b53`），在 M3 同一容器、同一命令行复跑基线 A
（`redis-benchmark -c 50 -d 32 --threads 4`，`-P 1` n=100 万、`-P 16` n=500 万），
proxy 侧数字与 M3 记录一致（延迟单位毫秒）：

| 配置 | M3 rps / p50 / p95 / p99 | 本次 |
|---|---|---|
| io_uring w4 c1 SET P=1 | 307,409 / 0.135 / 0.239 / 0.303 | 307,503 / 0.135 / 0.247 / 0.319 |
| io_uring w4 c1 GET P=1 | 307,503 / 0.135 / 0.239 / 0.303 | 307,409 / 0.135 / 0.239 / 0.295 |
| io_uring w4 c1 SET P=16 | 2,498,751 / 0.263 / 0.479 / 0.591 | 2,498,751 / 0.263 / 0.479 / 0.583 |
| io_uring w4 c1 GET P=16 | 2,853,881 / 0.239 / 0.439 / 0.543 | 2,855,511 / 0.239 / 0.439 / 0.551 |
| epoll w4 c1 SET P=16 | 1,998,401 / 0.327 / 0.591 / 0.719 | 2,219,263 / 0.319 / 0.583 / 0.727 |
| epoll w4 c1 GET P=16 | 2,220,248 / 0.295 / 0.535 / 0.655 | 2,220,248 / 0.295 / 0.535 / 0.671 |

无回归：`deque` 槽位在单后端下队头恒被立即填充，走的还是「直接 append、不落中间
string」那条路径。

**注意直连对照组这次不可用**：同一轮里直连 `-P 16` 在 2.0M 与 3.33M 之间双峰跳变
（单线程 redis-server 的核放置随机），M3 记录的 2.86M/3.33M 属于其中的「好」模式。
所以本次只做 proxy-对-proxy 的纵向对比；直连比值留到 M6 优化专章再在稳定环境下测。
