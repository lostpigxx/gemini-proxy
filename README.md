# valkey-proxy

自研的高性能、高可维护 valkey proxy。C++23，shared-nothing 线程模型，
io_uring（epoll fallback）+ 自研 C++20 无栈协程层。

- 架构决策与调研：[docs/architecture-decisions.md](docs/architecture-decisions.md)
- 开发计划（M0~M6）：[docs/development-plan.md](docs/development-plan.md)

## 构建

要求：CMake ≥ 3.28、Ninja、C++23 编译器（Clang 18+ / GCC 14+ / Apple Clang 15+）；
Linux 上另需 `liburing-dev`（≥ 2.5）。

```sh
cmake --preset debug        # 其他预设：release / asan-ubsan / tsan
cmake --build --preset debug
ctest --preset debug
./build/debug/src/proxyd --version
```

依赖由 [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) 在配置阶段自动拉取并锁定版本；
设置 `CPM_SOURCE_CACHE` 环境变量可在多个构建目录间共享依赖源码。

## 运行

```sh
proxyd -l 127.0.0.1:6380 -b 127.0.0.1:6379 -w 4        # 单机后端
proxyd --cluster-seeds 127.0.0.1:7001,127.0.0.1:7002   # cluster 模式
proxyd --config config/proxy.toml                      # 配置文件
```

配置的优先级是「默认值 → TOML 文件 → 命令行显式给出的选项」，生效配置在启动时打印。
全部字段与说明见 [config/proxy.toml](config/proxy.toml)；`proxyd --help` 列出命令行侧。

**管理端口**（默认 `127.0.0.1:9180`，`--admin-listen ""` 关闭）：

| 路径 | 内容 |
|---|---|
| `/metrics` | Prometheus 文本格式（0.0.4）。QPS、两条延迟直方图、连接与后端池状态、错误与重定向计数 |
| `/health` | `200 {"status":"ok"}`；收到 SIGTERM 进入 drain 后立刻转 `503 {"status":"draining"}`，供 LB 先摘流量 |
| `/topology` | 当前 slot→节点映射，以及每 worker 一行的快照摘要（worker 间不一致时一眼可见） |

Grafana 面板：[deploy/grafana-dashboard.json](deploy/grafana-dashboard.json)（导入即可，
数据源在面板顶部选）。JSON 由 `deploy/gen-dashboard.py` 生成，改面板请改生成器。

**优雅关闭**：SIGTERM 后停止 accept，处在请求边界的连接立即以干净 FIN 关闭，有在途请求的
等它们收到回复再关；`--shutdown-grace-ms`（默认 5000）是兜底硬停。实测 32 连接满载下
drain 在毫秒级完成且零客户端错误，判据见 `scripts/cluster-loadcheck.py --expect-close`。

## 性能

测试环境：Linux arm64 容器（OrbStack 18 vCPU，clang-18 Release + mimalloc），后端为同机
redis 7.0.15（单机 1 核 / 集群 3 主各 1 核，proxy 4 worker 固 4 核，压测端固 8 核），
keyspace 1M 预先填满（GET 命中率 ≥98%），wall-clock rps 与 `schedstat` CPU µs/请求取 5 轮中位数。
完整方法论与 M6 逐项优化 A/B 报告见 [docs/design/m6-performance.md](docs/design/m6-performance.md)。

| 场景（`-d 32`） | 直连 rps | proxy rps | 直连 p99 | proxy p99 | proxy CPU µs/req | 较 M5 CPU 降幅 |
|---|---|---|---|---|---|---|
| 单机 SET `P=1 c=200` | 452,857 | 584,016 | 0.663 ms | 0.511 ms | 5.14 | −5.2% |
| 单机 GET `P=1 c=200` | 453,149 | 584,815 | 0.671 ms | 0.487 ms | 5.10 | −5.5% |
| 单机 SET `P=16 c=50` | 1,425,030 | 1,535,870 | 0.663 ms | 0.599 ms | 0.351 | −22.7% |
| 单机 GET `P=16 c=50` | 1,535,310 | 1,663,863 | 0.623 ms | 0.567 ms | 0.352 | −18.1% |
| 集群 SET `P=16 c=50`（3 主） | — | 2,494,705 | — | 0.447 ms | 0.908 | −11.6% |
| 集群 GET `P=16 c=50`（3 主） | — | 2,494,797 | — | 0.423 ms | 0.906 | −10.3% |

- 单机模式借助连接汇聚与双缓冲写合并，吞吐与 p99 均优于直连；`epoll` fallback 与 `io_uring`
  在 `-P 16` 下 CPU/req 差距约 3~6%。
- 集群 CPU/req 高于单机主因是同批请求拆到 3 条后端连接后 TCP 分段更小；**同机环回会把接收端
  TCP 协议栈开销也记在 proxy 进程上**，此处数字是环回工况上界，真实跨机部署差距更小。
- RESP 浅解析器微基准（`vkp_resp_bench`）：`GET` 3.81 GB/s、`SET 64B` 8.85 GB/s、
  `MGET 100` 8.31 GB/s、`1KiB bulk` 234.6 GB/s。

## 开发

```sh
./scripts/format.sh          # 格式化全部源码（--check 仅校验）
```

提交信息使用 conventional commits；新代码必须带测试。详见开发计划中的「工作约定」。
