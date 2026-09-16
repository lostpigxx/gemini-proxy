# fuzz/

libFuzzer 目标目录，见[开发计划](../docs/development-plan.md)。

| 目标 | 被测对象 | 引入于 |
|---|---|---|
| `resp_parser_fuzz.cpp` | `resp::parser` | M1 |
| `admin_http_fuzz.cpp` | `admin::parse_request`（管理接口请求行） | M5 |

语料**按目标分目录**：`corpus/<target>/`。跑法见根 [CLAUDE.md](../CLAUDE.md)：

```sh
./scripts/fuzz.sh              # 全部目标各跑 60 秒
./scripts/fuzz.sh 300          # 全部目标各跑 300 秒
./scripts/fuzz.sh 300 admin_http   # 只跑一个
```

新语料由 libFuzzer 直接写回对应的 `corpus/<target>/`，定期用 `-merge=1` 精简后提交。
