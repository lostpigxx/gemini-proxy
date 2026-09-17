#!/usr/bin/env python3
"""Generates deploy/grafana-dashboard.json.

Hand-writing Grafana panel JSON is 90% boilerplate; this keeps the parts that
matter (title, queries, units) readable and in one place. Re-run after changing
a panel, do not edit the JSON by hand.
"""
import json

SEL = '{job=~"$job", instance=~"$instance"}'
RI = "[$__rate_interval]"
_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


def target(expr, legend):
    return {
        "datasource": {"type": "prometheus", "uid": "${datasource}"},
        "editorMode": "code",
        "expr": expr,
        "legendFormat": legend,
        "range": True,
        "refId": chr(ord("A") + target.n % 26),
    }


target.n = 0


def targets(pairs):
    out = []
    for i, (expr, legend) in enumerate(pairs):
        target.n = i
        out.append(target(expr, legend))
    return out


def ts(title, gp, pairs, unit="short", desc="", stack=False, minv=0):
    return {
        "type": "timeseries",
        "id": nid(),
        "title": title,
        "description": desc,
        "datasource": {"type": "prometheus", "uid": "${datasource}"},
        "gridPos": gp,
        "targets": targets(pairs),
        "options": {
            "legend": {"displayMode": "table", "placement": "bottom",
                       "calcs": ["mean", "max"], "showLegend": True},
            "tooltip": {"mode": "multi", "sort": "desc"},
        },
        "fieldConfig": {
            "defaults": {
                "unit": unit,
                "min": minv,
                "custom": {
                    "drawStyle": "line",
                    "lineWidth": 1,
                    "fillOpacity": 15 if stack else 0,
                    "showPoints": "never",
                    "stacking": {"mode": "normal" if stack else "none",
                                 "group": "A"},
                },
            },
            "overrides": [],
        },
    }


def stat(title, gp, pairs, unit="short", desc="", text_mode="auto", thresholds=None):
    return {
        "type": "stat",
        "id": nid(),
        "title": title,
        "description": desc,
        "datasource": {"type": "prometheus", "uid": "${datasource}"},
        "gridPos": gp,
        "targets": targets(pairs),
        "options": {
            "colorMode": "value",
            "graphMode": "area",
            "textMode": text_mode,
            "reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
        },
        "fieldConfig": {
            "defaults": {
                "unit": unit,
                "thresholds": {
                    "mode": "absolute",
                    "steps": thresholds or [{"color": "text", "value": None}],
                },
            },
            "overrides": [],
        },
    }


def row(title, y):
    return {"type": "row", "id": nid(), "title": title, "collapsed": False,
            "gridPos": {"h": 1, "w": 24, "x": 0, "y": y}, "panels": []}


def quantiles(metric):
    return [
        ('histogram_quantile(%s, sum by (le) (rate(%s_bucket%s%s)))' % (q, metric, SEL, RI),
         "p%s" % lbl)
        for q, lbl in (("0.50", "50"), ("0.90", "90"), ("0.99", "99"))
    ] + [
        ("sum(rate(%s_sum%s%s)) / sum(rate(%s_count%s%s))" % (metric, SEL, RI, metric, SEL, RI),
         "avg")
    ]


panels = []
y = 0

panels.append(row("概览", y)); y += 1
panels.append(stat("请求速率", {"h": 4, "w": 4, "x": 0, "y": y},
                   [("sum(rate(vkp_requests_total%s%s))" % (SEL, RI), "rps")], "reqps"))
panels.append(stat("客户端连接", {"h": 4, "w": 4, "x": 4, "y": y},
                   [("sum(vkp_client_connections%s)" % SEL, "conns")]))
panels.append(stat("非 ok 回复速率", {"h": 4, "w": 4, "x": 8, "y": y},
                   [('sum(rate(vkp_responses_total{job=~"$job", instance=~"$instance", '
                     'outcome!="ok"}%s))' % RI, "rps")], "reqps",
                   desc="timeout / unavailable / proxy 生成的错误。稳态应当恒为 0。",
                   thresholds=[{"color": "green", "value": None},
                               {"color": "red", "value": 0.001}]))
panels.append(stat("p99 延迟（proxy 侧）", {"h": 4, "w": 4, "x": 12, "y": y},
                   [('histogram_quantile(0.99, sum by (le) '
                     '(rate(vkp_request_duration_seconds_bucket%s%s)))' % (SEL, RI), "p99")], "s"))
panels.append(stat("运行时长", {"h": 4, "w": 4, "x": 16, "y": y},
                   [("max(vkp_uptime_seconds%s)" % SEL, "uptime")], "s"))
panels.append(stat("版本", {"h": 4, "w": 4, "x": 20, "y": y},
                   [("vkp_build_info%s" % SEL, "{{version}}")], text_mode="name"))
y += 4

panels.append(row("吞吐", y)); y += 1
panels.append(ts("请求速率（按命令类别）", {"h": 8, "w": 12, "x": 0, "y": y},
                 [("sum by (class) (rate(vkp_requests_total%s%s))" % (SEL, RI), "{{class}}")],
                 "reqps", stack=True,
                 desc="class 由命令表静态标注：read / write / admin / connection / other。"))
panels.append(ts("回复速率（按结果）", {"h": 8, "w": 12, "x": 12, "y": y},
                 [("sum by (outcome) (rate(vkp_responses_total%s%s))" % (SEL, RI),
                   "{{outcome}}")], "reqps", stack=True))
y += 8

panels.append(row("延迟", y)); y += 1
panels.append(ts("proxy 侧延迟（读到帧 → 填满槽位）", {"h": 8, "w": 12, "x": 0, "y": y},
                 quantiles("vkp_request_duration_seconds"), "s",
                 desc="客户端实际感受到的服务时间，含排队与保序等待。"))
panels.append(ts("后端侧延迟（入队 → 配对回复）", {"h": 8, "w": 12, "x": 12, "y": y},
                 quantiles("vkp_backend_duration_seconds"), "s",
                 desc="两条曲线的差值就是 proxy 自己加上去的那部分。"))
y += 8

panels.append(row("连接与流量", y)); y += 1
panels.append(ts("客户端连接", {"h": 8, "w": 8, "x": 0, "y": y},
                 [("sum(vkp_client_connections%s)" % SEL, "当前"),
                  ("sum(rate(vkp_client_connections_total%s%s))" % (SEL, RI), "新建/s")]))
panels.append(ts("后端连接池", {"h": 8, "w": 8, "x": 8, "y": y},
                 [("sum by (state) (vkp_backend_connections%s)" % SEL, "{{state}}"),
                  ("sum(vkp_backend_inflight%s)" % SEL, "在途请求")],
                 desc="connecting / down 持续不为 0 就是后端有问题。"))
panels.append(ts("客户端字节流", {"h": 8, "w": 8, "x": 16, "y": y},
                 [("sum by (dir) (rate(vkp_client_bytes_total%s%s))" % (SEL, RI), "{{dir}}")],
                 "Bps"))
y += 8

panels.append(row("路由与错误", y)); y += 1
panels.append(ts("错误（按原因）", {"h": 8, "w": 8, "x": 0, "y": y},
                 [("sum by (kind) (rate(vkp_errors_total%s%s))" % (SEL, RI), "{{kind}}")],
                 "reqps", stack=True,
                 desc="proxy 自己生成的错误回复，不含后端返回的错误。"))
panels.append(ts("重定向", {"h": 8, "w": 8, "x": 8, "y": y},
                 [("sum by (kind) (rate(vkp_redirects_total%s%s))" % (SEL, RI), "{{kind}}")],
                 "reqps",
                 desc="moved 持续偏高说明本地拓扑落后；ask 只在 resharding 期间出现。"))
panels.append(ts("拓扑", {"h": 8, "w": 8, "x": 16, "y": y},
                 [("max(vkp_topology_nodes%s)" % SEL, "节点数"),
                  ("max(vkp_topology_slots_assigned%s)" % SEL, "已分配 slot"),
                  ('sum(rate(vkp_topology_refresh_total{job=~"$job", instance=~"$instance", '
                   'result="fail"}%s))' % RI, "刷新失败/s")],
                 desc="已分配 slot 低于 16384 就意味着有 key 空间无人负责。"))
y += 8

panels.append(row("worker 均衡", y)); y += 1
panels.append(ts("每 worker 请求速率", {"h": 8, "w": 12, "x": 0, "y": y},
                 [("sum by (worker) (rate(vkp_worker_requests_total%s%s))" % (SEL, RI),
                   "worker {{worker}}")], "reqps",
                 desc="SO_REUSEPORT 由内核分配连接，长期倾斜说明某些连接远比其他忙。"))
panels.append(ts("每 worker 客户端连接", {"h": 8, "w": 12, "x": 12, "y": y},
                 [("sum by (worker) (vkp_worker_client_connections%s)" % SEL,
                   "worker {{worker}}")]))

dashboard = {
    "title": "valkey-proxy",
    "uid": "valkey-proxy",
    "description": "自研 valkey proxy 的核心运维面板。数据源：proxyd 的 "
                   "--admin-listen 端口上的 /metrics。",
    "tags": ["valkey", "proxy"],
    "timezone": "browser",
    "editable": True,
    "schemaVersion": 39,
    "version": 1,
    "refresh": "10s",
    "time": {"from": "now-1h", "to": "now"},
    "templating": {
        "list": [
            {"type": "datasource", "name": "datasource", "label": "数据源",
             "query": "prometheus", "current": {}, "hide": 0},
            {"type": "query", "name": "job", "label": "job",
             "datasource": {"type": "prometheus", "uid": "${datasource}"},
             "query": "label_values(vkp_build_info, job)",
             "refresh": 1, "includeAll": True, "multi": True, "current": {}},
            {"type": "query", "name": "instance", "label": "实例",
             "datasource": {"type": "prometheus", "uid": "${datasource}"},
             "query": 'label_values(vkp_build_info{job=~"$job"}, instance)',
             "refresh": 1, "includeAll": True, "multi": True, "current": {}},
        ]
    },
    "panels": panels,
}

with open(__file__.rsplit("/", 1)[0] + "/grafana-dashboard.json", "w") as f:
    json.dump(dashboard, f, indent=2, ensure_ascii=False)
    f.write("\n")
print("panels: %d" % len([p for p in panels if p["type"] != "row"]))
