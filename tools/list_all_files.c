#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct RootFilter {
    wchar_t drive;
    wchar_t* prefix;
    size_t prefix_len;
} RootFilter;

typedef struct ScanOptions {
    wchar_t* output_path;
    wchar_t* suffix;
    uint64_t limit;
    int has_limit;
    RootFilter* filters;
    size_t filter_count;
    size_t filter_cap;
} ScanOptions;

typedef struct ScanContext {
    FILE* output;
    const wchar_t* suffix;
    size_t suffix_len;
    uint64_t written;
    uint64_t limit;
    int has_limit;
} ScanContext;

static void print_usage(void)
{
    fwprintf(stderr, L"Usage:\n");
    fwprintf(stderr, L"  ListAllFiles [-o output.tsv] [-n limit] [--suffix suffix] [roots...]\n");
}

static int enable_privilege(const wchar_t* privilege_name)
{
    HANDLE token = NULL;
    LUID luid;
    TOKEN_PRIVILEGES tp;
    BOOL ok = FALSE;
    DWORD error = ERROR_SUCCESS;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return 0;
    }
    if (!LookupPrivilegeValueW(NULL, privilege_name, &luid)) {
        CloseHandle(token);
        return 0;
    }

    memset(&tp, 0, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    error = GetLastError();
    CloseHandle(token);
    return ok && error == ERROR_SUCCESS;
}

static wchar_t upper_drive(wchar_t c)
{
    if (c >= L'a' && c <= L'z') return (wchar_t)(c - L'a' + L'A');
    return c;
}

static int parse_u64(const wchar_t* text, uint64_t* out)
{
    wchar_t* end = NULL;
    unsigned long long value = 0;

    if (!text || text[0] == L'\0' || text[0] == L'-') return 0;
    value = wcstoull(text, &end, 10);
    if (!end || *end != L'\0') return 0;
    *out = (uint64_t)value;
    return 1;
}

static wchar_t* xwcsdup(const wchar_t* text)
{
    size_t len = wcslen(text);
    wchar_t* copy = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) return NULL;
    memcpy(copy, text, (len + 1) * sizeof(wchar_t));
    return copy;
}

static int ensure_filter_capacity(ScanOptions* opts)
{
    RootFilter* next = NULL;
    size_t next_cap = 0;

    if (opts->filter_count < opts->filter_cap) return 1;
    next_cap = opts->filter_cap ? opts->filter_cap * 2 : 8;
    next = (RootFilter*)realloc(opts->filters, next_cap * sizeof(RootFilter));
    if (!next) return 0;
    opts->filters = next;
    opts->filter_cap = next_cap;
    return 1;
}

static void trim_trailing_slashes(wchar_t* path)
{
    size_t len = wcslen(path);
    while (len > 3 && (path[len - 1] == L'\\' || path[len - 1] == L'/')) {
        path[--len] = L'\0';
    }
}

static wchar_t* normalize_full_path(const wchar_t* input)
{
    DWORD need = GetFullPathNameW(input, 0, NULL, NULL);
    wchar_t* path = NULL;

    if (need == 0) return NULL;
    path = (wchar_t*)malloc((size_t)need * sizeof(wchar_t));
    if (!path) return NULL;
    if (GetFullPathNameW(input, need, path, NULL) == 0) {
        free(path);
        return NULL;
    }
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'/') *p = L'\\';
    }
    trim_trailing_slashes(path);
    return path;
}

static int add_filter(ScanOptions* opts, wchar_t drive, wchar_t* prefix)
{
    RootFilter* filter = NULL;
    drive = upper_drive(drive);

    for (size_t i = 0; i < opts->filter_count; ++i) {
        RootFilter* existing = &opts->filters[i];
        if (existing->drive != drive) continue;
        if (!existing->prefix || !prefix) {
            free(existing->prefix);
            existing->prefix = NULL;
            existing->prefix_len = 0;
            free(prefix);
            return 1;
        }
        if (_wcsicmp(existing->prefix, prefix) == 0) {
            free(prefix);
            return 1;
        }
    }

    if (!ensure_filter_capacity(opts)) {
        free(prefix);
        return 0;
    }
    filter = &opts->filters[opts->filter_count++];
    filter->drive = drive;
    filter->prefix = prefix;
    filter->prefix_len = prefix ? wcslen(prefix) : 0;
    return 1;
}

static int add_root_argument(ScanOptions* opts, const wchar_t* root)
{
    wchar_t* full_path = normalize_full_path(root);
    DWORD attrs = 0;
    wchar_t drive = 0;

    if (!full_path || wcslen(full_path) < 2 || full_path[1] != L':') {
        fwprintf(stderr, L"Skip invalid root: %ls\n", root);
        free(full_path);
        return 1;
    }

    attrs = GetFileAttributesW(full_path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"Skip missing root: %ls\n", full_path);
        free(full_path);
        return 1;
    }

    drive = upper_drive(full_path[0]);
    if (full_path[2] == L'\\' && full_path[3] == L'\0') {
        free(full_path);
        full_path = NULL;
    }
    return add_filter(opts, drive, full_path);
}

static int add_all_windows_drives(ScanOptions* opts)
{
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (mask & (1u << i)) {
            if (!add_filter(opts, (wchar_t)(L'A' + i), NULL)) return 0;
        }
    }
    return 1;
}

static int parse_args(int argc, wchar_t** argv, ScanOptions* opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->output_path = xwcsdup(L"all_files.txt");
    if (!opts->output_path) return 0;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"-o") == 0 || wcscmp(argv[i], L"--output") == 0) {
            if (++i >= argc) {
                fwprintf(stderr, L"Missing value for --output\n");
                return 0;
            }
            free(opts->output_path);
            opts->output_path = xwcsdup(argv[i]);
            if (!opts->output_path) return 0;
        } else if (wcscmp(argv[i], L"-n") == 0 || wcscmp(argv[i], L"--limit") == 0) {
            if (++i >= argc) {
                fwprintf(stderr, L"Missing value for --limit\n");
                return 0;
            }
            if (!parse_u64(argv[i], &opts->limit)) {
                fwprintf(stderr, L"limit must be greater than or equal to 0\n");
                return 0;
            }
            opts->has_limit = 1;
        } else if (wcscmp(argv[i], L"--suffix") == 0) {
            if (++i >= argc) {
                fwprintf(stderr, L"Missing value for --suffix\n");
                return 0;
            }
            if (argv[i][0] == L'\0') {
                fwprintf(stderr, L"suffix must not be empty\n");
                return 0;
            }
            free(opts->suffix);
            opts->suffix = xwcsdup(argv[i]);
            if (!opts->suffix) return 0;
        } else if (argv[i][0] == L'-') {
            fwprintf(stderr, L"Unknown option: %ls\n", argv[i]);
            return 0;
        } else {
            if (!add_root_argument(opts, argv[i])) return 0;
        }
    }

    if (opts->filter_count == 0 && !add_all_windows_drives(opts)) return 0;
    return 1;
}

static void free_options(ScanOptions* opts)
{
    for (size_t i = 0; i < opts->filter_count; ++i) {
        free(opts->filters[i].prefix);
    }
    free(opts->filters);
    free(opts->suffix);
    free(opts->output_path);
}

static int ensure_parent_directory(const wchar_t* file_path)
{
    wchar_t* full_path = normalize_full_path(file_path);
    wchar_t* slash = NULL;

    if (!full_path) return 0;
    slash = wcsrchr(full_path, L'\\');
    if (!slash) {
        free(full_path);
        return 1;
    }
    *slash = L'\0';
    if (wcslen(full_path) > 3) {
        wchar_t* p = full_path;
        if (p[1] == L':' && p[2] == L'\\') p += 3;
        for (; *p; ++p) {
            if (*p == L'\\') {
                *p = L'\0';
                CreateDirectoryW(full_path, NULL);
                *p = L'\\';
            }
        }
        CreateDirectoryW(full_path, NULL);
    }
    free(full_path);
    return 1;
}

static int is_ntfs_drive(wchar_t drive)
{
    wchar_t root[] = L"X:\\";
    wchar_t fs_name[MAX_PATH];
    root[0] = drive;
    memset(fs_name, 0, sizeof(fs_name));
    if (!GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs_name, MAX_PATH)) return 0;
    return _wcsicmp(fs_name, L"NTFS") == 0;
}

static uint64_t filetime_to_unix_seconds(FILETIME ft)
{
    ULARGE_INTEGER value;
    const uint64_t windows_to_unix_seconds = 11644473600ULL;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    if (value.QuadPart < windows_to_unix_seconds * 10000000ULL) return 0;
    return (value.QuadPart / 10000000ULL) - windows_to_unix_seconds;
}

static wchar_t* get_path_by_handle(HANDLE h_file)
{
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD need = GetFinalPathNameByHandleW(h_file, NULL, 0, flags);
    wchar_t* path = NULL;
    DWORD got = 0;

    if (need == 0) return NULL;
    path = (wchar_t*)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!path) return NULL;
    got = GetFinalPathNameByHandleW(h_file, path, need + 1, flags);
    if (got == 0 || got > need) {
        free(path);
        return NULL;
    }
    path[got] = L'\0';
    if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
        memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(wchar_t));
    }
    return path;
}

static int path_matches_filters(wchar_t drive, const wchar_t* path, const RootFilter* filters, size_t filter_count)
{
    drive = upper_drive(drive);
    for (size_t i = 0; i < filter_count; ++i) {
        const RootFilter* filter = &filters[i];
        if (filter->drive != drive) continue;
        if (!filter->prefix) return 1;
        if (_wcsnicmp(path, filter->prefix, filter->prefix_len) == 0) {
            wchar_t next = path[filter->prefix_len];
            if (next == L'\0' || next == L'\\') return 1;
        }
    }
    return 0;
}

static int path_matches_suffix(const wchar_t* path, const wchar_t* suffix, size_t suffix_len)
{
    size_t path_len = 0;

    if (!suffix) return 1;
    path_len = wcslen(path);
    if (path_len < suffix_len) return 0;
    return _wcsicmp(path + path_len - suffix_len, suffix) == 0;
}

static wchar_t lower_ascii_wchar(wchar_t c)
{
    if (c >= L'A' && c <= L'Z') return (wchar_t)(c - L'A' + L'a');
    return c;
}

static int name_matches_suffix(const wchar_t* name, size_t name_len,
                               const wchar_t* suffix, size_t suffix_len)
{
    size_t start = 0;

    if (!suffix) return 1;
    if (name_len < suffix_len) return 0;
    start = name_len - suffix_len;
    for (size_t i = 0; i < suffix_len; ++i) {
        if (lower_ascii_wchar(name[start + i]) != lower_ascii_wchar(suffix[i])) return 0;
    }
    return 1;
}

static int write_utf8(FILE* output, const wchar_t* text)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char* buffer = NULL;
    int ok = 0;

    if (need <= 0) return 0;
    buffer = (char*)malloc((size_t)need);
    if (!buffer) return 0;
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, need, NULL, NULL) > 0) {
        ok = fwrite(buffer, 1, (size_t)need - 1, output) == (size_t)need - 1;
    }
    free(buffer);
    return ok;
}

static int write_record(ScanContext* ctx, wchar_t drive, uint64_t file_ref, int64_t usn,
                        const wchar_t* path, uint64_t file_size, uint64_t modified_time)
{
    if (ctx->has_limit && ctx->written >= ctx->limit) return 0;
    if (fprintf(ctx->output, "%c\t%llu\t%lld\t",
                (char)drive,
                (unsigned long long)file_ref,
                (long long)usn) < 0) {
        return 0;
    }
    if (!write_utf8(ctx->output, path)) return 0;
    if (fprintf(ctx->output, "\t%llu\t%llu\n",
                (unsigned long long)file_size,
                (unsigned long long)modified_time) < 0) {
        return 0;
    }
    ++ctx->written;
    return 1;
}

static int get_file_info_by_ref(HANDLE h_vol, uint64_t file_ref, uint64_t* out_size,
                                uint64_t* out_mtime, wchar_t** out_path)
{
    FILE_ID_DESCRIPTOR fid;
    HANDLE h_file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION info;

    memset(&fid, 0, sizeof(fid));
    fid.dwSize = sizeof(fid);
    fid.Type = FileIdType;
    fid.FileId.QuadPart = (LONGLONG)file_ref;

    h_file = OpenFileById(h_vol, &fid, FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, 0);
    if (h_file == INVALID_HANDLE_VALUE) return 0;

    memset(&info, 0, sizeof(info));
    if (!GetFileInformationByHandle(h_file, &info)) {
        CloseHandle(h_file);
        return 0;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        CloseHandle(h_file);
        return 0;
    }

    *out_size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    *out_mtime = filetime_to_unix_seconds(info.ftLastWriteTime);
    *out_path = get_path_by_handle(h_file);
    CloseHandle(h_file);
    return *out_path != NULL;
}

static int drive_has_filter(wchar_t drive, const RootFilter* filters, size_t filter_count)
{
    drive = upper_drive(drive);
    for (size_t i = 0; i < filter_count; ++i) {
        if (filters[i].drive == drive) return 1;
    }
    return 0;
}

static int scan_drive(wchar_t drive, const RootFilter* filters, size_t filter_count, ScanContext* ctx)
{
    wchar_t volume_path[] = L"\\\\.\\X:";
    HANDLE h_vol = INVALID_HANDLE_VALUE;
    USN_JOURNAL_DATA_V0 journal;
    MFT_ENUM_DATA_V0 med;
    BYTE* buffer = NULL;
    DWORD bytes = 0;
    int success = 1;

    if (!drive_has_filter(drive, filters, filter_count)) return 1;
    if (!is_ntfs_drive(drive)) {
        fwprintf(stderr, L"Skip non-NTFS drive: %c:\n", drive);
        return 1;
    }

    volume_path[4] = drive;
    h_vol = CreateFileW(volume_path, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (h_vol == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            fwprintf(stderr, L"Skip drive %c: access denied, run as administrator for USN scan\n", drive);
        } else {
            fwprintf(stderr, L"Skip drive %c: CreateFileW failed (%lu)\n", drive, error);
        }
        return 1;
    }

    memset(&journal, 0, sizeof(journal));
    if (!DeviceIoControl(h_vol, FSCTL_QUERY_USN_JOURNAL, NULL, 0,
                         &journal, sizeof(journal), &bytes, NULL)) {
        fwprintf(stderr, L"Skip drive %c: FSCTL_QUERY_USN_JOURNAL failed (%lu)\n", drive, GetLastError());
        CloseHandle(h_vol);
        return 1;
    }

    memset(&med, 0, sizeof(med));
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = journal.NextUsn;

    buffer = (BYTE*)malloc(1u << 20);
    if (!buffer) {
        CloseHandle(h_vol);
        return 0;
    }

    for (;;) {
        const USN* next_file_ref = NULL;
        DWORD offset = sizeof(USN);

        if (ctx->has_limit && ctx->written >= ctx->limit) break;
        bytes = 0;
        if (!DeviceIoControl(h_vol, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                             buffer, 1u << 20, &bytes, NULL)) {
            DWORD error = GetLastError();
            if (error == ERROR_HANDLE_EOF) break;
            fwprintf(stderr, L"Drive %c: FSCTL_ENUM_USN_DATA failed (%lu)\n", drive, error);
            success = 0;
            break;
        }
        if (bytes < sizeof(USN)) break;

        next_file_ref = (const USN*)buffer;
        while (offset + sizeof(USN_RECORD) <= bytes) {
            const USN_RECORD* rec = (const USN_RECORD*)(buffer + offset);
            size_t file_name_len = 0;
            uint64_t file_size = 0;
            uint64_t modified_time = 0;
            wchar_t* path = NULL;

            if (rec->RecordLength == 0 || offset + rec->RecordLength > bytes) break;
            file_name_len = rec->FileNameLength / sizeof(wchar_t);
            if ((rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                name_matches_suffix(rec->FileName, file_name_len, ctx->suffix, ctx->suffix_len) &&
                get_file_info_by_ref(h_vol, rec->FileReferenceNumber, &file_size, &modified_time, &path)) {
                if (path_matches_filters(drive, path, filters, filter_count) &&
                    path_matches_suffix(path, ctx->suffix, ctx->suffix_len)) {
                    if (!write_record(ctx, drive, rec->FileReferenceNumber, rec->Usn,
                                      path, file_size, modified_time)) {
                        free(path);
                        success = ferror(ctx->output) ? 0 : 1;
                        goto done;
                    }
                }
                free(path);
            }

            offset += rec->RecordLength;
            if (ctx->has_limit && ctx->written >= ctx->limit) break;
        }
        med.StartFileReferenceNumber = *next_file_ref;
    }

done:
    free(buffer);
    CloseHandle(h_vol);
    return success;
}

int wmain(int argc, wchar_t** argv)
{
    ScanOptions opts;
    ScanContext ctx;
    int exit_code = 0;

    if (!parse_args(argc, argv, &opts)) {
        print_usage();
        free_options(&opts);
        return 2;
    }

    enable_privilege(L"SeBackupPrivilege");
    enable_privilege(L"SeManageVolumePrivilege");

    if (!ensure_parent_directory(opts.output_path)) {
        fwprintf(stderr, L"Failed to prepare output directory: %ls\n", opts.output_path);
        free_options(&opts);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.suffix = opts.suffix;
    ctx.suffix_len = opts.suffix ? wcslen(opts.suffix) : 0;
    ctx.limit = opts.limit;
    ctx.has_limit = opts.has_limit;
    ctx.output = _wfopen(opts.output_path, L"wb");
    if (!ctx.output) {
        fwprintf(stderr, L"Failed to open output: %ls\n", opts.output_path);
        free_options(&opts);
        return 1;
    }

    for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
        if (ctx.has_limit && ctx.written >= ctx.limit) break;
        if (!scan_drive(drive, opts.filters, opts.filter_count, &ctx)) {
            exit_code = 1;
            break;
        }
    }

    if (fclose(ctx.output) != 0) exit_code = 1;
    wprintf(L"Saved %llu records to: %ls\n", (unsigned long long)ctx.written, opts.output_path);
    free_options(&opts);
    return exit_code;
}
