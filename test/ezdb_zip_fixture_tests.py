import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def run_command(args):
    proc = subprocess.run(args, text=True, capture_output=True)
    output = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise AssertionError(
            "command failed with exit code {}\n{}\n{}".format(
                proc.returncode, " ".join(str(arg) for arg in args), output
            )
        )
    return output


def require_field(output, name, expected):
    match = re.search(r"^{}:\s+([^\r\n]+)$".format(re.escape(name)), output, re.MULTILINE)
    if not match:
        raise AssertionError("missing field '{}'\n{}".format(name, output))
    actual = match.group(1).strip()
    if actual != str(expected):
        raise AssertionError("field '{}' expected {}, got {}\n{}".format(name, expected, actual, output))


def require_contains(output, text):
    if text not in output:
        raise AssertionError("expected output to contain {!r}\n{}".format(text, output))


def create_fixtures(zip_dir):
    zip_dir.mkdir(parents=True, exist_ok=True)

    small_zip = zip_dir / "small_fixture.zip"
    with zipfile.ZipFile(small_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("folder/readme.txt", "small fixture")
        zf.writestr("config/index_9146.js", "fixture index")

    empty_zip = zip_dir / "empty_fixture.zip"
    with zipfile.ZipFile(empty_zip, "w", compression=zipfile.ZIP_DEFLATED):
        pass

    comment_zip = zip_dir / "comment_fixture.zip"
    with zipfile.ZipFile(comment_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.comment = b"central directory comment"
        zf.writestr("comment_payload.txt", "comment fixture")

    dirs_zip = zip_dir / "dirs_fixture.zip"
    with zipfile.ZipFile(dirs_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("only_dir/", "")
        zf.writestr("only_dir/inside_dir.txt", "directory entry should be skipped")

    return [small_zip, empty_zip, comment_zip, dirs_zip]


def write_zip_tsv(paths, tsv_path):
    lines = []
    for index, path in enumerate(paths):
        size = path.stat().st_size
        lines.append("T\t{}\t{}\t{}\t{}\t{}".format(1000 + index, 2000 + index, path, size, 132537600000000000))
    tsv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    bench = Path(args.bench)
    work_dir = Path(args.work_dir)
    if work_dir.exists():
        shutil.rmtree(work_dir)
    work_dir.mkdir(parents=True)

    zip_dir = work_dir / "zips"
    zip_paths = create_fixtures(zip_dir)
    tsv_path = work_dir / "zip_files.tsv"
    db_path = work_dir / "zip_fixture_tests.ezdb"
    write_zip_tsv(zip_paths, tsv_path)

    build_output = run_command([str(bench), "build-zip-entries", str(tsv_path), str(db_path), "2"])
    require_field(build_output, "zip_archives_loaded", 4)
    require_field(build_output, "zip_opened_archives", 4)
    require_field(build_output, "zip_failed_archives", 0)
    require_field(build_output, "zip_entries", 4)

    info_output = run_command([str(bench), "info", str(db_path)])
    require_field(info_output, "records", 4)
    require_field(info_output, "entries", 4)
    require_field(info_output, "active_entries", 4)
    require_field(info_output, "base_entries", 4)
    require_field(info_output, "delta_entries", 0)

    index_output = run_command([str(bench), "search-v2", str(db_path), "entry", "index_9146", "10"])
    require_field(index_output, "returned", 1)
    require_contains(index_output, "config/index_9146.js")

    comment_output = run_command([str(bench), "search-v2", str(db_path), "entry", "comment_payload", "10"])
    require_field(comment_output, "returned", 1)
    require_contains(comment_output, "comment_payload.txt")

    dir_output = run_command([str(bench), "search-v2", str(db_path), "entry", "inside_dir", "10"])
    require_field(dir_output, "returned", 1)
    require_contains(dir_output, "only_dir/inside_dir.txt")

    archive_output = run_command([str(bench), "search-v2", str(db_path), "archive", "small_fixture", "10"])
    require_field(archive_output, "returned", 1)
    require_contains(archive_output, "small_fixture.zip")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
