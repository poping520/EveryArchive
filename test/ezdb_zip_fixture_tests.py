import argparse
import binascii
import re
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path


def run_command(args):
    proc = subprocess.run(args, text=True, capture_output=True, encoding="utf-8", errors="replace")
    output = (proc.stdout or "") + (proc.stderr or "")
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


def extract_int_field(output, name):
    match = re.search(r"^{}:\s+([0-9]+)$".format(re.escape(name)), output, re.MULTILINE)
    if not match:
        raise AssertionError("missing integer field '{}'\n{}".format(name, output))
    return int(match.group(1))


def stable_result_lines(output):
    lines = []
    for line in output.splitlines():
        if line.startswith("[archive:") or line.startswith("[entry:") or re.match(r"^\[[0-9]+ archive:", line):
            lines.append(line)
    return lines


def require_temp_dir_removed(db_path):
    temp_dir = Path(str(db_path) + ".tmp")
    if temp_dir.exists():
        raise AssertionError("temporary directory was not removed: {}".format(temp_dir))


def dos_date_time():
    return 0x5B21, 0x0000


def create_stored_zip(path, raw_name, payload, utf8_flag=False, force_zip64=False):
    flags = 0x800 if utf8_flag else 0
    method = 0
    mod_date, mod_time = dos_date_time()
    crc = binascii.crc32(payload) & 0xFFFFFFFF
    comp_size = len(payload)
    uncomp_size = len(payload)

    local_extra = b""
    local_comp_size = comp_size
    local_uncomp_size = uncomp_size
    cd_extra = b""
    cd_comp_size = comp_size
    cd_uncomp_size = uncomp_size
    if force_zip64:
        local_extra = struct.pack("<HHQQ", 0x0001, 16, uncomp_size, comp_size)
        local_comp_size = 0xFFFFFFFF
        local_uncomp_size = 0xFFFFFFFF
        cd_extra = local_extra
        cd_comp_size = 0xFFFFFFFF
        cd_uncomp_size = 0xFFFFFFFF

    local_header = struct.pack(
        "<IHHHHHIIIHH",
        0x04034B50,
        45 if force_zip64 else 20,
        flags,
        method,
        mod_time,
        mod_date,
        crc,
        local_comp_size,
        local_uncomp_size,
        len(raw_name),
        len(local_extra),
    )
    local_offset = 0
    body = local_header + raw_name + local_extra + payload

    cd_offset = len(body)
    central_header = struct.pack(
        "<IHHHHHHIIIHHHHHII",
        0x02014B50,
        45 << 8,
        45 if force_zip64 else 20,
        flags,
        method,
        mod_time,
        mod_date,
        crc,
        cd_comp_size,
        cd_uncomp_size,
        len(raw_name),
        len(cd_extra),
        0,
        0,
        0,
        0,
        local_offset,
    )
    central_dir = central_header + raw_name + cd_extra
    cd_size = len(central_dir)

    if force_zip64:
        zip64_eocd_offset = cd_offset + cd_size
        zip64_eocd = struct.pack(
            "<IQHHIIQQQQ",
            0x06064B50,
            44,
            45,
            45,
            0,
            0,
            1,
            1,
            cd_size,
            cd_offset,
        )
        zip64_locator = struct.pack("<IIQI", 0x07064B50, 0, zip64_eocd_offset, 1)
        eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0)
        path.write_bytes(body + central_dir + zip64_eocd + zip64_locator + eocd)
    else:
        eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 1, 1, cd_size, cd_offset, 0)
        path.write_bytes(body + central_dir + eocd)


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

    zip64_zip = zip_dir / "zip64_fixture.zip"
    create_stored_zip(zip64_zip, b"zip64_payload.txt", b"zip64 fixture", utf8_flag=True, force_zip64=True)

    non_utf8_zip = zip_dir / "non_utf8_fixture.zip"
    create_stored_zip(non_utf8_zip, "中文_payload.txt".encode("mbcs"), b"non utf8 fixture", utf8_flag=False)

    return [small_zip, empty_zip, comment_zip, dirs_zip, zip64_zip, non_utf8_zip]


def write_zip_tsv(paths, tsv_path):
    lines = []
    for index, path in enumerate(paths):
        size = path.stat().st_size if path.exists() else 0
        lines.append("T\t{}\t{}\t{}\t{}\t{}".format(1000 + index, 2000 + index, path, size, 132537600000000000))
    tsv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_fixture_db(bench, tsv_path, db_path, expected_archives=6, expected_opened=6, expected_failed=0, expected_entries=6):
    build_output = run_command([str(bench), "build-zip-entries", str(tsv_path), str(db_path), "2"])
    require_field(build_output, "zip_archives_loaded", expected_archives)
    require_field(build_output, "zip_opened_archives", expected_opened)
    require_field(build_output, "zip_failed_archives", expected_failed)
    require_field(build_output, "zip_entries", expected_entries)
    require_temp_dir_removed(db_path)
    return build_output


def collect_deterministic_probe(bench, db_path):
    probes = {}
    info_output = run_command([str(bench), "info", str(db_path)])
    for name in ("records", "entries", "active_entries", "base_entries", "delta_entries"):
        probes[name] = extract_int_field(info_output, name)

    for name, args in {
        "entry_index": ["search-v2", str(db_path), "entry", "index_9146", "10"],
        "entry_comment": ["search-v2", str(db_path), "entry", "comment_payload", "10"],
        "entry_zip64": ["search-v2", str(db_path), "entry", "zip64_payload", "10"],
        "entry_non_utf8": ["search-v2", str(db_path), "entry", "中文_payload", "10"],
        "archive_small": ["search-v2", str(db_path), "archive", "small_fixture", "10"],
        "query_entries": ["query-entries", str(db_path), "entry", "index_9146", "0", "10"],
        "wildcard": ["search-v2", str(db_path), "entry", "*_9146.js", "10"],
    }.items():
        output = run_command([str(bench)] + args)
        probes[name] = {
            "returned": extract_int_field(output, "returned"),
            "results": stable_result_lines(output),
        }
        if name == "query_entries":
            probes[name]["total"] = extract_int_field(output, "total")
    return probes


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
    second_db_path = work_dir / "zip_fixture_tests_second.ezdb"
    write_zip_tsv(zip_paths, tsv_path)

    build_fixture_db(bench, tsv_path, db_path)
    first_probe = collect_deterministic_probe(bench, db_path)

    info_output = run_command([str(bench), "info", str(db_path)])
    require_field(info_output, "records", 6)
    require_field(info_output, "entries", 6)
    require_field(info_output, "active_entries", 6)
    require_field(info_output, "base_entries", 6)
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

    zip64_output = run_command([str(bench), "search-v2", str(db_path), "entry", "zip64_payload", "10"])
    require_field(zip64_output, "returned", 1)
    require_contains(zip64_output, "zip64_payload.txt")

    non_utf8_output = run_command([str(bench), "search-v2", str(db_path), "entry", "中文_payload", "10"])
    require_field(non_utf8_output, "returned", 1)
    require_contains(non_utf8_output, "中文_payload.txt")
    require_contains(non_utf8_output, "raw=")

    archive_output = run_command([str(bench), "search-v2", str(db_path), "archive", "small_fixture", "10"])
    require_field(archive_output, "returned", 1)
    require_contains(archive_output, "small_fixture.zip")

    get_entry_output = run_command([str(bench), "get-entry", str(db_path), "1"])
    require_contains(get_entry_output, "config/index_9146.js")

    query_entries_output = run_command([str(bench), "query-entries", str(db_path), "entry", "index_9146", "0", "10"])
    require_field(query_entries_output, "total", 1)
    require_field(query_entries_output, "returned", 1)
    require_contains(query_entries_output, "config/index_9146.js")

    wildcard_output = run_command([str(bench), "search-v2", str(db_path), "entry", "*_9146.js", "10"])
    require_field(wildcard_output, "returned", 1)
    require_contains(wildcard_output, "config/index_9146.js")

    insert_output = run_command(
        [str(bench), "insert", str(db_path), "T:\\stage_e_insert_target.zip", "1234", "132537600000000001"]
    )
    inserted_id = extract_int_field(insert_output, "insert_id")
    inserted_search = run_command([str(bench), "search-v2", str(db_path), "archive", "stage_e_insert_target", "10"])
    require_field(inserted_search, "returned", 1)
    require_contains(inserted_search, "stage_e_insert_target.zip")

    run_command(
        [
            str(bench),
            "update",
            str(db_path),
            str(inserted_id),
            "T:\\stage_e_update_target.zip",
            "5678",
            "132537600000000002",
        ]
    )
    updated_search = run_command([str(bench), "search-v2", str(db_path), "archive", "stage_e_update_target", "10"])
    require_field(updated_search, "returned", 1)
    require_contains(updated_search, "stage_e_update_target.zip")
    old_search = run_command([str(bench), "search-v2", str(db_path), "archive", "stage_e_insert_target", "10"])
    require_field(old_search, "returned", 0)

    run_command([str(bench), "delete", str(db_path), str(inserted_id)])
    deleted_search = run_command([str(bench), "search-v2", str(db_path), "archive", "stage_e_update_target", "10"])
    require_field(deleted_search, "returned", 0)

    compact_output = run_command([str(bench), "compact", str(db_path)])
    require_contains(compact_output, "compact_ms:")
    post_compact_entry = run_command([str(bench), "search-v2", str(db_path), "entry", "index_9146", "10"])
    require_field(post_compact_entry, "returned", 1)
    require_contains(post_compact_entry, "small_fixture.zip")
    require_contains(post_compact_entry, "config/index_9146.js")

    build_fixture_db(bench, tsv_path, second_db_path)
    second_probe = collect_deterministic_probe(bench, second_db_path)
    if first_probe != second_probe:
        raise AssertionError("deterministic probes differ\nfirst={}\nsecond={}".format(first_probe, second_probe))

    missing_tsv_path = work_dir / "missing_zip_files.tsv"
    missing_db_path = work_dir / "missing_zip_fixture.ezdb"
    stale_temp_dir = Path(str(missing_db_path) + ".tmp")
    stale_temp_dir.mkdir()
    (stale_temp_dir / "stale.spool").write_text("stale", encoding="utf-8")
    missing_zip_path = zip_dir / "missing_fixture.zip"
    write_zip_tsv([missing_zip_path], missing_tsv_path)
    missing_output = build_fixture_db(
        bench,
        missing_tsv_path,
        missing_db_path,
        expected_archives=1,
        expected_opened=0,
        expected_failed=1,
        expected_entries=0,
    )
    require_contains(missing_output, "zip_parse_error[0]:")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(exc, file=sys.stderr)
        sys.exit(1)
