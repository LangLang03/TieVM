#!/usr/bin/env python3
import argparse
import statistics
import time


def fib_value(n: int) -> int:
    if n <= 0:
        return 0
    if n == 1:
        return 1
    a = 0
    b = 1
    for _ in range(2, n + 1):
        a, b = b, a + b
    return b


def sum_desc(n: int) -> int:
    acc = 0
    counter = n
    while counter != 0:
        acc += counter
        counter -= 1
    return acc


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--task", choices=["sum", "fib"], default="sum")
    parser.add_argument("--n", type=int, default=2_000_000)
    parser.add_argument("--fib-n", type=int, default=40)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--rounds", type=int, default=8)
    args = parser.parse_args()

    if args.rounds <= 0 or args.warmup < 0 or args.repeat <= 0:
        raise ValueError("invalid benchmark settings")

    if args.task == "sum":
        if args.n <= 0:
            raise ValueError("sum task requires --n > 0")
        expect = args.n * (args.n + 1) // 2

        def run_once() -> int:
            return sum_desc(args.n)

    else:
        if args.fib_n < 0 or args.fib_n > 92:
            raise ValueError("fib task requires --fib-n in [0,92]")
        expect = fib_value(args.fib_n)

        def run_once() -> int:
            return fib_value(args.fib_n)

    def run_with_repeat() -> int:
        result = 0
        for _ in range(args.repeat):
            result = run_once()
        return result

    for _ in range(args.warmup):
        run_with_repeat()

    times_ms = []
    result = 0
    for _ in range(args.rounds):
        t0 = time.perf_counter()
        result = run_with_repeat()
        t1 = time.perf_counter()
        times_ms.append((t1 - t0) * 1000.0)

    avg_ms = statistics.fmean(times_ms)
    min_ms = min(times_ms)
    max_ms = max(times_ms)

    out = [
        "bench=Python",
        f"task={args.task}",
        f"warmup={args.warmup}",
        f"rounds={args.rounds}",
        f"repeat={args.repeat}",
        f"result={result}",
        f"expect={expect}",
        f"avg_ms={avg_ms}",
        f"min_ms={min_ms}",
        f"max_ms={max_ms}",
    ]
    if args.task == "sum":
        out.append(f"n={args.n}")
    else:
        out.append(f"fib_n={args.fib_n}")
    print(" ".join(out))

    return 0 if result == expect else 4


if __name__ == "__main__":
    raise SystemExit(main())
