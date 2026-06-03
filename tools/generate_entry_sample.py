#!/usr/bin/env python3
"""Generate archive TSV + entry TSV sample files for ezdb benchmarking.

Usage:
  python tools/generate_entry_sample.py \
      --archives test_data/archives_10k.tsv \
      --entries test_data/entries_100k.tsv \
      --archive-count 10000 \
      --entry-count 100000 \
      [--seed 42] \
      [--zh-ratio 0.3] \
      [--entries-min 0] \
      [--entries-max 50]
"""

import argparse
import os
import struct
import sys


# --- RNG: xoshiro256** ---

def _splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    z = z ^ (z >> 31)
    return state, z


class Xoshiro256:
    def __init__(self, seed):
        self.s = [0] * 4
        st = seed
        for i in range(4):
            st, v = _splitmix64(st)
            self.s[i] = v

    def next(self):
        s = self.s
        result = ((s[1] * 5) & 0xFFFFFFFFFFFFFFFF)
        result = ((result << 7) | (result >> 57)) & 0xFFFFFFFFFFFFFFFF
        result = (result * 9) & 0xFFFFFFFFFFFFFFFF
        t = s[1] << 17
        s[2] = (s[2] ^ s[0]) & 0xFFFFFFFFFFFFFFFF
        s[3] = (s[3] ^ s[1]) & 0xFFFFFFFFFFFFFFFF
        s[1] = (s[1] ^ s[2]) & 0xFFFFFFFFFFFFFFFF
        s[0] = (s[0] ^ s[3]) & 0xFFFFFFFFFFFFFFFF
        s[2] = (s[2] ^ t) & 0xFFFFFFFFFFFFFFFF
        s[3] = ((s[3] << 45) | (s[3] >> 19)) & 0xFFFFFFFFFFFFFFFF
        return result

    def next_int(self, n):
        """Uniform integer in [0, n)."""
        if n <= 0:
            return 0
        return self.next() % n


# --- Path components ---

DIRS_EN = [
    "Documents", "Downloads", "Pictures", "Videos", "Music", "Projects",
    "Archives", "Work", "Temp", "Backups", "Logs", "Source", "Data",
]
DIRS_ZH = [
    "文档", "下载", "图片", "视频", "音乐", "项目", "归档",
    "工作", "临时", "备份", "日志", "源码", "数据",
]
NAMES_EN = [
    "report", "image", "archive", "invoice", "notes", "backup", "photo",
    "dataset", "summary", "config", "readme", "build", "export", "index",
]
NAMES_ZH = [
    "报告", "图片", "归档", "发票", "笔记", "备份", "照片",
    "数据集", "汇总", "配置", "说明", "构建", "导出", "索引",
]
ARCHIVE_EXTS = ["zip", "7z", "rar", "tar", "gz", "bz2", "xz", "cab", "iso", "wim"]

ENTRY_DIRS = [
    "src", "include", "lib", "bin", "res", "assets", "data", "config",
    "docs", "scripts", "templates", "locale", "plugins", "modules",
]
ENTRY_EXTS = [
    "txt", "csv", "json", "xml", "html", "css", "js", "py", "c", "h",
    "cpp", "md", "png", "jpg", "gif", "bin", "dat", "cfg", "ini", "log",
]


def _pick(rng, choices, zh_choices, zh_ratio):
    use_zh = rng.next_int(100) < int(zh_ratio * 100)
    pool = zh_choices if use_zh else choices
    return pool[rng.next_int(len(pool))]


def generate_archive_path(rng, index, zh_ratio):
    drive = chr(ord('C') + rng.next_int(4))
    dir1 = _pick(rng, DIRS_EN, DIRS_ZH, zh_ratio)
    dir2 = _pick(rng, DIRS_EN, DIRS_ZH, zh_ratio)
    name = _pick(rng, NAMES_EN, NAMES_ZH, zh_ratio)
    ext = ARCHIVE_EXTS[rng.next_int(len(ARCHIVE_EXTS))]
    return f"{drive}:\\Users\\sample\\{dir1}\\{dir2}\\{name}_{index:08d}.{ext}"


def generate_entry_path(rng, zh_ratio):
    d1 = ENTRY_DIRS[rng.next_int(len(ENTRY_DIRS))]
    d2 = ENTRY_DIRS[rng.next_int(len(ENTRY_DIRS))]
    name = _pick(rng, NAMES_EN, NAMES_ZH, zh_ratio)
    ext = ENTRY_EXTS[rng.next_int(len(ENTRY_EXTS))]
    num = rng.next_int(10000)
    return f"{d1}/{d2}/{name}_{num:04d}.{ext}"


def main():
    parser = argparse.ArgumentParser(description="Generate archive + entry TSV samples for ezdb benchmarking")
    parser.add_argument("--archives", required=True, help="Output archive TSV path")
    parser.add_argument("--entries", required=True, help="Output entry TSV path")
    parser.add_argument("--archive-count", type=int, required=True, help="Number of archive records")
    parser.add_argument("--entry-count", type=int, required=True, help="Target total entry count")
    parser.add_argument("--seed", type=int, default=42, help="RNG seed (default: 42)")
    parser.add_argument("--zh-ratio", type=float, default=0.3, help="Chinese path ratio 0..1 (default: 0.3)")
    parser.add_argument("--entries-min", type=int, default=0, help="Min entries per archive (default: 0)")
    parser.add_argument("--entries-max", type=int, default=50, help="Max entries per archive (default: 50)")
    args = parser.parse_args()

    n_archives = args.archive_count
    target_entries = args.entry_count
    e_min = args.entries_min
    e_max = args.entries_max
    zh_ratio = args.zh_ratio
    rng = Xoshiro256(args.seed)

    if n_archives <= 0 or target_entries < 0:
        print("Error: archive-count and entry-count must be positive", file=sys.stderr)
        sys.exit(1)
    if e_min < 0 or e_max < e_min:
        print("Error: invalid entries-min/max range", file=sys.stderr)
        sys.exit(1)

    # Distribute entries across archives
    counts = [e_min] * n_archives
    remaining = target_entries - e_min * n_archives

    if remaining < 0:
        print(f"Error: entry-count ({target_entries}) too low for {n_archives} archives x entries-min={e_min}",
              file=sys.stderr)
        sys.exit(1)

    span = e_max - e_min
    attempts = 0
    while remaining > 0 and attempts < remaining * 10:
        idx = rng.next_int(n_archives)
        if counts[idx] < e_max:
            counts[idx] += 1
            remaining -= 1
        attempts += 1

    if remaining > 0:
        for i in range(n_archives):
            add = min(remaining, e_max - counts[i])
            counts[i] += add
            remaining -= add
            if remaining <= 0:
                break

    total_entries = sum(counts)
    archives_with_entries = sum(1 for c in counts if c > 0)

    for path in (args.archives, args.entries):
        d = os.path.dirname(path)
        if d:
            os.makedirs(d, exist_ok=True)

    print(f"Generating {n_archives} archives and {total_entries} entries "
          f"({archives_with_entries} archives with entries)...")

    with open(args.archives, "w", encoding="utf-8", newline="") as f_arch:
        for i in range(n_archives):
            drive = chr(ord('C') + rng.next_int(4))
            dir1 = _pick(rng, DIRS_EN, DIRS_ZH, zh_ratio)
            dir2 = _pick(rng, DIRS_EN, DIRS_ZH, zh_ratio)
            name = _pick(rng, NAMES_EN, NAMES_ZH, zh_ratio)
            ext = ARCHIVE_EXTS[rng.next_int(len(ARCHIVE_EXTS))]
            file_path = f"{drive}:\\Users\\sample\\{dir1}\\{dir2}\\{name}_{i:08d}.{ext}"
            file_ref = 0x100000000 + i * 17 + (rng.next() & 0xFFFF)
            usn = 0x400000000 + i * 3 + (rng.next() & 0xFFFF)
            file_size = rng.next() % (512 * 1024 * 1024)
            modified_time = 1577836800 + (rng.next() % 220752000)
            f_arch.write(f"{drive}\t{file_ref}\t{usn}\t{file_path}\t{file_size}\t{modified_time}\n")

    archive_size = os.path.getsize(args.archives)

    with open(args.entries, "w", encoding="utf-8", newline="") as f_ent:
        for arch_idx in range(n_archives):
            for e_idx in range(counts[arch_idx]):
                entry_path = generate_entry_path(rng, zh_ratio)
                original_size = rng.next() % (50 * 1024 * 1024)
                ratio = 0.1 + (rng.next() % 80) / 100.0
                compressed_size = int(original_size * ratio)
                if rng.next_int(10) == 0:
                    compressed_size = -1
                modified_time = 1577836800 + (rng.next() % 220752000)
                f_ent.write(f"{arch_idx}\t{entry_path}\t{compressed_size}\t{original_size}\t{modified_time}\n")

    entry_size = os.path.getsize(args.entries)

    print(f"Done.")
    print(f"  Archives: {args.archives} ({archive_size:,} bytes, {n_archives} records)")
    print(f"  Entries:  {args.entries} ({entry_size:,} bytes, {total_entries} records)")


if __name__ == "__main__":
    main()