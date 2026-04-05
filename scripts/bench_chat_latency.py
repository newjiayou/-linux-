#!/usr/bin/env python3
import argparse
import concurrent.futures
import csv
import json
import socket
import struct
import time
from typing import Dict, List, Optional


def pack_packet(msg_type: int, body: str) -> bytes:
    b = body.encode("utf-8")
    return struct.pack("!I", 6 + len(b)) + struct.pack("!H", msg_type) + b


def percentile(values: List[float], p: float) -> float:
    if not values:
        return 0.0
    s = sorted(values)
    i = int((len(s) - 1) * p)
    return s[i]


def parse_int_list(text: str) -> List[int]:
    return [int(x.strip()) for x in text.split(",") if x.strip()]


def parse_float_list(text: str) -> List[float]:
    return [float(x.strip()) for x in text.split(",") if x.strip()]


def pick_private_target_id(cid: int, total_clients: int, sender_prefix: str) -> str:
    # 成对私发：1<->2, 3<->4 ...
    # 若总数为奇数，最后一个客户端与前一个配对
    if total_clients <= 1:
        buddy = cid
    elif cid % 2 == 1:
        buddy = cid + 1 if cid + 1 <= total_clients else cid - 1
    else:
        buddy = cid - 1
    return f"{sender_prefix}_{buddy}"


def run_single_client(
    host: str,
    port: int,
    messages: int,
    qps: float,
    timeout_ms: int,
    sender: str,
    cid: int,
    mode: str,
    total_clients: int,
    bind_wait_ms: int,
):
    lat: List[float] = []
    sent = recv_ok = err = 0

    sender_id = f"{sender}_{cid}"

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(0.02)
        sock.connect((host, port))
    except Exception:
        return {"lat": [], "sent": 0, "recv": 0, "err": messages}

    # 先做身份绑定（msgType=3），确保私发可命中在线映射
    try:
        bind_body = json.dumps({"sender": sender_id}, separators=(",", ":"), ensure_ascii=False)
        sock.sendall(pack_packet(3, bind_body))
    except Exception:
        err += 1

    if mode == "private" and bind_wait_ms > 0:
        time.sleep(bind_wait_ms / 1000.0)

    timeout_ns = timeout_ms * 1_000_000
    send_interval = (1.0 / qps) if qps > 0 else 0.0
    next_send = time.perf_counter()
    buf = bytearray()
    outstanding: Dict[int, int] = {}
    prefix = f"bench_c{cid}_seq_"
    disconnected = False

    private_target = pick_private_target_id(cid, total_clients, sender)

    while True:
        now = time.perf_counter()

        while sent < messages and now >= next_send:
            seq = sent + 1
            target = "broadcast" if mode == "broadcast" else private_target
            body = json.dumps(
                {
                    "sender": sender_id,
                    "target": target,
                    "message": f"{prefix}{seq}",
                },
                separators=(",", ":"),
                ensure_ascii=False,
            )
            try:
                sock.sendall(pack_packet(1, body))
                outstanding[seq] = time.perf_counter_ns()
                sent += 1
            except Exception:
                err += 1

            next_send = (next_send + send_interval) if send_interval > 0 else now
            now = time.perf_counter()

        try:
            data = sock.recv(65536)
            if not data:
                disconnected = True
            else:
                buf.extend(data)
        except socket.timeout:
            pass
        except Exception:
            err += 1

        while len(buf) >= 6:
            total_len = struct.unpack("!I", buf[0:4])[0]
            if total_len < 6 or total_len > 10 * 1024 * 1024:
                err += 1
                disconnected = True
                break
            if len(buf) < total_len:
                break

            msg_type = struct.unpack("!H", buf[4:6])[0]
            body_bytes = bytes(buf[6:total_len])
            del buf[:total_len]
            if msg_type != 1:
                continue

            try:
                obj = json.loads(body_bytes.decode("utf-8", errors="ignore"))
                m = obj.get("message", "")
                if isinstance(m, str) and m.startswith(prefix):
                    seq = int(m[len(prefix):])
                    t0 = outstanding.pop(seq, None)
                    if t0 is not None:
                        lat.append((time.perf_counter_ns() - t0) / 1_000_000.0)
                        recv_ok += 1
            except Exception:
                err += 1

        now_ns = time.perf_counter_ns()
        timed_out = [k for k, t0 in outstanding.items() if now_ns - t0 > timeout_ns]
        for k in timed_out:
            outstanding.pop(k, None)
            err += 1

        if disconnected:
            err += len(outstanding)
            outstanding.clear()
            break
        if sent >= messages and not outstanding:
            break

    try:
        sock.close()
    except Exception:
        pass

    return {"lat": lat, "sent": sent, "recv": recv_ok, "err": err}


def run_benchmark(host: str, port: int, messages: int, clients: int, qps: float, timeout_ms: int, sender: str, mode: str, bind_wait_ms: int):
    t0 = time.perf_counter()
    all_lat: List[float] = []
    sent = recv = err = 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as ex:
        futs = [
            ex.submit(run_single_client, host, port, messages, qps, timeout_ms, sender, i + 1, mode, clients, bind_wait_ms)
            for i in range(clients)
        ]
        for f in concurrent.futures.as_completed(futs):
            r = f.result()
            all_lat.extend(r["lat"])
            sent += r["sent"]
            recv += r["recv"]
            err += r["err"]

    dt = max(time.perf_counter() - t0, 1e-9)
    avg = (sum(all_lat) / len(all_lat)) if all_lat else 0.0
    p95 = percentile(all_lat, 0.95)
    p99 = percentile(all_lat, 0.99)
    err_rate = (err / sent) if sent > 0 else 1.0
    in_msgps = sent / dt

    # 估算出站：
    # - broadcast：每条入站约发给 clients 个连接
    # - private：当前服务端逻辑会回给 target + sender，约 2 倍
    out_multiplier = clients if mode == "broadcast" else 2
    out_msgps_est = in_msgps * out_multiplier

    return {
        "mode": mode,
        "clients": clients,
        "qps": qps,
        "messages": messages,
        "planned": clients * messages,
        "sent": sent,
        "recv": recv,
        "err": err,
        "err_rate": err_rate,
        "duration": dt,
        "avg": avg,
        "p95": p95,
        "p99": p99,
        "in_msgps": in_msgps,
        "out_msgps_est": out_msgps_est,
    }


def print_result(r):
    print("\n===== Chat Latency Benchmark Result =====")
    print(f"Mode                  : {r['mode']}")
    print(f"Clients               : {r['clients']}")
    print(f"QPS per client        : {r['qps']}")
    print(f"Messages per client   : {r['messages']}")
    print(f"Messages planned total: {r['planned']}")
    print(f"Messages sent         : {r['sent']}")
    print(f"Messages recv         : {r['recv']}")
    print(f"Errors                : {r['err']}")
    print(f"Error rate            : {r['err_rate'] * 100:.2f}%")
    print(f"Duration (s)          : {r['duration']:.3f}")
    print(f"Avg Latency (ms)      : {r['avg']:.3f}")
    print(f"P95 Latency (ms)      : {r['p95']:.3f}")
    print(f"P99 Latency (ms)      : {r['p99']:.3f}")
    print(f"In throughput (msg/s) : {r['in_msgps']:.2f}")
    print(f"Out throughput est    : {r['out_msgps_est']:.2f} msg/s")


def pick_capacity(rows: List[dict], max_err: float, max_p99: float) -> Optional[dict]:
    ok = [r for r in rows if r["err_rate"] <= max_err and r["p99"] <= max_p99]
    if not ok:
        return None
    ok.sort(key=lambda x: x["in_msgps"])
    return ok[-1]


def write_rows_csv(path: str, rows: List[dict]):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["mode", "clients", "qps", "messages", "planned", "sent", "recv", "err", "err_rate", "duration", "avg", "p95", "p99", "in_msgps", "out_msgps_est"])
        for r in rows:
            w.writerow([
                r["mode"], r["clients"], r["qps"], r["messages"], r["planned"], r["sent"], r["recv"], r["err"],
                f"{r['err_rate']:.6f}", f"{r['duration']:.6f}", f"{r['avg']:.6f}", f"{r['p95']:.6f}", f"{r['p99']:.6f}",
                f"{r['in_msgps']:.6f}", f"{r['out_msgps_est']:.6f}"
            ])


def run_sweep(args):
    clients_list = parse_int_list(args.sweep_clients)
    qps_list = parse_float_list(args.sweep_qps)
    rows: List[dict] = []

    print("\n===== Sweep Start =====")
    for c in clients_list:
        for q in qps_list:
            print(f"\n[RUN] mode={args.mode}, clients={c}, qps_per_client={q}, messages_per_client={args.messages}")
            r = run_benchmark(args.host, args.port, args.messages, c, q, args.timeout_ms, args.sender, args.mode, args.bind_wait_ms)
            print_result(r)
            rows.append(r)

    write_rows_csv(args.csv, rows)

    cap = pick_capacity(rows, args.max_error_rate, args.max_p99_ms)
    print("\n===== Sweep Summary =====")
    print(f"CSV saved: {args.csv}")
    print(f"Mode: {args.mode}")
    print(f"SLO: error_rate <= {args.max_error_rate * 100:.2f}%, p99 <= {args.max_p99_ms:.2f} ms")
    if cap is None:
        print("没有测试点满足 SLO，请降低并发或 qps 再试。")
        return

    print("\n[Estimated Capacity Under SLO]")
    print(f"Concurrency capability (clients): {cap['clients']}")
    print(f"IO limit in (msg/s)            : {cap['in_msgps']:.2f}")
    print(f"IO limit out est (msg/s)       : {cap['out_msgps_est']:.2f}")
    print(f"At point                       : clients={cap['clients']}, qps_per_client={cap['qps']}")
    print(f"error_rate={cap['err_rate'] * 100:.2f}%, p99={cap['p99']:.2f}ms")


def run_capacity_quick(args):
    # 快速模式：固定 qps，只扫并发（clients），遇到第一个不满足 SLO 的点就停止
    clients_list = parse_int_list(args.sweep_clients)
    rows: List[dict] = []
    best: Optional[dict] = None

    print("\n===== Quick Capacity Start =====")
    print(f"Mode={args.mode}, fixed_qps_per_client={args.qps}, messages_per_client={args.messages}")
    print(f"SLO: error_rate <= {args.max_error_rate * 100:.2f}%, p99 <= {args.max_p99_ms:.2f} ms")

    for c in clients_list:
        print(f"\n[RUN] mode={args.mode}, clients={c}, qps_per_client={args.qps}, messages_per_client={args.messages}")
        r = run_benchmark(args.host, args.port, args.messages, c, args.qps, args.timeout_ms, args.sender, args.mode, args.bind_wait_ms)
        print_result(r)
        rows.append(r)

        if r["err_rate"] <= args.max_error_rate and r["p99"] <= args.max_p99_ms:
            best = r
        else:
            print("\n[STOP] 当前并发不满足 SLO，快速模式提前停止。")
            break

    write_rows_csv(args.csv, rows)

    print("\n===== Quick Capacity Summary =====")
    print(f"CSV saved: {args.csv}")
    if best is None:
        print("没有任何并发点满足 SLO。")
        return

    print("\n[Estimated Max Concurrency Under SLO]")
    print(f"Concurrency capability (clients): {best['clients']}")
    print(f"At fixed qps/client            : {best['qps']}")
    print(f"IO limit in (msg/s)            : {best['in_msgps']:.2f}")
    print(f"IO limit out est (msg/s)       : {best['out_msgps_est']:.2f}")
    print(f"error_rate={best['err_rate'] * 100:.2f}%, p99={best['p99']:.2f}ms")


def main():
    p = argparse.ArgumentParser(description="聊天延迟/吞吐压测，支持 sweep 自动估算并发能力与IO上限")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=12345)
    p.add_argument("-n", "--messages", type=int, default=200)
    p.add_argument("--clients", type=int, default=1)
    p.add_argument("--qps", type=float, default=50.0)
    p.add_argument("--timeout-ms", type=int, default=3000)
    p.add_argument("--sender", default="bench_user")

    p.add_argument("--mode", choices=["broadcast", "private"], default="broadcast", help="测广播或私发")
    p.add_argument("--bind-wait-ms", type=int, default=300, help="私发模式下，身份绑定后的等待时间(ms)")

    p.add_argument("--sweep", action="store_true")
    p.add_argument("--quick-capacity", action="store_true", help="快速测并发上限：固定qps，只扫clients，首个失败即停")
    p.add_argument("--sweep-clients", default="20,50,100,150,200")
    p.add_argument("--sweep-qps", default="10,20,30,40,50")
    p.add_argument("--max-error-rate", type=float, default=0.01)
    p.add_argument("--max-p99-ms", type=float, default=200.0)
    p.add_argument("--csv", default="bench_capacity.csv")
    args = p.parse_args()

    if args.quick_capacity:
        run_capacity_quick(args)
    elif args.sweep:
        run_sweep(args)
    else:
        r = run_benchmark(args.host, args.port, args.messages, args.clients, args.qps, args.timeout_ms, args.sender, args.mode, args.bind_wait_ms)
        print_result(r)


if __name__ == "__main__":
    main()
