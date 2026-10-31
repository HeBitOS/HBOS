#!/usr/bin/env python3
"""perf_baseline.py 的统计口径回归测试（纯宿主，不需要 QEMU）。

为什么要有它：性能脚本最危险的失效方式不是"报错"，而是"报了一个看起来
很正常的数"。这里把几条口径钉死：

  1. 失败的样本不能进统计（-1.0 这种哨兵值一旦参与 median，会把结论
     拉向"变快了"）。
  2. `n` 必须如实反映真正参与计算的样本数，不能只写 requested。
  3. 样本不足时不给结论（退出码非 0），而不是拿 1 个样本冒充中位数。
  4. measure_workload 只解析 `run ...` 那条命令的耗时，不把整段子进程
     耗时（含启动 + 固定 1s 等待）当成负载耗时。

用法：python3 tests/host/test_perf_baseline.py
"""
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
FAILURES = 0


def check(cond, label):
    global FAILURES
    if not cond:
        print(f"  [FAIL] {label}")
        FAILURES += 1


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_summarize():
    pb = load("perf_baseline", "scripts/perf_baseline.py")

    # 全部失败：不给中位数
    s = pb.summarize([], 3, "x")
    check(s["median_s"] is None, "全失败时 median 必须是 None")
    check(s["n"] == 0, "全失败时 n 必须是 0")
    check(s["requested"] == 3, "requested 要如实记录")

    # 部分失败：n 必须缩小，且 requested 保持不变
    s = pb.summarize([10.1, 9.9], 3, "x")
    check(s["n"] == 2, "2 个有效样本 n 必须是 2")
    check(s["requested"] == 3, "requested 仍是 3（丢了 1 个）")
    check(s["median_s"] == 10.0, "中位数应是 10.0")

    # 单个样本：n=1，供 main 判断"样本不足"
    s = pb.summarize([10.1], 3, "x")
    check(s["n"] == 1, "1 个有效样本 n 必须是 1")

    # spread 能反映抖动
    s = pb.summarize([9.0, 11.0], 2, "x")
    check(s["spread_s"] == 2.0, f"spread 应为 2.0，得到 {s['spread_s']}")

    # 关键：-1.0 绝不会被当成样本（哨兵值已从返回类型里去掉）
    check(pb.summarize([-1.0, -1.0], 2, "x")["median_s"] == -1.0,
          "sanity: 若有人硬塞 -1.0，summarize 不会自己过滤——"
          "过滤责任在 measure_* 返回 None")


def test_workload_sentinel():
    """measure_workload 必须返回 None 而不是 -1.0。"""
    src = (ROOT / "scripts/perf_baseline.py").read_text()
    check("return -1.0" not in src, "perf_baseline.py 里不应再有 return -1.0")
    check("return None" in src, "失败路径应返回 None")


def test_workload_timing_parse():
    """用假的 qemu_session 输出验证：只认 `run ...` 那一条的耗时。"""
    pb = load("perf_baseline", "scripts/perf_baseline.py")
    import types

    fake_stderr = (
        "[qemu_session] accel=tcg mem=2048\n"
        "[qemu_session] cmd 12.500s: wayland --probe-input\n"
        "[qemu_session] cmd 3.250s: run /bin/wayland_surface\n"
    )
    real_run = pb.subprocess.run

    class FakeProc:
        stderr = fake_stderr

    pb.subprocess.run = lambda *a, **k: FakeProc()
    tmpdir = Path(tempfile.mkdtemp(dir=ROOT / "build/tmp"))
    log = tmpdir / "fake.log"
    log.write_text("wl-surface: PASS\n")
    try:
        got = pb.measure_workload(Path("iso"), log, None)
    finally:
        pb.subprocess.run = real_run
    check(got == 3.25,
          f"应取 run 那条的 3.25s（不是 12.5s，也不是两段之和），得到 {got}")

    # 没 PASS 时必须返回 None
    log.write_text("wl-surface: FAIL\n")
    pb.subprocess.run = lambda *a, **k: FakeProc()
    try:
        got = pb.measure_workload(Path("iso"), log, None)
    finally:
        pb.subprocess.run = real_run
    check(got is None, f"测试未通过时应返回 None，得到 {got}")


def test_cli_no_samples():
    """没有任何有效样本时，退出码必须非 0 且不写假的 median。"""
    tmpdir = Path(tempfile.mkdtemp(dir=ROOT / "build/tmp"))
    iso = tmpdir / "fake.iso"
    iso.write_bytes(b"not really an iso")
    out = tmpdir / "out.json"
    # 用一个必然失败的"ISO"跑 1 轮；QEMU 起不来 -> boot 超时/失败
    proc = subprocess.run(
        [sys.executable, str(ROOT / "scripts/perf_baseline.py"),
         "--iso", str(iso), "--runs", "1", "--json", str(out)],
        capture_output=True, text=True, timeout=600)
    check(proc.returncode != 0, f"无有效样本时退出码应非 0，得到 {proc.returncode}")
    check(out.exists(), "即使失败也要写出 JSON 供排查")
    if out.exists():
        data = json.loads(out.read_text())
        check(data["boot"]["median_s"] is None,
              "无有效样本时 boot.median_s 必须是 null，不能是数字")


def main():
    print("[PERF-BASELINE] 统计口径回归测试")
    print(" - summarize 的 n/median/spread")
    test_summarize()
    print(" - measure_workload 的失败哨兵")
    test_workload_sentinel()
    print(" - measure_workload 的计时段解析")
    test_workload_timing_parse()
    if "--with-qemu" in sys.argv:
        print(" - CLI 无样本行为（会短暂启动 QEMU）")
        test_cli_no_samples()
    else:
        print(" - CLI 无样本行为：跳过（加 --with-qemu 才跑，需独占 QEMU）")
    if FAILURES:
        print(f"\n[PERF-BASELINE] FAILED: {FAILURES}")
        return 1
    print("\n[PERF-BASELINE] all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
