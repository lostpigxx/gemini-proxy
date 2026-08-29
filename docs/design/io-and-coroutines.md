# M2 设计记录：完成式 IO 抽象与自研协程层

> 日期：2026-08-29
> 状态：定稿（M2 实施依据）
> 前置：[架构决策调研报告](../architecture-decisions.md) 决策一、决策二

## 1. 分层

```
proxy 逻辑（连接协程）
  └─ co_await event_loop 提供的 awaitable（async_accept / async_recv / ...）
       └─ operation（POD 完成对象，嵌在协程帧里）
            └─ backend（io_uring | epoll | kqueue，编译期裁剪 + 运行时探测）
```

- **`vkp::io::task<T>`**（`src/io/task.hpp`）：lazy 无栈协程。symmetric transfer 续接，
  异常存 `exception_ptr` 在 `await_resume` 重抛。协程帧经 `promise_type::operator new`
  走 thread-local freelist（`frame_pool.hpp`），不赌 HALO。
- **`vkp::io::operation`**（`operation.hpp`）：一次 IO 的全部状态（opcode、fd、buffer、
  结果、continuation、侵入式链表指针）。**生命周期 = awaiter 生命周期 = 协程帧的一部分**，
  提交后到完成前地址稳定（协程帧不会移动），因此 backend 可以裸存指针，io_uring 直接把
  `&op` 塞进 `user_data`，零额外分配。
- **`vkp::io::event_loop`**（`event_loop.hpp/.cpp`）：单线程。拥有 ready 队列（已完成
  待恢复的 op）、定时器最小堆、backend。`run()` 循环：恢复 ready → 到期定时器 →
  计算最近 deadline → `backend::poll(timeout)`。**ready 队列、定时器堆与 backend 无关**，
  三个 backend 共享同一套；`sleep_for` 纯用户态定时器实现，不占 backend 能力。
- **backend 接口**（`backend.hpp`）：`submit(operation&)` / `cancel(operation&)` /
  `poll(timeout, ready_list&)` 三个虚函数。虚调用开销相对一次 syscall 可忽略。

## 2. 完成语义与结果约定

所有操作完成时携带 `std::int32_t result`：`>= 0` 为字节数（accept 为新 fd，connect 为 0），
`< 0` 为 `-errno`。与 io_uring CQE 的 `res` 约定完全一致——reactor 后端向此约定看齐，
而不是反过来（架构决策 1.3 的教训）。

- `async_recv` 返回 0 表示对端关闭（与 recv(2) 一致）。
- `async_send` 允许**部分写**（返回实际写入字节数）；`send_all` 协程助手负责循环写完。
- reactor 后端在就绪后执行非阻塞 syscall，若得 EAGAIN（伪就绪）则重新挂回兴趣集，
  不向上层暴露。

## 3. 三个 backend

| | 提交 | 完成 | 备注 |
|---|---|---|---|
| io_uring | op → SQE，`user_data=&op` | CQE `res` 直接就是约定格式 | M2 用单发 op；multishot recv **依赖 provided buffer ring**（`IORING_RECV_MULTISHOT` 必须 `IOSQE_BUFFER_SELECT`），计划里"multishot recv M2 / buffer ring M6"不可拆，两者一并挪到 M6；multishot accept 同批评估（偏差已记入开发计划） |
| epoll | 每 fd 一个 `{reader op, writer op}` 槽位，`EPOLL_CTL_ADD/MOD` 合成兴趣掩码 | 就绪 → syscall → 填 result | fd 粒度注册，读写两方向共用一个 epoll 表项 |
| kqueue | `(ident, filter)` 粒度，`EV_ADD\|EV_ONESHOT`，`udata=&op` | 同 epoll | 仅保证 macOS 开发可跑 |

每 fd 每方向**同一时刻最多一个在途 op**（一条连接一个读协程一个写协程，天然满足）；
backend 以此为契约，违反即 assert。

运行时选择（`make_backend()`）：Linux 上先试 `io_uring_queue_init`，失败（seccomp/
`io_uring_disabled`/内核过旧）自动降级 epoll；`--io-backend=uring|epoll|kqueue` 强制指定。
macOS 只有 kqueue。

## 4. 取消与超时（M2 骨架）

- `event_loop::cancel(operation&)`：未完成则从 backend 摘除并以 `-ECANCELED` 完成（进
  ready 队列，不同步恢复）。io_uring 走 `IORING_OP_ASYNC_CANCEL`；reactor 直接从槽位摘除。
- M2 只用取消实现优雅关闭（撤销挂起的 accept/recv）；per-op 超时（uring LINK_TIMEOUT、
  reactor 定时器取消）与 `race(op, timeout)` 组合子按计划归 M3「超时体系」。
- 信号：SIGTERM/SIGINT 用 self-pipe（信号处理函数只写一字节），事件循环对 pipe 读端挂
  常规 `async_recv`——三个 backend 零特殊代码。

## 5. 最小 proxy（M2 形态）

单线程单后端整条透传。每客户端连接一个协程：

```
client_loop:
  read_buffer ← async_recv
  resp::parser 浅解析出完整帧（只要边界，不看内容）
  async_send 原样转发到后端连接
  后端侧对称：收响应帧 → 回写客户端
```

M2 简化：请求-响应严格串行（一来一回），不做 pipelining/配对队列（M3）。后端连接
每客户端一条（M3 换 per-thread 连接池）。这保证 FIFO 正确性的同时把 M2 的验收面
压在 IO 抽象与协程层上。

优雅关闭：SIGTERM → 取消 accept op → 等在途连接自然结束（计数归零）或 5s 超时强关。

## 6. 测试策略

- 协程层：无 IO 单测（返回值/异常传播/嵌套 co_await/symmetric transfer 深链不爆栈/
  帧池复用）。
- IO 层：loopback socketpair/TCP echo 单测，覆盖 recv/send/accept/connect/sleep_for/
  cancel；macOS 跑 kqueue，Linux 容器跑 epoll 与 io_uring 同一套测试（backend 参数化）。
- proxy：`redis-server`（本机）/valkey 容器 + `valkey-cli`/`redis-cli` 功能脚本
  （PING/SET/GET/MGET）+ `valkey-benchmark -c 1`；ASan/TSan 下重跑。
