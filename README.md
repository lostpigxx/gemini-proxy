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

## 开发

```sh
./scripts/format.sh          # 格式化全部源码（--check 仅校验）
```

提交信息使用 conventional commits；新代码必须带测试。详见开发计划中的「工作约定」。
