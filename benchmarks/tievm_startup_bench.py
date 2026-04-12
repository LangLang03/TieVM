#!/usr/bin/env python3
import argparse
import glob
import math
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def run_checked(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd) if cwd is not None else None,
        text=True,
        capture_output=True,
        check=True,
    )


def find_release_binary(name: str) -> Path | None:
    candidates = [
        Path(p)
        for p in glob.glob(f"build/**/release/{name}", recursive=True)
        if Path(p).is_file()
    ]
    if not candidates and sys.platform.startswith("win"):
        candidates = [
            Path(p)
            for p in glob.glob(f"build/**/release/{name}.exe", recursive=True)
            if Path(p).is_file()
        ]
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def ensure_binaries(skip_build: bool) -> tuple[Path, Path]:
    tiebc = find_release_binary("tiebc")
    tievm = find_release_binary("tievm")
    if tiebc is not None and tievm is not None:
        return tiebc, tievm
    if skip_build:
        raise RuntimeError("release binaries not found; build first or remove --skip-build")
    run_checked(["xmake", "f", "-m", "release"])
    run_checked(["xmake", "build", "tiebc", "tievm"])
    tiebc = find_release_binary("tiebc")
    tievm = find_release_binary("tievm")
    if tiebc is None or tievm is None:
        raise RuntimeError("failed to locate release tiebc/tievm binaries after build")
    return tiebc, tievm


def percentile(values: list[float], p: float) -> float:
    if not values:
        raise ValueError("empty sample")
    sorted_values = sorted(values)
    rank = max(0, min(len(sorted_values) - 1, math.ceil((p / 100.0) * len(sorted_values)) - 1))
    return sorted_values[rank]


def timed_run(cmd: list[str]) -> tuple[float, str]:
    t0 = time.perf_counter()
    completed = run_checked(cmd)
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    return elapsed_ms, completed.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--cold-rounds", type=int, default=7)
    parser.add_argument("--hot-rounds", type=int, default=15)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--trusted", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--keep-workdir", action="store_true")
    parser.add_argument("--workdir", type=Path, default=None)
    parser.add_argument("--cache-dir", type=Path, default=None)
    args = parser.parse_args()

    if args.cold_rounds < 0 or args.hot_rounds < 0 or args.warmup < 0:
        raise ValueError("cold-rounds, hot-rounds, warmup must be >= 0")
    if args.cold_rounds == 0 and args.hot_rounds == 0:
        raise ValueError("at least one of cold-rounds/hot-rounds must be > 0")

    tiebc, tievm = ensure_binaries(args.skip_build)

    auto_workdir = args.workdir is None
    workdir = args.workdir or Path(tempfile.mkdtemp(prefix="tievm_startup_bench_"))
    workdir.mkdir(parents=True, exist_ok=True)
    cache_dir = args.cache_dir or (workdir / "cache")
    sample_tbc = workdir / "demo_oop_ok.tbc"
    stdlib_tlbs = workdir / "stdlib.tlbs"

    run_checked([str(tiebc), "emit-oop-ok", str(sample_tbc)])
    run_checked([str(tiebc), "build-stdlib-tlbs", str(stdlib_tlbs)])

    cmd = [str(tievm), "run", str(sample_tbc), "--cache-dir", str(cache_dir)]
    if args.trusted:
        cmd.append("--trusted")

    cold_times: list[float] = []
    hot_times: list[float] = []
    outputs: list[str] = []

    for _ in range(args.warmup):
        shutil.rmtree(cache_dir, ignore_errors=True)
        _, out = timed_run(cmd)
        outputs.append(out)

    for _ in range(args.cold_rounds):
        shutil.rmtree(cache_dir, ignore_errors=True)
        elapsed, out = timed_run(cmd)
        cold_times.append(elapsed)
        outputs.append(out)

    if args.hot_rounds > 0 and not cache_dir.exists():
        # Prime cache when hot-only benchmark is requested.
        _, out = timed_run(cmd)
        outputs.append(out)

    for _ in range(args.hot_rounds):
        elapsed, out = timed_run(cmd)
        hot_times.append(elapsed)
        outputs.append(out)

    if outputs and any(line != outputs[0] for line in outputs):
        raise RuntimeError("run output mismatch across rounds")

    fields: list[str] = [
        "bench=TieVMStartup",
        "sample=emit-oop-ok",
        f"trusted={1 if args.trusted else 0}",
        f"cold_rounds={args.cold_rounds}",
        f"hot_rounds={args.hot_rounds}",
        f"cache_dir={cache_dir}",
        f"result={outputs[0] if outputs else ''}",
    ]
    if cold_times:
        fields.extend(
            [
                f"cold_median_ms={statistics.median(cold_times):.6f}",
                f"cold_p95_ms={percentile(cold_times, 95):.6f}",
                f"cold_min_ms={min(cold_times):.6f}",
                f"cold_max_ms={max(cold_times):.6f}",
            ]
        )
    if hot_times:
        fields.extend(
            [
                f"hot_median_ms={statistics.median(hot_times):.6f}",
                f"hot_p95_ms={percentile(hot_times, 95):.6f}",
                f"hot_min_ms={min(hot_times):.6f}",
                f"hot_max_ms={max(hot_times):.6f}",
            ]
        )
    print(" ".join(fields))

    if auto_workdir and not args.keep_workdir:
        shutil.rmtree(workdir, ignore_errors=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
