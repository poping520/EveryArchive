#include "../src/ezdb/ezdb.h"

#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ioapi.h"
#include "iowin32.h"
#include "unzip.h"
#include "zip_cd_scanner.h"

typedef struct SearchStats {
    uint32_t printed;
    uint32_t total;
} SearchStats;

typedef struct LoadedArchives {
    EzdbArchiveRecord* records;
    uint32_t count;
    uint32_t cap;
} LoadedArchives;

static char* trim_ascii(char* text);

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    static int initialized = 0;
    LARGE_INTEGER now;
    if (!initialized) {
        QueryPerformanceFrequency(&freq);
        initialized = 1;
    }
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static double ms_to_seconds(double ms)
{
    return ms / 1000.0;
}

static double bytes_to_mb(uint64_t bytes)
{
    return (double)bytes / 1024.0 / 1024.0;
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  EzdbBench build-archives <input.tsv> <output.ezdb>\n");
    printf("  EzdbBench build-entries <combined.tsv> <output.ezdb>\n");
    printf("  EzdbBench build-zip-entries <zip_files.tsv> <output.ezdb> [threads]\n");
    printf("  EzdbBench live-entry-append <output.ezdb> <entry_count> [batch_size]\n");
    printf("  EzdbBench live-entry-append-batch <combined.tsv> <output.ezdb> [batch_size]\n");

    printf("  EzdbBench info <db.ezdb>\n");
    printf("  EzdbBench open <db.ezdb> [limit]\n");
    printf("  EzdbBench get <db.ezdb> <id>\n");
    printf("  EzdbBench get-archive <db.ezdb> <id>\n");
    printf("  EzdbBench get-entry <db.ezdb> <id>\n");
    printf("  EzdbBench search <db.ezdb> <keyword> [limit]\n");
    printf("  EzdbBench search-v2 <db.ezdb> <scope> <keyword> [limit]\n");
    printf("  EzdbBench insert <db.ezdb> <path> [size] [mtime]\n");
    printf("  EzdbBench update <db.ezdb> <id> <path> [size] [mtime]\n");
    printf("  EzdbBench delete <db.ezdb> <id>\n");
    printf("  EzdbBench delete-archive-ref <db.ezdb> <drive> <file_ref_number>\n");
    printf("  EzdbBench compact <db.ezdb>\n");

}

static void print_memory_usage(const char* prefix)
{
    PROCESS_MEMORY_COUNTERS_EX counters;
    memset(&counters, 0, sizeof(counters));
    counters.cb = sizeof(counters);

    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&counters, sizeof(counters))) {
        printf("%s_memory_error: %lu\n", prefix, GetLastError());
        return;
    }

    printf("%s_working_set_mb: %.2f\n", prefix, (double)counters.WorkingSetSize / 1024.0 / 1024.0);
    printf("%s_peak_working_set_mb: %.2f\n", prefix, (double)counters.PeakWorkingSetSize / 1024.0 / 1024.0);
    printf("%s_private_mb: %.2f\n", prefix, (double)counters.PrivateUsage / 1024.0 / 1024.0);
}

static uint64_t file_size_of_path(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) return 0;
    return ((uint64_t)data.nFileSizeHigh << 32u) | data.nFileSizeLow;
}

static void on_result(const EzdbSearchResult* result, void* user_data)
{
    SearchStats* stats = (SearchStats*)user_data;
    ++stats->total;
    if (stats->printed < 20) {
        printf("[%u] %s, %llu, %llu\n",
               result->id,
               result->path,
               (unsigned long long)result->size,
               (unsigned long long)result->modified_time);
        ++stats->printed;
    }
}

static void on_v2_result(const EzdbSearchV2Result* result, void* user_data)
{
    SearchStats* stats = (SearchStats*)user_data;
    ++stats->total;
    if (stats->printed < 20) {
        if (result->kind == EZDB_RESULT_ARCHIVE) {
            printf("[archive:%u] %c %llu %lld %s, %llu, %llu\n",
                   result->id,
                   result->drive_letter ? result->drive_letter : '-',
                   (unsigned long long)result->file_ref_number,
                   (long long)result->usn,
                   result->archive_path ? result->archive_path : "",
                   (unsigned long long)result->file_size,
                   (unsigned long long)result->modified_time);
        } else {
            printf("[entry:%u archive:%u] %s :: %s, %lld, %llu, %llu raw=%u\n",
                   result->id,
                   result->archive_id,
                   result->archive_path ? result->archive_path : "",
                   result->entry_path ? result->entry_path : "",
                   (long long)result->compressed_size,
                   (unsigned long long)result->original_size,
                   (unsigned long long)result->modified_time,
                   result->entry_raw_path_len);
        }
        ++stats->printed;
    }
}

static uint32_t parse_scope_arg(const char* text)
{
    if (!text || strcmp(text, "archive") == 0 || strcmp(text, "archives") == 0) return EZDB_SEARCH_ARCHIVE_PATH;
    if (strcmp(text, "entry") == 0 || strcmp(text, "entries") == 0) return EZDB_SEARCH_ENTRY_PATH;
    if (strcmp(text, "combined") == 0 || strcmp(text, "combo") == 0) return EZDB_SEARCH_COMBINED_PATH;
    if (strcmp(text, "all") == 0) return EZDB_SEARCH_ALL;
    return (uint32_t)strtoul(text, NULL, 0);
}

static int run_search_once(Ezdb* db, const char* keyword, uint32_t limit, const char* memory_prefix)
{
    SearchStats stats;
    memset(&stats, 0, sizeof(stats));
    double search_start = now_ms();
    int rc = ezdb_search_path(db, keyword, limit, on_result, &stats);
    double search_elapsed = now_ms() - search_start;
    if (rc != 0) {
        fprintf(stderr, "search failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("search_ms: %.2f\n", search_elapsed);
    printf("returned: %u\n", stats.total);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_search_v2_once(Ezdb* db, const char* keyword, uint32_t scope, uint32_t limit, const char* memory_prefix)
{
    SearchStats stats;
    memset(&stats, 0, sizeof(stats));
    double search_start = now_ms();
    int rc = ezdb_search(db, keyword, scope, limit, on_v2_result, &stats);
    double search_elapsed = now_ms() - search_start;
    if (rc != 0) {
        fprintf(stderr, "search-v2 failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("search_ms: %.2f\n", search_elapsed);
    printf("returned: %u\n", stats.total);
    print_memory_usage(memory_prefix);
    return 0;
}

static uint64_t parse_u64_arg(const char* text, uint64_t fallback)
{
    if (!text) return fallback;
    return (uint64_t)_strtoui64(text, NULL, 10);
}

static void free_loaded_archives(LoadedArchives* loaded)
{
    if (!loaded) return;
    if (loaded->records) {
        for (uint32_t i = 0; i < loaded->count; ++i) free((void*)loaded->records[i].file_path);
    }
    free(loaded->records);
    loaded->records = NULL;
    loaded->count = 0;
    loaded->cap = 0;
}

static int push_loaded_archive(LoadedArchives* loaded, const EzdbArchiveRecord* record)
{
    if (loaded->count == loaded->cap) {
        uint32_t next = loaded->cap ? loaded->cap * 2u : 1024u;
        EzdbArchiveRecord* records = (EzdbArchiveRecord*)realloc(loaded->records, sizeof(EzdbArchiveRecord) * (size_t)next);
        if (!records) return 0;
        loaded->records = records;
        loaded->cap = next;
    }
    loaded->records[loaded->count] = *record;
    loaded->records[loaded->count].file_path = _strdup(record->file_path);
    if (!loaded->records[loaded->count].file_path) return 0;
    ++loaded->count;
    return 1;
}

static int parse_archive_tsv_line_for_bench(char* line, EzdbArchiveRecord* out_record)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';

    char* fields[6];
    char* cursor = line;
    for (int i = 0; i < 6; ++i) {
        fields[i] = cursor;
        if (i < 5) {
            char* tab = strchr(cursor, '\t');
            if (!tab) return 0;
            *tab = '\0';
            cursor = tab + 1;
        }
    }
    if (!fields[3][0]) return 0;
    memset(out_record, 0, sizeof(*out_record));
    out_record->drive_letter = fields[0][0] ? fields[0][0] : 0;
    out_record->file_ref_number = (uint64_t)_strtoui64(fields[1], NULL, 10);
    out_record->usn = (int64_t)_strtoi64(fields[2], NULL, 10);
    out_record->file_path = fields[3];
    out_record->file_size = (uint64_t)_strtoui64(fields[4], NULL, 10);
    out_record->modified_time = (uint64_t)_strtoui64(fields[5], NULL, 10);
    return 1;
}

static int load_archive_tsv(const char* input_tsv, LoadedArchives* loaded)
{
    FILE* in = fopen(input_tsv, "rb");
    if (!in) return 0;
    char line[32768];
    while (fgets(line, sizeof(line), in)) {
        EzdbArchiveRecord archive_record;
        if (!parse_archive_tsv_line_for_bench(line, &archive_record)) continue;
        if (!push_loaded_archive(loaded, &archive_record)) {
            fclose(in);
            return 0;
        }
    }
    fclose(in);
    return 1;
}

static int build_from_loaded_archives(LoadedArchives* loaded, const char* output_ezdb, const char* error_prefix)
{
    int rc = ezdb_build_snapshot(loaded->records, loaded->count, NULL, 0, output_ezdb);
    if (rc != 0) {
        fprintf(stderr, "%s failed: %s (%d)\n", error_prefix, ezdb_error_message(rc), rc);
        return 2;
    }
    return 0;
}

static char* wide_to_utf8(const wchar_t* text)
{
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) return NULL;
    char* out = (char*)malloc((size_t)needed);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, out, needed, NULL, NULL) != needed) {
        free(out);
        return NULL;
    }
    return out;
}

static void free_utf8_argv(int argc, char** argv)
{
    if (!argv) return;
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
}

static int make_utf8_argv(int* out_argc, char*** out_argv)
{
    int argc = 0;
    wchar_t** wide_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide_argv) return 0;
    char** argv = (char**)calloc((size_t)argc, sizeof(char*));
    if (!argv) {
        LocalFree(wide_argv);
        return 0;
    }
    for (int i = 0; i < argc; ++i) {
        argv[i] = wide_to_utf8(wide_argv[i]);
        if (!argv[i]) {
            free_utf8_argv(argc, argv);
            LocalFree(wide_argv);
            return 0;
        }
    }
    LocalFree(wide_argv);
    *out_argc = argc;
    *out_argv = argv;
    return 1;
}

static int read_console_utf8_line(char* out, size_t out_size)
{
    if (!out || !out_size) return 0;
    out[0] = '\0';

    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
        wchar_t wide[4096];
        DWORD read = 0;
        if (!ReadConsoleW(input, wide, (DWORD)(sizeof(wide) / sizeof(wide[0]) - 1u), &read, NULL)) return 0;
        wide[read] = L'\0';
        while (read > 0 && (wide[read - 1u] == L'\n' || wide[read - 1u] == L'\r')) wide[--read] = L'\0';
        int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL, NULL);
        if (needed <= 0) return 0;
        out[out_size - 1u] = '\0';
        return 1;
    }

    if (!fgets(out, (int)out_size, stdin)) return 0;
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1u] == '\n' || out[len - 1u] == '\r')) out[--len] = '\0';
    return 1;
}

static char* trim_ascii(char* text)
{
    while (*text == ' ' || *text == '\t') ++text;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1u] == ' ' || text[len - 1u] == '\t')) text[--len] = '\0';
    return text;
}

static void print_interactive_help(uint32_t default_limit)
{
    printf("Commands:\n");
    printf("  help\n");
    printf("  info\n");
    printf("  get <id>\n");
    printf("  search <keyword> [limit]\n");
    printf("  <keyword> [limit]\n");
    printf("  insert <path> [size] [mtime]\n");
    printf("  update <id> <path> [size] [mtime]\n");
    printf("  delete <id>\n");
    printf("  exit | quit\n");
    printf("Default search limit: %u\n", default_limit);
}

static char* next_token(char** cursor)
{
    char* p = *cursor;
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) {
        *cursor = p;
        return NULL;
    }
    char* start = p;
    while (*p && *p != ' ' && *p != '\t') ++p;
    if (*p) *p++ = '\0';
    *cursor = p;
    return start;
}

static int parse_interactive_query(char* line, char** out_keyword, uint32_t* out_limit, uint32_t default_limit)
{
    char* text = trim_ascii(line);
    *out_keyword = text;
    *out_limit = default_limit;
    if (!*text) return 0;
    if (strcmp(text, "exit") == 0 || strcmp(text, "quit") == 0) return -1;

    char* last_space = NULL;
    for (char* p = text; *p; ++p) {
        if (*p == ' ' || *p == '\t') last_space = p;
    }
    if (last_space) {
        char* maybe_limit = trim_ascii(last_space + 1);
        if (*maybe_limit) {
            char* end = NULL;
            unsigned long value = strtoul(maybe_limit, &end, 10);
            if (end && *end == '\0') {
                *last_space = '\0';
                *out_keyword = trim_ascii(text);
                *out_limit = (uint32_t)value;
            }
        }
    }
    return **out_keyword ? 1 : 0;
}

static int print_db_info(Ezdb* db, const char* memory_prefix)
{
    EzdbStats stats;
    int rc = ezdb_stats(db, &stats);
    if (rc != 0) {
        fprintf(stderr, "stats failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("records: %u\n", stats.record_count);
    printf("active: %u\n", stats.active_count);
        printf("entries: %u\n", stats.entry_count);
        printf("active_entries: %u\n", stats.active_entry_count);
        printf("base_entries: %u\n", stats.base_entry_count);
        printf("delta_entries: %u\n", stats.delta_entry_count);
        printf("file_size: %llu bytes\n", (unsigned long long)stats.file_size);
        printf("delta_size: %llu bytes\n", (unsigned long long)stats.delta_size);
    printf("records_size: %llu bytes\n", (unsigned long long)stats.records_size);
    printf("dirs_size: %llu bytes\n", (unsigned long long)stats.dirs_size);
    printf("names_size: %llu bytes\n", (unsigned long long)stats.names_size);
    printf("archive_meta_size: %llu bytes\n", (unsigned long long)stats.archive_meta_size);
    printf("entry_records_size: %llu bytes\n", (unsigned long long)stats.entry_records_size);
    printf("raw_blob_size: %llu bytes\n", (unsigned long long)stats.raw_blob_size);
    printf("index_size: %llu bytes\n", (unsigned long long)stats.index_size);
    printf("postings_size: %llu bytes\n", (unsigned long long)stats.postings_size);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_get_once(Ezdb* db, uint32_t id, const char* memory_prefix)
{
    EzdbSearchResult result;
    double start = now_ms();
    int rc = ezdb_get_by_id(db, id, &result);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "get failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("[%u] %s, %llu, %llu\n",
           result.id,
           result.path,
           (unsigned long long)result.size,
           (unsigned long long)result.modified_time);
    printf("get_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    ezdb_free_result(&result);
    return 0;
}

static int run_insert_once(Ezdb* db, const char* path, uint64_t size, uint64_t mtime, const char* memory_prefix)
{
    EzdbFileRecord record;
    record.path = path;
    record.size = size;
    record.modified_time = mtime;
    uint32_t id = 0;
    double start = now_ms();
    int rc = ezdb_insert(db, &record, &id);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "insert failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("insert_id: %u\n", id);
    printf("insert_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_update_once(Ezdb* db, uint32_t id, const char* path, uint64_t size, uint64_t mtime, const char* memory_prefix)
{
    EzdbFileRecord record;
    record.path = path;
    record.size = size;
    record.modified_time = mtime;
    double start = now_ms();
    int rc = ezdb_update(db, id, &record);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "update failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("update_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

static int run_delete_once(Ezdb* db, uint32_t id, const char* memory_prefix)
{
    double start = now_ms();
    int rc = ezdb_delete(db, id);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "delete failed: %s (%d)\n", ezdb_error_message(rc), rc);
        return rc;
    }
    printf("delete_ms: %.2f\n", elapsed);
    print_memory_usage(memory_prefix);
    return 0;
}

/* --- Combined TSV loading --- */

typedef struct LoadedEntry {
    uint32_t archive_index;
    char* entry_path;
    void* entry_raw_path;
    uint32_t entry_raw_path_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} LoadedEntry;

typedef struct LoadedEntries {
    LoadedEntry* entries;
    uint32_t count;
    uint32_t cap;
} LoadedEntries;

typedef struct LoadedCombined {
    LoadedArchives archives;
    LoadedEntries entries;
} LoadedCombined;

static void free_loaded_entries(LoadedEntries* loaded)
{
    if (!loaded) return;
    if (loaded->entries) {
        for (uint32_t i = 0; i < loaded->count; ++i) {
            free(loaded->entries[i].entry_path);
            free(loaded->entries[i].entry_raw_path);
        }
    }
    free(loaded->entries);
    loaded->entries = NULL;
    loaded->count = 0;
    loaded->cap = 0;
}

static void free_loaded_combined(LoadedCombined* combined)
{
    if (!combined) return;
    free_loaded_archives(&combined->archives);
    free_loaded_entries(&combined->entries);
}

static int push_loaded_entry(LoadedEntries* loaded, const LoadedEntry* entry)
{
    if (loaded->count == loaded->cap) {
        uint32_t next = loaded->cap ? loaded->cap * 2u : 4096u;
        LoadedEntry* entries = (LoadedEntry*)realloc(loaded->entries, sizeof(LoadedEntry) * (size_t)next);
        if (!entries) return 0;
        loaded->entries = entries;
        loaded->cap = next;
    }
    loaded->entries[loaded->count] = *entry;
    ++loaded->count;
    return 1;
}

static int parse_combined_archive_fields(char* cursor, EzdbArchiveRecord* out_record)
{
    /* cursor points past the 'A\t' prefix; 6 tab-separated fields remain */
    char* fields[6];
    for (int i = 0; i < 6; ++i) {
        fields[i] = cursor;
        if (i < 5) {
            char* tab = strchr(cursor, '\t');
            if (!tab) return 0;
            *tab = '\0';
            cursor = tab + 1;
        }
    }
    if (!fields[3][0]) return 0;
    memset(out_record, 0, sizeof(*out_record));
    out_record->drive_letter = fields[0][0] ? fields[0][0] : 0;
    out_record->file_ref_number = (uint64_t)_strtoui64(fields[1], NULL, 10);
    out_record->usn = (int64_t)_strtoi64(fields[2], NULL, 10);
    out_record->file_path = fields[3];
    out_record->file_size = (uint64_t)_strtoui64(fields[4], NULL, 10);
    out_record->modified_time = (uint64_t)_strtoui64(fields[5], NULL, 10);
    return 1;
}

static int parse_combined_entry_fields(char* cursor, uint32_t archive_index, LoadedEntry* out_entry)
{
    /* cursor points past the 'E\t' prefix; 4 tab-separated fields remain */
    char* fields[4];
    for (int i = 0; i < 4; ++i) {
        fields[i] = cursor;
        if (i < 3) {
            char* tab = strchr(cursor, '\t');
            if (!tab) return 0;
            *tab = '\0';
            cursor = tab + 1;
        }
    }
    if (!fields[0][0]) return 0;
    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->archive_index = archive_index;
    out_entry->entry_path = _strdup(fields[0]);
    if (!out_entry->entry_path) return 0;
    out_entry->compressed_size = (int64_t)_strtoi64(fields[1], NULL, 10);
    out_entry->original_size = (uint64_t)_strtoui64(fields[2], NULL, 10);
    out_entry->modified_time = (uint64_t)_strtoui64(fields[3], NULL, 10);
    return 1;
}

static int load_combined_tsv(const char* input_tsv, LoadedCombined* combined)
{
    FILE* in = fopen(input_tsv, "rb");
    if (!in) return 0;
    memset(combined, 0, sizeof(*combined));

    char line[32768];
    uint32_t current_archive_index = UINT32_MAX;
    while (fgets(line, sizeof(line), in)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        if (line[0] == 'A' && len > 1 && line[1] == '\t') {
            EzdbArchiveRecord archive_record;
            if (!parse_combined_archive_fields(line + 2, &archive_record)) continue;
            current_archive_index = combined->archives.count;
            if (!push_loaded_archive(&combined->archives, &archive_record)) {
                fclose(in);
                free_loaded_combined(combined);
                return 0;
            }
        } else if (line[0] == 'E' && len > 1 && line[1] == '\t') {
            if (current_archive_index == UINT32_MAX) continue;
            LoadedEntry entry;
            if (!parse_combined_entry_fields(line + 2, current_archive_index, &entry)) continue;
            if (!push_loaded_entry(&combined->entries, &entry)) {
                fclose(in);
                free_loaded_combined(combined);
                return 0;
            }
        }
    }
    fclose(in);
    return 1;
}

typedef struct CombinedEntryStream {
    const LoadedEntries* entries;
    uint32_t pos;
} CombinedEntryStream;

static int combined_entry_reset(void* user_data)
{
    CombinedEntryStream* stream = (CombinedEntryStream*)user_data;
    if (!stream) return -1;
    stream->pos = 0;
    return 0;
}

static int combined_entry_next(void* user_data, EzdbEntryRecord* out_record)
{
    CombinedEntryStream* stream = (CombinedEntryStream*)user_data;
    if (!stream || !stream->entries || !out_record) return -1;
    if (stream->pos >= stream->entries->count) return -1;
    const LoadedEntry* e = &stream->entries->entries[stream->pos];
    memset(out_record, 0, sizeof(*out_record));
    out_record->archive_id = e->archive_index;
    out_record->entry_path = e->entry_path;
    out_record->entry_raw_path = e->entry_raw_path;
    out_record->entry_raw_path_len = e->entry_raw_path_len;
    out_record->compressed_size = e->compressed_size;
    out_record->original_size = e->original_size;
    out_record->modified_time = e->modified_time;
    ++stream->pos;
    return 0;
}

/* --- build-zip-entries command --- */

typedef struct ZipSpoolRecordHeader {
    uint32_t path_len;
    uint32_t raw_len;
    int64_t compressed_size;
    uint64_t original_size;
    uint64_t modified_time;
} ZipSpoolRecordHeader;

typedef struct ArchiveEntryChunk {
    uint32_t shard_id;
    uint64_t offset;
    uint64_t byte_size;
    uint32_t entry_count;
    int ok;
    char error[96];
    double parse_ms;
} ArchiveEntryChunk;

typedef struct ZipSpoolShard {
    char path[MAX_PATH * 2];
    FILE* fp;
    uint64_t written;
} ZipSpoolShard;

typedef struct ZipParseJob {
    const LoadedArchives* archives;
    ArchiveEntryChunk* chunks;
    ZipSpoolShard* shards;
    uint32_t archive_count;
    LONG next_index;
    volatile LONG completed;
    volatile LONG opened;
    volatile LONG failed;
    volatile LONG fallback;
    volatile LONG64 entries;
    volatile LONG64 spool_bytes;
} ZipParseJob;

typedef struct ZipWorkerContext {
    ZipParseJob* job;
    uint32_t shard_id;
} ZipWorkerContext;

typedef struct SpoolEntryStream {
    const ArchiveEntryChunk* chunks;
    const ZipSpoolShard* shards;
    uint32_t archive_count;
    uint32_t archive_index;
    uint32_t current_archive_id;
    uint32_t remaining_entries;
    FILE* fp;
    uint32_t current_shard_id;
    char* path_buf;
    uint32_t path_cap;
    void* raw_buf;
    uint32_t raw_cap;
} SpoolEntryStream;

static wchar_t* utf8_to_wide_alloc(const char* text)
{
    if (!text) return NULL;
    int need = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t* out = NULL;
    if (need <= 0) return NULL;
    out = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)need);
    if (!out) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, out, need) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char* wide_range_to_utf8_alloc(const wchar_t* text, int len)
{
    int need = WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL);
    char* out = NULL;
    if (need <= 0) return NULL;
    out = (char*)malloc((size_t)need + 1u);
    if (!out) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, text, len, out, need, NULL, NULL) <= 0) {
        free(out);
        return NULL;
    }
    out[need] = '\0';
    return out;
}

static int ends_with_archive_slash(const char* name, size_t len)
{
    if (!name || !len) return 0;
    return name[len - 1u] == '/' || name[len - 1u] == '\\';
}

static uint64_t zip_tm_to_filetime_value(const tm_unz* t)
{
    if (!t || t->tm_year <= 1900 || t->tm_mon < 0 || t->tm_mon > 11 ||
        t->tm_mday < 1 || t->tm_mday > 31) {
        return 0;
    }

    SYSTEMTIME local_st;
    SYSTEMTIME utc_st;
    FILETIME ft;
    ULARGE_INTEGER value;
    memset(&local_st, 0, sizeof(local_st));
    memset(&utc_st, 0, sizeof(utc_st));
    memset(&ft, 0, sizeof(ft));
    local_st.wYear = (WORD)t->tm_year;
    local_st.wMonth = (WORD)(t->tm_mon + 1);
    local_st.wDay = (WORD)t->tm_mday;
    local_st.wHour = (WORD)t->tm_hour;
    local_st.wMinute = (WORD)t->tm_min;
    local_st.wSecond = (WORD)t->tm_sec;
    if (!TzSpecificLocalTimeToSystemTime(NULL, &local_st, &utc_st)) return 0;
    if (!SystemTimeToFileTime(&utc_st, &ft)) return 0;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return value.QuadPart;
}

static char* decode_zip_name_best_effort(const char* raw, uint32_t raw_len, int is_utf8)
{
    wchar_t* wide = NULL;
    char* utf8 = NULL;
    int wide_len = 0;
    if (!raw || raw_len == 0) return NULL;

    if (is_utf8) {
        wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)raw_len, NULL, 0);
        if (wide_len > 0) {
            utf8 = (char*)malloc((size_t)raw_len + 1u);
            if (!utf8) return NULL;
            memcpy(utf8, raw, raw_len);
            utf8[raw_len] = '\0';
            return utf8;
        }
    }

    wide_len = MultiByteToWideChar(CP_ACP, 0, raw, (int)raw_len, NULL, 0);
    if (wide_len <= 0) {
        wide_len = MultiByteToWideChar(CP_UTF8, 0, raw, (int)raw_len, NULL, 0);
        if (wide_len <= 0) return NULL;
        wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wide_len);
        if (!wide) return NULL;
        if (MultiByteToWideChar(CP_UTF8, 0, raw, (int)raw_len, wide, wide_len) <= 0) {
            free(wide);
            return NULL;
        }
    } else {
        wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wide_len);
        if (!wide) return NULL;
        if (MultiByteToWideChar(CP_ACP, 0, raw, (int)raw_len, wide, wide_len) <= 0) {
            free(wide);
            return NULL;
        }
    }
    utf8 = wide_range_to_utf8_alloc(wide, wide_len);
    free(wide);
    return utf8;
}

static int raw_name_differs_from_utf8(const char* raw, uint32_t raw_len, const char* utf8)
{
    size_t utf8_len = utf8 ? strlen(utf8) : 0;
    return utf8_len != raw_len || (raw_len && memcmp(raw, utf8, raw_len) != 0);
}

static int write_zip_spool_entry(ZipSpoolShard* shard,
                                 const char* raw_name,
                                 uint32_t raw_name_len,
                                 int is_utf8,
                                 int64_t compressed_size,
                                 uint64_t original_size,
                                 uint64_t modified_time,
                                 uint32_t* out_entry_count)
{
    char* decoded = NULL;
    void* raw_copy = NULL;
    uint32_t raw_len = 0;
    ZipSpoolRecordHeader header;
    decoded = decode_zip_name_best_effort(raw_name, raw_name_len, is_utf8);
    if (!decoded || decoded[0] == '\0') {
        free(decoded);
        return 1;
    }
    if (raw_name_differs_from_utf8(raw_name, raw_name_len, decoded)) {
        raw_copy = malloc(raw_name_len ? raw_name_len : 1u);
        if (!raw_copy) {
            free(decoded);
            return 0;
        }
        memcpy(raw_copy, raw_name, raw_name_len);
        raw_len = raw_name_len;
    }
    memset(&header, 0, sizeof(header));
    header.path_len = (uint32_t)strlen(decoded);
    header.raw_len = raw_len;
    header.compressed_size = compressed_size;
    header.original_size = original_size;
    header.modified_time = modified_time;
    if (fwrite(&header, sizeof(header), 1, shard->fp) != 1 ||
        (header.path_len && fwrite(decoded, 1, header.path_len, shard->fp) != header.path_len) ||
        (raw_len && fwrite(raw_copy, 1, raw_len, shard->fp) != raw_len)) {
        free(decoded);
        free(raw_copy);
        return 0;
    }
    shard->written += sizeof(header) + header.path_len + raw_len;
    if (out_entry_count) *out_entry_count += 1u;
    free(decoded);
    free(raw_copy);
    return 1;
}

typedef struct ZipCdSpoolContext {
    ZipSpoolShard* shard;
    uint32_t entry_count;
} ZipCdSpoolContext;

static int on_zip_cd_entry(const ZipCdEntry* entry, void* user_data)
{
    ZipCdSpoolContext* ctx = (ZipCdSpoolContext*)user_data;
    if (!entry || !ctx || !ctx->shard) return 0;
    if (ends_with_archive_slash(entry->raw_name, entry->raw_name_len)) return 1;
    return write_zip_spool_entry(ctx->shard,
                                 entry->raw_name,
                                 entry->raw_name_len,
                                 (entry->flags & 0x800u) != 0,
                                 entry->compressed_size,
                                 entry->original_size,
                                 entry->modified_time,
                                 &ctx->entry_count);
}

static int parse_zip_archive_entries(const EzdbArchiveRecord* archive,
                                     uint32_t archive_index,
                                     ZipSpoolShard* shard,
                                     ArchiveEntryChunk* chunk)
{
    wchar_t* wide_path = NULL;
    zlib_filefunc64_def filefunc;
    unzFile uf = NULL;
    int rc = UNZ_OK;
    double start = now_ms();
    uint64_t start_offset = shard ? shard->written : 0;
    uint32_t entry_count = 0;

    memset(&filefunc, 0, sizeof(filefunc));
    if (!shard || !chunk) return 0;
    memset(chunk, 0, sizeof(*chunk));
    chunk->shard_id = UINT_MAX;
    wide_path = utf8_to_wide_alloc(archive->file_path);
    if (!wide_path) {
        strcpy(chunk->error, "path utf8 decode failed");
        return 0;
    }
    fill_win32_filefunc64W(&filefunc);
    uf = unzOpen2_64(wide_path, &filefunc);
    free(wide_path);
    if (!uf) {
        strcpy(chunk->error, "unzOpen2_64 failed");
        return 0;
    }

    rc = unzGoToFirstFile(uf);
    if (rc == UNZ_END_OF_LIST_OF_FILE) {
        chunk->ok = 1;
        chunk->offset = start_offset;
        chunk->byte_size = shard->written - start_offset;
        chunk->entry_count = 0;
        chunk->parse_ms = now_ms() - start;
        unzClose(uf);
        return 1;
    }
    if (rc != UNZ_OK) {
        strcpy(chunk->error, "unzGoToFirstFile failed");
        unzClose(uf);
        return 0;
    }

    while (rc == UNZ_OK) {
        unz_file_info64 info;
        char* name = NULL;
        memset(&info, 0, sizeof(info));
        rc = unzGetCurrentFileInfo64(uf, &info, NULL, 0, NULL, 0, NULL, 0);
        if (rc != UNZ_OK) {
            strcpy(chunk->error, "unzGetCurrentFileInfo64 failed");
            unzClose(uf);
            return 0;
        }
        if (info.size_filename > UINT_MAX - 1u) {
            strcpy(chunk->error, "zip entry name too long");
            unzClose(uf);
            return 0;
        }
        name = (char*)malloc((size_t)info.size_filename + 1u);
        if (!name) {
            strcpy(chunk->error, "out of memory");
            unzClose(uf);
            return 0;
        }
        rc = unzGetCurrentFileInfo64(uf, &info, name, info.size_filename + 1u, NULL, 0, NULL, 0);
        if (rc != UNZ_OK) {
            free(name);
            strcpy(chunk->error, "unzGetCurrentFileInfo64 name failed");
            unzClose(uf);
            return 0;
        }
        name[info.size_filename] = '\0';
        if (!ends_with_archive_slash(name, (size_t)info.size_filename)) {
            if (!write_zip_spool_entry(shard,
                                       name,
                                       (uint32_t)info.size_filename,
                                       (info.flag & 0x800) != 0,
                                       (int64_t)info.compressed_size,
                                       (uint64_t)info.uncompressed_size,
                                       zip_tm_to_filetime_value(&info.tmu_date),
                                       &entry_count)) {
                free(name);
                strcpy(chunk->error, "write spool failed");
                unzClose(uf);
                return 0;
            }
        }
        free(name);

        rc = unzGoToNextFile(uf);
        if (rc == UNZ_END_OF_LIST_OF_FILE) break;
        if (rc != UNZ_OK) {
            strcpy(chunk->error, "unzGoToNextFile failed");
            unzClose(uf);
            return 0;
        }
    }

    unzClose(uf);
    chunk->offset = start_offset;
    chunk->byte_size = shard->written - start_offset;
    chunk->entry_count = entry_count;
    chunk->ok = 1;
    chunk->parse_ms = now_ms() - start;
    (void)archive_index;
    return 1;
}

static int parse_zip_archive_entries_cd(const EzdbArchiveRecord* archive,
                                        ZipSpoolShard* shard,
                                        ArchiveEntryChunk* chunk)
{
    wchar_t* wide_path = NULL;
    double start = now_ms();
    uint64_t start_offset = shard ? shard->written : 0;
    ZipCdSpoolContext ctx;
    if (!archive || !shard || !chunk) return 0;
    memset(chunk, 0, sizeof(*chunk));
    chunk->shard_id = UINT_MAX;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shard = shard;
    wide_path = utf8_to_wide_alloc(archive->file_path);
    if (!wide_path) {
        strcpy(chunk->error, "path utf8 decode failed");
        return 0;
    }
    char error[96];
    memset(error, 0, sizeof(error));
    int ok = zip_cd_scan_entries(wide_path, on_zip_cd_entry, &ctx, error, (uint32_t)sizeof(error));
    free(wide_path);
    if (!ok) {
        snprintf(chunk->error, sizeof(chunk->error), "zip_cd failed: %s", error[0] ? error : "unknown");
        return 0;
    }
    chunk->offset = start_offset;
    chunk->byte_size = shard->written - start_offset;
    chunk->entry_count = ctx.entry_count;
    chunk->ok = 1;
    chunk->parse_ms = now_ms() - start;
    return 1;
}

static DWORD WINAPI zip_parse_worker(LPVOID param)
{
    ZipWorkerContext* ctx = (ZipWorkerContext*)param;
    ZipParseJob* job = ctx ? ctx->job : NULL;
    ZipSpoolShard* shard = job && ctx->shard_id < 64u ? &job->shards[ctx->shard_id] : NULL;
    if (!job || !shard) return 0;
    for (;;) {
        LONG index = InterlockedIncrement(&job->next_index) - 1;
        ArchiveEntryChunk* chunk = NULL;
        int ok = 0;
        if (index < 0 || (uint32_t)index >= job->archive_count) break;
        chunk = &job->chunks[index];
        ok = parse_zip_archive_entries_cd(&job->archives->records[index], shard, chunk);
        if (!ok) {
            InterlockedIncrement(&job->fallback);
            ok = parse_zip_archive_entries(&job->archives->records[index], (uint32_t)index, shard, chunk);
        }
        chunk->shard_id = ctx->shard_id;
        if (ok) {
            InterlockedIncrement(&job->opened);
            InterlockedAdd64(&job->entries, (LONG64)chunk->entry_count);
            InterlockedAdd64(&job->spool_bytes, (LONG64)chunk->byte_size);
        } else {
            InterlockedIncrement(&job->failed);
        }
        InterlockedIncrement(&job->completed);
    }
    return 0;
}

static void spool_entry_stream_close_file(SpoolEntryStream* stream)
{
    if (stream && stream->fp) {
        fclose(stream->fp);
        stream->fp = NULL;
    }
}

static int spool_entry_reset(void* user_data)
{
    SpoolEntryStream* stream = (SpoolEntryStream*)user_data;
    if (!stream) return -1;
    spool_entry_stream_close_file(stream);
    stream->archive_index = 0;
    stream->current_archive_id = UINT_MAX;
    stream->remaining_entries = 0;
    stream->current_shard_id = UINT_MAX;
    return 0;
}

static int spool_entry_next(void* user_data, EzdbEntryRecord* out_record)
{
    SpoolEntryStream* stream = (SpoolEntryStream*)user_data;
    if (!stream || !out_record) return -1;
    for (;;) {
        if (stream->remaining_entries > 0 && stream->fp) {
            ZipSpoolRecordHeader header;
            if (fread(&header, sizeof(header), 1, stream->fp) != 1) return -1;
            if (header.path_len + 1u > stream->path_cap) {
                uint32_t next = header.path_len + 1u;
                char* p = (char*)realloc(stream->path_buf, next);
                if (!p) return -1;
                stream->path_buf = p;
                stream->path_cap = next;
            }
            if (header.raw_len > stream->raw_cap) {
                uint32_t next = header.raw_len;
                void* p = realloc(stream->raw_buf, next ? next : 1u);
                if (!p) return -1;
                stream->raw_buf = p;
                stream->raw_cap = next;
            }
            if ((header.path_len && fread(stream->path_buf, 1, header.path_len, stream->fp) != header.path_len) ||
                (header.raw_len && fread(stream->raw_buf, 1, header.raw_len, stream->fp) != header.raw_len)) {
                return -1;
            }
            stream->path_buf[header.path_len] = '\0';
            memset(out_record, 0, sizeof(*out_record));
            out_record->archive_id = stream->current_archive_id;
            out_record->entry_path = stream->path_buf;
            out_record->entry_raw_path = header.raw_len ? stream->raw_buf : NULL;
            out_record->entry_raw_path_len = header.raw_len;
            out_record->compressed_size = header.compressed_size;
            out_record->original_size = header.original_size;
            out_record->modified_time = header.modified_time;
            --stream->remaining_entries;
            return 0;
        }
        spool_entry_stream_close_file(stream);
        while (stream->archive_index < stream->archive_count) {
            const ArchiveEntryChunk* chunk = &stream->chunks[stream->archive_index];
            if (!chunk->ok || chunk->entry_count == 0) {
                ++stream->archive_index;
                continue;
            }
            stream->fp = fopen(stream->shards[chunk->shard_id].path, "rb");
            if (!stream->fp) return -1;
            if (_fseeki64(stream->fp, (__int64)chunk->offset, SEEK_SET) != 0) return -1;
            stream->current_archive_id = stream->archive_index;
            ++stream->archive_index;
            stream->remaining_entries = chunk->entry_count;
            stream->current_shard_id = chunk->shard_id;
            break;
        }
        if (!stream->fp) return -1;
    }
}

static void free_spool_entry_stream(SpoolEntryStream* stream)
{
    if (!stream) return;
    spool_entry_stream_close_file(stream);
    free(stream->path_buf);
    free(stream->raw_buf);
    memset(stream, 0, sizeof(*stream));
}

static void cleanup_temp_dir_files(const char* temp_dir)
{
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA data;
    HANDLE find;
    if (!temp_dir || !temp_dir[0]) return;
    snprintf(pattern, sizeof(pattern), "%s\\*", temp_dir);
    find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            char path[MAX_PATH * 2];
            if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
            snprintf(path, sizeof(path), "%s\\%s", temp_dir, data.cFileName);
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileA(path);
        } while (FindNextFileA(find, &data));
        FindClose(find);
    }
    RemoveDirectoryA(temp_dir);
}

static int prepare_temp_dir(const char* temp_dir)
{
    cleanup_temp_dir_files(temp_dir);
    if (CreateDirectoryA(temp_dir, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    return 0;
}

static int run_build_zip_entries(const char* zip_tsv, const char* output_ezdb, uint32_t thread_count)
{
    LoadedArchives archives;
    ArchiveEntryChunk* chunks = NULL;
    ZipSpoolShard* shards = NULL;
    ZipWorkerContext* contexts = NULL;
    ZipParseJob job;
    HANDLE* threads = NULL;
    char temp_dir[MAX_PATH * 2];
    double load_start = now_ms();
    double parse_start = 0.0;
    double build_start = 0.0;
    double parse_to_build_start = 0.0;
    uint32_t entry_count = 0;
    uint32_t created_threads = 0;
    int exit_code = 0;

    if (thread_count == 0) thread_count = 6;
    if (thread_count > 64) thread_count = 64;

    memset(&archives, 0, sizeof(archives));
    if (!load_archive_tsv(zip_tsv, &archives)) {
        fprintf(stderr, "build-zip-entries: unable to read %s\n", zip_tsv);
        free_loaded_archives(&archives);
        return 2;
    }

    printf("zip_input: %s\n", zip_tsv);
    printf("zip_archives_loaded: %u\n", archives.count);
    printf("zip_threads: %u\n", thread_count);
    printf("zip_load_tsv_seconds: %.3f\n", ms_to_seconds(now_ms() - load_start));
    print_memory_usage("zip_loaded");

    snprintf(temp_dir, sizeof(temp_dir), "%s.tmp", output_ezdb);
    if (!prepare_temp_dir(temp_dir)) {
        fprintf(stderr, "build-zip-entries: unable to prepare temp dir %s\n", temp_dir);
        free_loaded_archives(&archives);
        return 2;
    }

    chunks = (ArchiveEntryChunk*)calloc(archives.count ? archives.count : 1u, sizeof(ArchiveEntryChunk));
    shards = (ZipSpoolShard*)calloc(thread_count, sizeof(ZipSpoolShard));
    contexts = (ZipWorkerContext*)calloc(thread_count, sizeof(ZipWorkerContext));
    threads = (HANDLE*)calloc(thread_count, sizeof(HANDLE));
    if (!chunks || !shards || !contexts || !threads) {
        free(threads);
        free(contexts);
        free(shards);
        free(chunks);
        cleanup_temp_dir_files(temp_dir);
        free_loaded_archives(&archives);
        return 2;
    }
    for (uint32_t i = 0; i < thread_count; ++i) {
        snprintf(shards[i].path, sizeof(shards[i].path), "%s\\zip_entries_%u.spool", temp_dir, i);
        shards[i].fp = fopen(shards[i].path, "wb");
        if (!shards[i].fp) {
            fprintf(stderr, "build-zip-entries: unable to create spool shard %s\n", shards[i].path);
            exit_code = 2;
            break;
        }
    }

    memset(&job, 0, sizeof(job));
    job.archives = &archives;
    job.chunks = chunks;
    job.shards = shards;
    job.archive_count = archives.count;
    parse_start = now_ms();
    parse_to_build_start = parse_start;
    for (uint32_t i = 0; exit_code == 0 && i < thread_count; ++i) {
        contexts[i].job = &job;
        contexts[i].shard_id = i;
        threads[i] = CreateThread(NULL, 0, zip_parse_worker, &contexts[i], 0, NULL);
        if (!threads[i]) {
            fprintf(stderr, "build-zip-entries: CreateThread failed (%lu)\n", GetLastError());
            exit_code = 2;
            break;
        }
        ++created_threads;
    }
    if (exit_code != 0) {
        InterlockedExchange(&job.next_index, (LONG)archives.count);
        if (created_threads) WaitForMultipleObjects(created_threads, threads, TRUE, INFINITE);
    }

    while (exit_code == 0) {
        DWORD wait_rc = WaitForMultipleObjects(created_threads, threads, TRUE, 1000);
        LONG done = job.completed;
        LONG opened = job.opened;
        LONG failed = job.failed;
        LONG fallback = job.fallback;
        LONG64 entries = job.entries;
        LONG64 spool_bytes = job.spool_bytes;
        double elapsed = now_ms() - parse_start;
        printf("zip_parse_progress: %ld/%u opened=%ld failed=%ld fallback=%ld entries=%lld spool_entry_bytes_mb=%.2f elapsed_seconds=%.3f archives_per_second=%.2f entries_per_second=%.2f\n",
               done,
               archives.count,
               opened,
               failed,
               fallback,
               (long long)entries,
               bytes_to_mb((uint64_t)spool_bytes),
               ms_to_seconds(elapsed),
               elapsed > 0.0 ? (double)done * 1000.0 / elapsed : 0.0,
               elapsed > 0.0 ? (double)entries * 1000.0 / elapsed : 0.0);
        fflush(stdout);
        if (wait_rc == WAIT_OBJECT_0) break;
        if (wait_rc == WAIT_FAILED) {
            fprintf(stderr, "build-zip-entries: wait failed (%lu)\n", GetLastError());
            exit_code = 2;
            break;
        }
    }
    for (uint32_t i = 0; i < created_threads; ++i) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    free(threads);
    threads = NULL;
    for (uint32_t i = 0; i < thread_count; ++i) {
        if (shards[i].fp) {
            fclose(shards[i].fp);
            shards[i].fp = NULL;
        }
    }
    if (exit_code != 0) {
        free(contexts);
        free(shards);
        free(chunks);
        cleanup_temp_dir_files(temp_dir);
        free_loaded_archives(&archives);
        return exit_code;
    }

    if (job.entries > UINT32_MAX) {
        fprintf(stderr, "build-zip-entries: too many entries for ezdb snapshot: %lld\n", (long long)job.entries);
        free(contexts);
        free(shards);
        free(chunks);
        cleanup_temp_dir_files(temp_dir);
        free_loaded_archives(&archives);
        return 2;
    }
    entry_count = (uint32_t)job.entries;

    printf("zip_parse_seconds: %.3f\n", ms_to_seconds(now_ms() - parse_start));
    printf("zip_cd_scan_seconds: %.3f\n", ms_to_seconds(now_ms() - parse_start));
    printf("zip_minizip_fallback_count: %ld\n", job.fallback);
    printf("zip_opened_archives: %ld\n", job.opened);
    printf("zip_failed_archives: %ld\n", job.failed);
    printf("zip_entries: %u\n", entry_count);
    printf("spool_parse_write_seconds: %.3f\n", ms_to_seconds(now_ms() - parse_start));
    printf("spool_entry_bytes_mb: %.2f\n", bytes_to_mb((uint64_t)job.spool_bytes));
    for (uint32_t i = 0, printed = 0; i < archives.count && printed < 10; ++i) {
        if (!chunks[i].ok && chunks[i].error[0]) {
            printf("zip_parse_error[%u]: %s :: %s\n", i, chunks[i].error, archives.records[i].file_path);
            ++printed;
        }
    }
    print_memory_usage("zip_parsed");

    DeleteFileA(output_ezdb);
    SpoolEntryStream spool_stream;
    EzdbEntryStream ez_stream;
    memset(&spool_stream, 0, sizeof(spool_stream));
    spool_stream.chunks = chunks;
    spool_stream.shards = shards;
    spool_stream.archive_count = archives.count;
    spool_stream.current_shard_id = UINT_MAX;
    memset(&ez_stream, 0, sizeof(ez_stream));
    ez_stream.user_data = &spool_stream;
    ez_stream.reset = spool_entry_reset;
    ez_stream.next = spool_entry_next;

    build_start = now_ms();
    {
        EzdbBuildOptions build_options;
        memset(&build_options, 0, sizeof(build_options));
        build_options.temp_dir = temp_dir;
        build_options.memory_limit_mb = 512u;
        build_options.flags = EZDB_BUILD_DEFAULT_FLAGS;
        build_options.zip_threads = thread_count;
        build_options.index_threads = thread_count;
        int rc = ezdb_build_snapshot_stream_entries_ex(archives.records,
                                                       archives.count,
                                                       entry_count ? &ez_stream : NULL,
                                                       entry_count,
                                                       output_ezdb,
                                                       &build_options);
        double build_elapsed = now_ms() - build_start;
        if (rc != 0) {
            fprintf(stderr, "build-zip-entries: ezdb_build_snapshot_stream_entries failed: %s (%d)\n", ezdb_error_message(rc), rc);
            exit_code = 2;
        }
        printf("zip_build_seconds: %.3f\n", ms_to_seconds(build_elapsed));
        printf("zip_total_parse_to_build_seconds: %.3f\n", ms_to_seconds(now_ms() - parse_to_build_start));
        printf("zip_output_size_mb: %.2f\n", bytes_to_mb(file_size_of_path(output_ezdb)));
        print_memory_usage("zip_built");
    }

    free_spool_entry_stream(&spool_stream);
    free(contexts);
    free(shards);
    free(chunks);
    cleanup_temp_dir_files(temp_dir);
    free_loaded_archives(&archives);
    return exit_code;
}

/* --- build-entries command --- */

static int run_build_entries(const char* combined_tsv, const char* output_ezdb)
{
    DeleteFileA(output_ezdb);

    LoadedCombined combined;
    memset(&combined, 0, sizeof(combined));
    if (!load_combined_tsv(combined_tsv, &combined)) {
        fprintf(stderr, "build-entries: unable to read %s\n", combined_tsv);
        free_loaded_combined(&combined);
        return 2;
    }
    printf("build_entries_archives: %u\n", combined.archives.count);
    printf("build_entries_entries: %u\n", combined.entries.count);
    print_memory_usage("build_entries_loaded");

    CombinedEntryStream cstream;
    memset(&cstream, 0, sizeof(cstream));
    cstream.entries = &combined.entries;
    cstream.pos = 0;

    EzdbEntryStream ez_stream;
    memset(&ez_stream, 0, sizeof(ez_stream));
    ez_stream.user_data = &cstream;
    ez_stream.reset = combined_entry_reset;
    ez_stream.next = combined_entry_next;

    double start = now_ms();
    int rc = ezdb_build_snapshot_stream_entries(combined.archives.records, combined.archives.count, &ez_stream, combined.entries.count, output_ezdb);
    double elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "build-entries: ezdb_build_snapshot_stream_entries failed: %s (%d)\n", ezdb_error_message(rc), rc);
        free_loaded_combined(&combined);
        return 2;
    }
    printf("build_entries_ms: %.2f\n", elapsed);
    printf("build_entries_output_size: %llu\n", (unsigned long long)file_size_of_path(output_ezdb));
    print_memory_usage("build_entries_after");

    free_loaded_combined(&combined);
    return 0;
}

/* --- live-entry-append-batch command --- */

static int run_live_entry_append_batch(const char* combined_tsv, const char* output_ezdb, uint32_t batch_size)
{
    DeleteFileA(output_ezdb);

    LoadedCombined combined;
    memset(&combined, 0, sizeof(combined));
    if (!load_combined_tsv(combined_tsv, &combined)) {
        fprintf(stderr, "live-entry-append-batch: unable to read %s\n", combined_tsv);
        free_loaded_combined(&combined);
        return 2;
    }
    printf("live_batch_archives: %u\n", combined.archives.count);
    printf("live_batch_entries: %u\n", combined.entries.count);
    printf("live_batch_batch_size: %u\n", batch_size);
    print_memory_usage("live_batch_loaded");

    /* Build archive-only base */
    double start = now_ms();
    int rc = ezdb_build_snapshot(combined.archives.records, combined.archives.count, NULL, 0, output_ezdb);
    double build_elapsed = now_ms() - start;
    if (rc != 0) {
        fprintf(stderr, "live-entry-append-batch: build snapshot failed: %s (%d)\n", ezdb_error_message(rc), rc);
        free_loaded_combined(&combined);
        return 2;
    }
    printf("live_batch_build_ms: %.2f\n", build_elapsed);

    Ezdb* db = NULL;
    rc = ezdb_open(output_ezdb, &db);
    if (rc != 0) {
        fprintf(stderr, "live-entry-append-batch: open failed: %s (%d)\n", ezdb_error_message(rc), rc);
        free_loaded_combined(&combined);
        return 2;
    }
    printf("live_batch_open_ms: %.2f\n", now_ms() - start - build_elapsed);

    /* Group entries by archive_index: scan to find run boundaries */
    /* entries are already sorted by archive_index */
    double append_start = now_ms();
    rc = ezdb_begin_write(db, 0);
    if (rc != 0) {
        fprintf(stderr, "live-entry-append-batch: begin_write failed: %s (%d)\n", ezdb_error_message(rc), rc);
        ezdb_close(db);
        free_loaded_combined(&combined);
        return 2;
    }

    uint32_t e_idx = 0;
    uint32_t archives_with_entries = 0;
    EzdbEntryRecord* batch = (EzdbEntryRecord*)calloc(batch_size ? batch_size : 1024u, sizeof(EzdbEntryRecord));
    char** path_copies = (char**)calloc(batch_size ? batch_size : 1024u, sizeof(char*));
    if (!batch || !path_copies) {
        free(batch);
        free(path_copies);
        ezdb_rollback_write(db);
        ezdb_close(db);
        free_loaded_combined(&combined);
        return 2;
    }

    while (e_idx < combined.entries.count) {
        uint32_t arch_id = combined.entries.entries[e_idx].archive_index;
        if (arch_id >= combined.archives.count) {
            fprintf(stderr, "live-entry-append-batch: archive_index %u out of range (max %u)\n", arch_id, combined.archives.count);
            break;
        }

        /* Count entries for this archive */
        uint32_t run_start = e_idx;
        while (e_idx < combined.entries.count && combined.entries.entries[e_idx].archive_index == arch_id) {
            ++e_idx;
        }
        uint32_t run_count = e_idx - run_start;

        rc = ezdb_begin_replace_archive_entries(db, arch_id);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append-batch: begin_replace archive %u failed: %s (%d)\n", arch_id, ezdb_error_message(rc), rc);
            break;
        }

        /* Append in batches */
        uint32_t written = 0;
        while (written < run_count) {
            uint32_t n = run_count - written;
            if (n > batch_size) n = batch_size;
            for (uint32_t i = 0; i < n; ++i) {
                const LoadedEntry* le = &combined.entries.entries[run_start + written + i];
                path_copies[i] = _strdup(le->entry_path);
                memset(&batch[i], 0, sizeof(batch[i]));
                batch[i].archive_id = arch_id;
                batch[i].entry_path = path_copies[i];
                batch[i].compressed_size = le->compressed_size;
                batch[i].original_size = le->original_size;
                batch[i].modified_time = le->modified_time;
            }
            rc = ezdb_append_archive_entries(db, arch_id, batch, n);
            for (uint32_t i = 0; i < n; ++i) {
                free(path_copies[i]);
                path_copies[i] = NULL;
            }
            if (rc != 0) break;
            written += n;
        }

        if (rc == 0) rc = ezdb_finish_replace_archive_entries(db, arch_id);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append-batch: error on archive %u: %s (%d)\n", arch_id, ezdb_error_message(rc), rc);
            break;
        }
        ++archives_with_entries;
    }

    if (rc == 0) {
        rc = ezdb_commit_write(db);
    } else {
        ezdb_rollback_write(db);
    }
    double append_elapsed = now_ms() - append_start;

    free(batch);
    free(path_copies);

    uint64_t end_size = ezdb_file_size(db);
    printf("live_batch_append_ms: %.2f\n", append_elapsed);
    printf("live_batch_archives_with_entries: %u\n", archives_with_entries);
    printf("live_batch_end_size: %llu\n", (unsigned long long)end_size);
    print_memory_usage("live_batch_after");

    ezdb_close(db);
    free_loaded_combined(&combined);
    return rc == 0 ? 0 : 2;
}

static int run_main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "build-archives") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        LoadedArchives loaded;
        memset(&loaded, 0, sizeof(loaded));
        if (!load_archive_tsv(argv[2], &loaded)) {
            fprintf(stderr, "build-archives failed: unable to read %s\n", argv[2]);
            free_loaded_archives(&loaded);
            return 2;
        }
        double start = now_ms();
        int rc = build_from_loaded_archives(&loaded, argv[3], "build-archives");
        double elapsed = now_ms() - start;
        free_loaded_archives(&loaded);
        if (rc != 0) {
            return rc;
        }
        printf("build ok: %.2f ms\n", elapsed);
        print_memory_usage("build");
        return 0;
    }

    if (strcmp(argv[1], "build-entries") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        return run_build_entries(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "build-zip-entries") == 0) {
        if (argc < 4 || argc > 5) {
            print_usage();
            return 1;
        }
        return run_build_zip_entries(argv[2], argv[3], argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 6u);
    }

    if (strcmp(argv[1], "live-entry-append-batch") == 0) {
        if (argc < 4 || argc > 5) {
            print_usage();
            return 1;
        }
        uint32_t lbatch_size = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 4096u;
        if (!lbatch_size) lbatch_size = 4096u;
        return run_live_entry_append_batch(argv[2], argv[3], lbatch_size);
    }

    if (strcmp(argv[1], "live-entry-append") == 0) {
        if (argc < 4 || argc > 5) {
            print_usage();
            return 1;
        }
        const char* path = argv[2];
        uint32_t entry_count = (uint32_t)strtoul(argv[3], NULL, 10);
        uint32_t batch_size = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 4096u;
        if (!batch_size) batch_size = 4096u;
        remove(path);

        EzdbArchiveRecord archive;
        memset(&archive, 0, sizeof(archive));
        archive.drive_letter = 'T';
        archive.file_ref_number = 12345;
        archive.usn = 67890;
        archive.file_path = "T:\\EveryZipBench\\live.zip";
        archive.file_size = 100;
        archive.modified_time = 200;
        int rc = ezdb_build_snapshot(&archive, 1, NULL, 0, path);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append build failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        Ezdb* db = NULL;
        rc = ezdb_open(path, &db);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        uint64_t start_size = ezdb_file_size(db);
        rc = ezdb_begin_write(db, 0);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append write transaction begin failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        rc = ezdb_begin_replace_archive_entries(db, 0);
        if (rc != 0) {
            fprintf(stderr, "live-entry-append begin failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_rollback_write(db);
            ezdb_close(db);
            return 2;
        }
        EzdbEntryRecord* batch = (EzdbEntryRecord*)calloc(batch_size, sizeof(EzdbEntryRecord));
        char** paths = (char**)calloc(batch_size, sizeof(char*));
        if (!batch || !paths) {
            free(batch);
            free(paths);
            ezdb_close(db);
            return 2;
        }
        double start = now_ms();
        uint32_t written = 0;
        while (written < entry_count) {
            uint32_t n = entry_count - written;
            if (n > batch_size) n = batch_size;
            for (uint32_t i = 0; i < n; ++i) {
                char buf[128];
                snprintf(buf, sizeof(buf), "folder/live_file_%08u.txt", written + i);
                paths[i] = _strdup(buf);
                batch[i].archive_id = 0;
                batch[i].entry_path = paths[i];
                batch[i].compressed_size = (int64_t)(written + i);
                batch[i].original_size = (uint64_t)(written + i) * 2u;
                batch[i].modified_time = 300 + written + i;
            }
            rc = ezdb_append_archive_entries(db, 0, batch, n);
            for (uint32_t i = 0; i < n; ++i) {
                free(paths[i]);
                paths[i] = NULL;
                memset(&batch[i], 0, sizeof(batch[i]));
            }
            if (rc != 0) break;
            written += n;
            printf("batch_written: %u file_size: %llu\n", written, (unsigned long long)ezdb_file_size(db));
        }
        if (rc == 0) rc = ezdb_finish_replace_archive_entries(db, 0);
        if (rc == 0) {
            rc = ezdb_commit_write(db);
        } else {
            ezdb_rollback_write(db);
        }
        double elapsed = now_ms() - start;
        uint64_t end_size = ezdb_file_size(db);
        free(batch);
        free(paths);
        ezdb_close(db);
        printf("live_append_ms: %.2f\n", elapsed);
        printf("start_size: %llu\n", (unsigned long long)start_size);
        printf("end_size: %llu\n", (unsigned long long)end_size);
        print_memory_usage("live_append");
        return rc == 0 ? 0 : 2;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbStats stats;
        rc = ezdb_stats(db, &stats);
        if (rc != 0) {
            fprintf(stderr, "stats failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("records: %u\n", stats.record_count);
        printf("active: %u\n", stats.active_count);
        printf("entries: %u\n", stats.entry_count);
        printf("active_entries: %u\n", stats.active_entry_count);
        printf("base_entries: %u\n", stats.base_entry_count);
        printf("delta_entries: %u\n", stats.delta_entry_count);
        printf("file_size: %llu bytes\n", (unsigned long long)stats.file_size);
        printf("delta_size: %llu bytes\n", (unsigned long long)stats.delta_size);
        printf("records_size: %llu bytes\n", (unsigned long long)stats.records_size);
        printf("dirs_size: %llu bytes\n", (unsigned long long)stats.dirs_size);
        printf("names_size: %llu bytes\n", (unsigned long long)stats.names_size);
        printf("archive_meta_size: %llu bytes\n", (unsigned long long)stats.archive_meta_size);
        printf("entry_records_size: %llu bytes\n", (unsigned long long)stats.entry_records_size);
        printf("raw_blob_size: %llu bytes\n", (unsigned long long)stats.raw_blob_size);
        printf("index_size: %llu bytes\n", (unsigned long long)stats.index_size);
        printf("postings_size: %llu bytes\n", (unsigned long long)stats.postings_size);
        print_memory_usage("info");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbSearchResult result;
        rc = ezdb_get_by_id(db, (uint32_t)strtoul(argv[3], NULL, 10), &result);
        if (rc != 0) {
            fprintf(stderr, "get failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("[%u] %s, %llu, %llu\n",
               result.id,
               result.path,
               (unsigned long long)result.size,
               (unsigned long long)result.modified_time);
        print_memory_usage("get");
        ezdb_free_result(&result);
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "get-archive") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbArchiveResult result;
        rc = ezdb_get_archive(db, (uint32_t)strtoul(argv[3], NULL, 10), &result);
        if (rc != 0) {
            fprintf(stderr, "get-archive failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("[%u] %c %llu %lld %s, %llu, %llu\n",
               result.id,
               result.drive_letter ? result.drive_letter : '-',
               (unsigned long long)result.file_ref_number,
               (long long)result.usn,
               result.file_path,
               (unsigned long long)result.file_size,
               (unsigned long long)result.modified_time);
        ezdb_free_archive_result(&result);
        print_memory_usage("get_archive");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "get-entry") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbEntryResult result;
        rc = ezdb_get_entry(db, (uint32_t)strtoul(argv[3], NULL, 10), &result);
        if (rc != 0) {
            fprintf(stderr, "get-entry failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("[%u archive:%u] %s :: %s, %lld, %llu, %llu raw=%u\n",
               result.id,
               result.archive_id,
               result.archive_path,
               result.entry_path,
               (long long)result.compressed_size,
               (unsigned long long)result.original_size,
               (unsigned long long)result.modified_time,
               result.entry_raw_path_len);
        ezdb_free_entry_result(&result);
        print_memory_usage("get_entry");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 4 || argc > 5) {
            print_usage();
            return 1;
        }
        uint32_t limit = argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 100;
        Ezdb* db = NULL;
        double open_start = now_ms();
        int rc = ezdb_open(argv[2], &db);
        double open_elapsed = now_ms() - open_start;
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }

        printf("open_ms: %.2f\n", open_elapsed);
        rc = run_search_once(db, argv[3], limit, "search");
        if (rc != 0) {
            ezdb_close(db);
            return 2;
        }
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "search-v2") == 0) {
        if (argc < 5 || argc > 6) {
            print_usage();
            return 1;
        }
        uint32_t scope = parse_scope_arg(argv[3]);
        uint32_t limit = argc == 6 ? (uint32_t)strtoul(argv[5], NULL, 10) : 100;
        Ezdb* db = NULL;
        double open_start = now_ms();
        int rc = ezdb_open(argv[2], &db);
        double open_elapsed = now_ms() - open_start;
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }

        printf("open_ms: %.2f\n", open_elapsed);
        rc = run_search_v2_once(db, argv[4], scope, limit, "search");
        if (rc != 0) {
            ezdb_close(db);
            return 2;
        }
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "open") == 0 || strcmp(argv[1], "interactive") == 0) {
        if (argc < 3 || argc > 4) {
            print_usage();
            return 1;
        }
        uint32_t default_limit = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 20u;
        Ezdb* db = NULL;
        double open_start = now_ms();
        int rc = ezdb_open(argv[2], &db);
        double open_elapsed = now_ms() - open_start;
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        printf("open_ms: %.2f\n", open_elapsed);
        print_memory_usage("open");
        print_interactive_help(default_limit);

        char line[4096];
        for (;;) {
            printf("ezdb> ");
            fflush(stdout);
            if (!read_console_utf8_line(line, sizeof(line))) break;
            char* text = trim_ascii(line);
            if (!*text || strcmp(text, "help") == 0 || strcmp(text, "?") == 0) {
                print_interactive_help(default_limit);
                continue;
            }
            if (strcmp(text, "exit") == 0 || strcmp(text, "quit") == 0) break;

            char* cursor = text;
            char* command = next_token(&cursor);
            if (!command) {
                print_interactive_help(default_limit);
                continue;
            }

            if (strcmp(command, "info") == 0) {
                rc = print_db_info(db, "open_info");
            } else if (strcmp(command, "get") == 0) {
                char* id_text = next_token(&cursor);
                if (!id_text) {
                    printf("usage: get <id>\n");
                    continue;
                }
                rc = run_get_once(db, (uint32_t)strtoul(id_text, NULL, 10), "open_get");
            } else if (strcmp(command, "search") == 0) {
                char* keyword = trim_ascii(cursor);
                uint32_t limit = default_limit;
                int parsed = parse_interactive_query(keyword, &keyword, &limit, default_limit);
                if (parsed <= 0) {
                    printf("usage: search <keyword> [limit]\n");
                    continue;
                }
                rc = run_search_once(db, keyword, limit, "open_search");
            } else if (strcmp(command, "insert") == 0) {
                char* path = next_token(&cursor);
                char* size_text = next_token(&cursor);
                char* mtime_text = next_token(&cursor);
                if (!path) {
                    printf("usage: insert <path> [size] [mtime]\n");
                    continue;
                }
                rc = run_insert_once(db,
                                     path,
                                     parse_u64_arg(size_text, 0),
                                     parse_u64_arg(mtime_text, 0),
                                     "open_insert");
            } else if (strcmp(command, "update") == 0) {
                char* id_text = next_token(&cursor);
                char* path = next_token(&cursor);
                char* size_text = next_token(&cursor);
                char* mtime_text = next_token(&cursor);
                if (!id_text || !path) {
                    printf("usage: update <id> <path> [size] [mtime]\n");
                    continue;
                }
                rc = run_update_once(db,
                                     (uint32_t)strtoul(id_text, NULL, 10),
                                     path,
                                     parse_u64_arg(size_text, 0),
                                     parse_u64_arg(mtime_text, 0),
                                     "open_update");
            } else if (strcmp(command, "delete") == 0) {
                char* id_text = next_token(&cursor);
                if (!id_text) {
                    printf("usage: delete <id>\n");
                    continue;
                }
                rc = run_delete_once(db, (uint32_t)strtoul(id_text, NULL, 10), "open_delete");
            } else {
                char* keyword = NULL;
                uint32_t limit = default_limit;
                int parsed = parse_interactive_query(text, &keyword, &limit, default_limit);
                if (parsed < 0) break;
                if (parsed == 0) {
                    print_interactive_help(default_limit);
                    continue;
                }
                rc = run_search_once(db, keyword, limit, "open_search");
            }
            if (rc != 0) printf("command failed, type help for usage.\n");
        }
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "insert") == 0) {
        if (argc < 4 || argc > 6) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbFileRecord record;
        record.path = argv[3];
        record.size = argc >= 5 ? parse_u64_arg(argv[4], 0) : 0;
        record.modified_time = argc >= 6 ? parse_u64_arg(argv[5], 0) : 0;
        uint32_t id = 0;
        double start = now_ms();
        rc = ezdb_insert(db, &record, &id);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "insert failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("insert_id: %u\n", id);
        printf("insert_ms: %.2f\n", elapsed);
        print_memory_usage("insert");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "update") == 0) {
        if (argc < 5 || argc > 7) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        EzdbFileRecord record;
        record.path = argv[4];
        record.size = argc >= 6 ? parse_u64_arg(argv[5], 0) : 0;
        record.modified_time = argc >= 7 ? parse_u64_arg(argv[6], 0) : 0;
        double start = now_ms();
        rc = ezdb_update(db, (uint32_t)strtoul(argv[3], NULL, 10), &record);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "update failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("update_ms: %.2f\n", elapsed);
        print_memory_usage("update");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "delete") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        double start = now_ms();
        rc = ezdb_delete(db, (uint32_t)strtoul(argv[3], NULL, 10));
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "delete failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("delete_ms: %.2f\n", elapsed);
        print_memory_usage("delete");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "delete-archive-ref") == 0) {
        if (argc != 5) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        double start = now_ms();
        rc = ezdb_delete_archive_by_ref(db, argv[3][0], parse_u64_arg(argv[4], 0));
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "delete-archive-ref failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("delete_archive_ref_ms: %.2f\n", elapsed);
        print_memory_usage("delete_archive_ref");
        ezdb_close(db);
        return 0;
    }

    if (strcmp(argv[1], "compact") == 0) {
        if (argc != 3) {
            print_usage();
            return 1;
        }
        Ezdb* db = NULL;
        int rc = ezdb_open(argv[2], &db);
        if (rc != 0) {
            fprintf(stderr, "open failed: %s (%d)\n", ezdb_error_message(rc), rc);
            return 2;
        }
        uint64_t before = ezdb_file_size(db);
        double start = now_ms();
        rc = ezdb_compact(db);
        double elapsed = now_ms() - start;
        if (rc != 0) {
            fprintf(stderr, "compact failed: %s (%d)\n", ezdb_error_message(rc), rc);
            ezdb_close(db);
            return 2;
        }
        printf("compact_ms: %.2f\n", elapsed);
        printf("before_size: %llu\n", (unsigned long long)before);
        printf("after_size: %llu\n", (unsigned long long)file_size_of_path(argv[2]));
        print_memory_usage("compact");
        ezdb_close(db);
        return 0;
    }

    print_usage();
    return 1;
}

int main(void)
{
    int argc = 0;
    char** argv = NULL;
    if (!make_utf8_argv(&argc, &argv)) {
        fprintf(stderr, "failed to parse UTF-8 command line\n");
        return 2;
    }
    int rc = run_main(argc, argv);
    free_utf8_argv(argc, argv);
    return rc;
}
