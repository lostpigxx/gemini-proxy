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

## 开发

```sh
./scripts/format.sh          # 格式化全部源码（--check 仅校验）
```

提交信息使用 conventional commits；新代码必须带测试。详见开发计划中的「工作约定」。
