#!/usr/bin/env python3
from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import random
import shutil
import time
import zipfile
from pathlib import Path
from typing import Iterable, Iterator


ASCII_WORDS = [
    "src",
    "build",
    "bin",
    "obj",
    "out",
    "dist",
    "docs",
    "doc",
    "test",
    "tests",
    "tmp",
    "temp",
    "cache",
    "logs",
    "log",
    "config",
    "configs",
    "settings",
    "scripts",
    "script",
    "tools",
    "vendor",
    "thirdparty",
    "patch",
    "bugfix",
    "release",
    "debug",
    "backup",
    "archive",
    "draft",
    "notes",
    "asset",
    "assets",
    "pkg",
    "packages",
    "module",
    "modules",
    "service",
    "client",
    "server",
    "worker",
    "api",
    "core",
    "common",
    "shared",
    "public",
    "private",
    "internal",
    "feature",
    "hotfix",
    "legacy",
    "report",
    "data",
    "dataset",
    "sample",
    "demo",
    "workspace",
    "project",
    "repo",
    "branch",
    "commit",
    "merge",
    "review",
    "prototype",
    "experiment",
    "benchmark",
    "profile",
    "trace",
    "dump",
    "snapshot",
    "index",
    "manifest",
    "schema",
    "migration",
    "fixture",
    "mock",
    "stub",
    "driver",
    "firmware",
    "device",
    "board",
    "sensor",
    "gateway",
    "cloud",
    "docker",
    "k8s",
    "deploy",
    "pipeline",
    "ci",
    "coverage",
    "artifact",
    "export",
    "import",
    "sync",
    "queue",
    "job",
    "task",
    "event",
    "metrics",
    "audit",
    "billing",
    "invoice",
    "meeting",
    "todo",
    "readme",
    "manual",
    "design",
    "spec",
    "plan",
    "ops",
]

ASCII_EXTS = [
    ".txt",
    ".log",
    ".json",
    ".ini",
    ".cfg",
    ".xml",
    ".yaml",
    ".yml",
    ".md",
    ".csv",
    ".py",
    ".cs",
    ".cpp",
    ".h",
    ".sql",
    ".bat",
    ".ps1",
    ".ts",
    ".js",
    ".sh",
    ".java",
    ".go",
    ".rs",
    ".kt",
    ".swift",
    ".lua",
    ".rb",
    ".php",
    ".toml",
    ".lock",
    ".env",
    ".conf",
    ".properties",
    ".proto",
    ".graphql",
    ".html",
    ".css",
    ".scss",
    ".less",
    ".map",
    ".dmp",
    ".trace",
    ".dump",
    ".bak",
]

CN_WORDS = [
    "源码",
    "构建",
    "输出",
    "文档",
    "测试",
    "临时",
    "缓存",
    "日志",
    "配置",
    "脚本",
    "工具",
    "依赖",
    "补丁",
    "修复",
    "发布",
    "调试",
    "备份",
    "归档",
    "草稿",
    "笔记",
    "资源",
    "资产",
    "包",
    "模块",
    "服务",
    "客户端",
    "服务端",
    "任务",
    "接口",
    "核心",
    "公共",
    "共享",
    "内部",
    "特性",
    "热修复",
    "历史",
    "报告",
    "数据",
    "样本",
    "示例",
    "工作区",
    "项目",
    "仓库",
    "分支",
    "提交",
    "合并",
    "评审",
    "原型",
    "实验",
    "基准",
    "性能",
    "追踪",
    "转储",
    "快照",
    "索引",
    "清单",
    "结构",
    "迁移",
    "夹具",
    "模拟",
    "桩件",
    "驱动",
    "固件",
    "设备",
    "板卡",
    "传感器",
    "网关",
    "云端",
    "容器",
    "集群",
    "部署",
    "流水线",
    "集成",
    "覆盖率",
    "制品",
    "导出",
    "导入",
    "同步",
    "队列",
    "作业",
    "事件",
    "指标",
    "审计",
    "账单",
    "会议",
    "待办",
    "说明",
    "手册",
    "设计",
    "规格",
    "计划",
    "运维",
    "客户",
    "资源历史",
    "现场",
    "验收",
    "排查",
    "诊断",
    "监控",
]

# Chinese base names still use ASCII-only suffixes to match normal Windows file associations.
CN_EXTS = [
    ".txt",
    ".log",
    ".json",
    ".ini",
    ".cfg",
    ".xml",
    ".yaml",
    ".yml",
    ".md",
    ".csv",
    ".py",
    ".cs",
    ".cpp",
    ".h",
    ".sql",
    ".bat",
    ".ps1",
    ".ts",
    ".js",
    ".sh",
    ".conf",
    ".properties",
    ".toml",
    ".proto",
    ".bak",
    ".dump",
]

RESERVED_NAMES = {
    "con",
    "prn",
    "aux",
    "nul",
    *(f"com{i}" for i in range(1, 10)),
    *(f"lpt{i}" for i in range(1, 10)),
}

INVALID_CHARS = '<>:"/\\|?*'


@dataclasses.dataclass(slots=True)
class Config:
    output_root: Path
    zip_count: int
    entry_count: int
    chinese_ratio: float
    zip_chinese_ratio: float
    compression: str
    compresslevel: int
    max_total_bytes: int
    seed: int
    clean: bool
    dry_run: bool
    verify_only: bool
    report_json: Path | None
    progress_every: int
    zip_name_max_len: int
    file_name_max_len: int
    content_min_bytes: int
    content_max_bytes: int
    jobs: int


@dataclasses.dataclass(frozen=True, slots=True)
class WriteTask:
    output_root: Path
    zip_index: int
    zip_count: int
    entry_start: int
    entry_count: int
    seed: int
    chinese_ratio: float
    zip_chinese_ratio: float
    zip_name_max_len: int
    file_name_max_len: int
    content_min_bytes: int
    content_max_bytes: int
    compression: str
    compresslevel: int


def parse_args() -> Config:
    parser = argparse.ArgumentParser(
        description="Generate EveryZip test zip archives with realistic programmer-style names."
    )
    parser.add_argument("--output-root", type=Path, default=Path(r"E:\EveryZipTestData"))
    parser.add_argument("--zip-count", type=int, default=10_000)
    parser.add_argument("--entry-count", type=int, default=5_300_000)
    parser.add_argument("--chinese-ratio", type=float, default=0.30)
    parser.add_argument("--zip-chinese-ratio", type=float, default=0.30)
    parser.add_argument("--compression", choices=("deflate", "store"), default="deflate")
    parser.add_argument("--compresslevel", type=int, default=6)
    parser.add_argument("--max-total-bytes", type=int, default=4 * 1024 * 1024 * 1024)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--report-json", type=Path)
    parser.add_argument("--progress-every", type=int, default=1000)
    parser.add_argument("--zip-name-max-len", type=int, default=48)
    parser.add_argument("--file-name-max-len", type=int, default=64)
    parser.add_argument("--content-min-bytes", type=int, default=48)
    parser.add_argument("--content-max-bytes", type=int, default=160)
    parser.add_argument("--jobs", type=int, default=1, help="Parallel zip writers. Use 4 or 8 for faster generation.")
    ns = parser.parse_args()

    if ns.zip_count <= 0:
        raise SystemExit("--zip-count must be positive")
    if ns.entry_count <= 0:
        raise SystemExit("--entry-count must be positive")
    if not (0.0 <= ns.chinese_ratio <= 1.0):
        raise SystemExit("--chinese-ratio must be between 0 and 1")
    if not (0.0 <= ns.zip_chinese_ratio <= 1.0):
        raise SystemExit("--zip-chinese-ratio must be between 0 and 1")
    if ns.max_total_bytes <= 0:
        raise SystemExit("--max-total-bytes must be positive")
    if ns.compresslevel < 0 or ns.compresslevel > 9:
        raise SystemExit("--compresslevel must be between 0 and 9")
    if ns.content_min_bytes <= 0 or ns.content_max_bytes < ns.content_min_bytes:
        raise SystemExit("--content-min-bytes and --content-max-bytes are invalid")
    if ns.progress_every <= 0:
        raise SystemExit("--progress-every must be positive")
    if ns.jobs <= 0:
        raise SystemExit("--jobs must be positive")

    return Config(
        output_root=ns.output_root,
        zip_count=ns.zip_count,
        entry_count=ns.entry_count,
        chinese_ratio=ns.chinese_ratio,
        zip_chinese_ratio=ns.zip_chinese_ratio,
        compression=ns.compression,
        compresslevel=ns.compresslevel,
        max_total_bytes=ns.max_total_bytes,
        seed=ns.seed,
        clean=ns.clean,
        dry_run=ns.dry_run,
        verify_only=ns.verify_only,
        report_json=ns.report_json,
        progress_every=ns.progress_every,
        zip_name_max_len=ns.zip_name_max_len,
        file_name_max_len=ns.file_name_max_len,
        content_min_bytes=ns.content_min_bytes,
        content_max_bytes=ns.content_max_bytes,
        jobs=ns.jobs,
    )


def clamp_name(text: str, max_len: int) -> str:
    cleaned = "".join(ch for ch in text if ch not in INVALID_CHARS)
    cleaned = cleaned.strip(" .")
    if not cleaned:
        cleaned = "file"
    if cleaned.lower() in RESERVED_NAMES:
        cleaned = "_" + cleaned
    if len(cleaned) > max_len:
        cleaned = cleaned[:max_len].rstrip(" ._")
    if not cleaned:
        cleaned = "file"
    return cleaned


def build_dir_parts(rng: random.Random, chinese: bool, min_parts: int = 1, max_parts: int = 3) -> list[str]:
    pool = CN_WORDS if chinese else ASCII_WORDS
    count = rng.randint(min_parts, max_parts)
    parts: list[str] = []
    for _ in range(count):
        part = rng.choice(pool)
        if not chinese:
            part = clamp_name(part, 24)
        parts.append(part)
    return parts


def normalize_ext(ext: str, chinese: bool) -> str:
    if chinese:
        return ext if ext.startswith(".") else f".{ext}"
    if ext and ext[0] == "." and all(ord(ch) < 128 for ch in ext):
        return ext
    return ".txt"


def zip_compression_name(compression: str) -> int:
    if compression == "store":
        return zipfile.ZIP_STORED
    return zipfile.ZIP_DEFLATED


def zip_compression(config: Config) -> int:
    return zip_compression_name(config.compression)


def make_rng(seed: int) -> random.Random:
    return random.Random(seed)


def choose_ratio(rng: random.Random, ratio: float) -> bool:
    return rng.random() < ratio


def build_ascii_zip_name(rng: random.Random, idx: int, max_len: int) -> str:
    parts = [rng.choice(ASCII_WORDS) for _ in range(rng.randint(2, 4))]
    parts.append(f"{idx:05d}")
    ext = rng.choice([".zip", ".ZIP"])
    name = "_".join(parts) + ext
    return clamp_name(name, max_len)


def build_cn_zip_name(rng: random.Random, idx: int, max_len: int) -> str:
    parts = [rng.choice(CN_WORDS) for _ in range(rng.randint(2, 4))]
    parts.append(f"{idx:05d}")
    ext = rng.choice([".zip", ".ZIP"])
    name = "".join(parts) + ext
    return clamp_name(name, max_len)


def build_ascii_file_name(rng: random.Random, idx: int, max_len: int) -> str:
    prefix = rng.choice(ASCII_WORDS)
    middle = rng.choice(ASCII_WORDS)
    suffix = rng.choice(ASCII_WORDS)
    ext = rng.choice(ASCII_EXTS)
    serial = f"{idx % 100000:05d}"
    templates = [
        f"{prefix}_{middle}_{suffix}_{serial}{ext}",
        f"{prefix}-{middle}-{serial}{ext}",
        f"{prefix}.{middle}.{suffix}.{serial}{ext}",
        f"{prefix}_{serial}{ext}",
        f"{prefix}_{middle}_{serial}{ext}",
    ]
    return clamp_name(rng.choice(templates), max_len)


def build_cn_file_name(rng: random.Random, idx: int, max_len: int) -> str:
    prefix = rng.choice(CN_WORDS)
    middle = rng.choice(CN_WORDS)
    suffix = rng.choice(CN_WORDS)
    ext = normalize_ext(rng.choice(CN_EXTS), True)
    serial = f"{idx % 100000:05d}"
    templates = [
        f"{prefix}_{middle}_{suffix}_{serial}{ext}",
        f"{prefix}-{middle}-{serial}{ext}",
        f"{prefix}{middle}{suffix}{serial}{ext}",
        f"{prefix}_{serial}{ext}",
        f"{prefix}_{middle}_{serial}{ext}",
    ]
    return clamp_name(rng.choice(templates), max_len)


def build_ascii_file_path(rng: random.Random, idx: int, max_len: int) -> str:
    dirs = build_dir_parts(rng, chinese=False, min_parts=1, max_parts=3)
    file_name = build_ascii_file_name(rng, idx, max_len)
    return "/".join(dirs + [file_name])


def build_cn_file_path(rng: random.Random, idx: int, max_len: int) -> str:
    dirs = build_dir_parts(rng, chinese=True, min_parts=1, max_parts=3)
    file_name = build_cn_file_name(rng, idx, max_len)
    return "/".join(dirs + [file_name])


def build_ascii_zip_relpath(rng: random.Random, idx: int, max_len: int) -> str:
    dirs = build_dir_parts(rng, chinese=False, min_parts=1, max_parts=2)
    zip_name = build_ascii_zip_name(rng, idx, max_len)
    return "/".join(dirs + [zip_name])


def build_cn_zip_relpath(rng: random.Random, idx: int, max_len: int) -> str:
    dirs = build_dir_parts(rng, chinese=True, min_parts=1, max_parts=2)
    zip_name = build_cn_zip_name(rng, idx, max_len)
    return "/".join(dirs + [zip_name])


def make_content(rng: random.Random, zip_index: int, entry_index: int, chinese: bool, min_size: int, max_size: int) -> bytes:
    size = rng.randint(min_size, max_size)
    if chinese:
        base = f"测试数据|zip={zip_index}|entry={entry_index}|构建|日志|配置|脚本|".encode("utf-8")
    else:
        base = f"test-data|zip={zip_index}|entry={entry_index}|build|log|config|script|".encode("utf-8")
    if len(base) >= size:
        return base[:size]
    filler = (b"0" if not chinese else "零".encode("utf-8"))
    repeat = (size - len(base) + len(filler) - 1) // len(filler)
    return (base + filler * repeat)[:size]


def iter_write_tasks(config: Config) -> Iterator[WriteTask]:
    base = config.entry_count // config.zip_count
    remainder = config.entry_count % config.zip_count
    entry_start = 0
    for zip_index in range(config.zip_count):
        per_zip = base + (1 if zip_index < remainder else 0)
        yield WriteTask(
            output_root=config.output_root,
            zip_index=zip_index,
            zip_count=config.zip_count,
            entry_start=entry_start,
            entry_count=per_zip,
            seed=config.seed,
            chinese_ratio=config.chinese_ratio,
            zip_chinese_ratio=config.zip_chinese_ratio,
            zip_name_max_len=config.zip_name_max_len,
            file_name_max_len=config.file_name_max_len,
            content_min_bytes=config.content_min_bytes,
            content_max_bytes=config.content_max_bytes,
            compression=config.compression,
            compresslevel=config.compresslevel,
        )
        entry_start += per_zip


def make_task_rng(task: WriteTask) -> random.Random:
    return make_rng(task.seed + task.zip_index * 1_000_003)


def write_one_archive(task: WriteTask) -> tuple[int, int, int, str]:
    rng = make_task_rng(task)
    zip_is_cn = choose_ratio(rng, task.zip_chinese_ratio)
    zip_relpath = (
        build_cn_zip_relpath(rng, task.zip_index, task.zip_name_max_len)
        if zip_is_cn
        else build_ascii_zip_relpath(rng, task.zip_index, task.zip_name_max_len)
    )
    zip_path = task.output_root / Path(zip_relpath)
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    compression = zip_compression_name(task.compression)
    with zipfile.ZipFile(
        zip_path,
        mode="w",
        compression=compression,
        compresslevel=task.compresslevel if compression == zipfile.ZIP_DEFLATED else None,
        allowZip64=True,
    ) as zf:
        for offset in range(task.entry_count):
            entry_index = task.entry_start + offset
            is_cn = choose_ratio(rng, task.chinese_ratio)
            internal_name = (
                build_cn_file_path(rng, entry_index, task.file_name_max_len)
                if is_cn
                else build_ascii_file_path(rng, entry_index, task.file_name_max_len)
            )
            zf.writestr(
                internal_name,
                make_content(
                    rng,
                    zip_index=task.zip_index,
                    entry_index=entry_index,
                    chinese=is_cn,
                    min_size=task.content_min_bytes,
                    max_size=task.content_max_bytes,
                ),
            )
    return task.zip_index, task.entry_count, zip_path.stat().st_size, str(zip_path)


def estimate_archive_bytes(config: Config) -> int:
    avg_filename_bytes = (
        max(16, config.file_name_max_len // 2)
        if config.chinese_ratio < 0.5
        else max(20, int(config.file_name_max_len * 1.6))
    )
    avg_content_bytes = (config.content_min_bytes + config.content_max_bytes) // 2
    compressed_guess = max(24, avg_content_bytes // 3)
    per_entry = 120 + avg_filename_bytes + compressed_guess
    total = config.entry_count * per_entry
    total += config.zip_count * 128
    return total


def ensure_output_root(config: Config) -> None:
    if config.clean or config.verify_only:
        return
    config.output_root.mkdir(parents=True, exist_ok=True)


def safe_clean(output_root: Path) -> None:
    resolved = output_root.resolve()
    if not resolved.exists():
        return
    if len(resolved.parts) < 2:
        raise SystemExit(f"Refusing to clean unsafe path: {resolved}")
    shutil.rmtree(resolved)


def write_archives(config: Config) -> dict[str, object]:
    total_written = 0
    zip_written = 0
    entry_written = 0
    started = time.time()
    next_progress = config.progress_every

    def handle_result(result: tuple[int, int, int, str]) -> None:
        nonlocal total_written, zip_written, entry_written, next_progress
        zip_index, entries, archive_bytes, zip_path = result
        zip_written += 1
        entry_written += entries
        total_written += archive_bytes
        if total_written > config.max_total_bytes:
            raise SystemExit(
                f"Exceeded budget after {zip_written} zip files: "
                f"{total_written} > {config.max_total_bytes}"
            )
        if entry_written >= next_progress or zip_written == config.zip_count:
            elapsed = max(time.time() - started, 0.001)
            print(
                f"[progress] zips={zip_written}/{config.zip_count} "
                f"entries={entry_written}/{config.entry_count} "
                f"bytes={total_written} elapsed={elapsed:.1f}s"
            )
            next_progress += config.progress_every
        if zip_written % max(1, config.progress_every) == 0:
            print(f"[zip] wrote {zip_written}/{config.zip_count}: {Path(zip_path).name}")

    tasks = list(iter_write_tasks(config))
    if config.jobs == 1:
        for task in tasks:
            handle_result(write_one_archive(task))
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=config.jobs) as executor:
            future_to_zip = {executor.submit(write_one_archive, task): task.zip_index for task in tasks}
            for future in concurrent.futures.as_completed(future_to_zip):
                handle_result(future.result())

    elapsed = max(time.time() - started, 0.001)
    return {
        "zip_count": zip_written,
        "entry_count": entry_written,
        "total_bytes": total_written,
        "elapsed_seconds": elapsed,
    }


def verify_archives(config: Config) -> dict[str, object]:
    if not config.output_root.exists():
        raise SystemExit(f"Output root does not exist: {config.output_root}")
    zip_files = sorted({path.resolve() for path in config.output_root.rglob("*") if path.suffix.lower() == ".zip"})
    total_entries = 0
    total_bytes = 0
    for path in zip_files:
        total_bytes += path.stat().st_size
        with zipfile.ZipFile(path, "r") as zf:
            total_entries += len(zf.infolist())
            bad = zf.testzip()
            if bad is not None:
                raise SystemExit(f"Corrupted entry in {path}: {bad}")
    return {
        "zip_count": len(zip_files),
        "entry_count": total_entries,
        "total_bytes": total_bytes,
    }


def write_report(path: Path, report: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> int:
    config = parse_args()

    if config.clean:
        if config.output_root.exists():
            safe_clean(config.output_root)

    if config.verify_only:
        report = verify_archives(config)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if config.report_json:
            write_report(config.report_json, report)
        return 0

    ensure_output_root(config)

    estimated = estimate_archive_bytes(config)

    report: dict[str, object] = {
        "output_root": str(config.output_root),
        "zip_count": config.zip_count,
        "entry_count": config.entry_count,
        "chinese_ratio": config.chinese_ratio,
        "zip_chinese_ratio": config.zip_chinese_ratio,
        "compression": config.compression,
        "compresslevel": config.compresslevel,
        "max_total_bytes": config.max_total_bytes,
        "estimated_total_bytes": estimated,
        "dry_run": config.dry_run,
        "seed": config.seed,
        "jobs": config.jobs,
    }

    if estimated > config.max_total_bytes:
        report["status"] = "rejected"
        report["reason"] = "estimated archive size exceeds budget"
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if config.report_json:
            write_report(config.report_json, report)
        return 2

    if config.dry_run:
        report["status"] = "dry_run"
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if config.report_json:
            write_report(config.report_json, report)
        return 0

    result = write_archives(config)
    report.update(result)
    report["status"] = "ok"
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if config.report_json:
        write_report(config.report_json, report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
