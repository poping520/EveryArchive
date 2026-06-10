#include "edb_util.h"
#include <stdlib.h>
#include <string.h>

/* ===== 线程抽象 ===== */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int edb_thread_create(EdbThread* t, edb_thread_fn fn, void* arg) {
    t->handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
    return t->handle ? 0 : -1;
}

int edb_thread_join(EdbThread* t) {
    if (!t->handle) return -1;
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
    return 0;
}
#else
#include <pthread.h>

int edb_thread_create(EdbThread* t, edb_thread_fn fn, void* arg) {
    pthread_t pt;
    int rc = pthread_create(&pt, NULL, fn, arg);
    if (rc != 0) return -1;
    t->id = (unsigned long)pt;
    return 0;
}

int edb_thread_join(EdbThread* t) {
    pthread_t pt = (pthread_t)t->id;
    return pthread_join(pt, NULL);
}
#endif

/* ===== 哈希 ===== */

uint32_t edb_fnv1a(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

uint32_t edb_murmur3_final(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* ===== 位图 ===== */

int edb_bit_get(const unsigned char* bits, uint32_t idx) {
    return (bits[idx >> 3] >> (idx & 7)) & 1;
}

void edb_bit_set(unsigned char* bits, uint32_t idx) {
    bits[idx >> 3] |= (unsigned char)(1u << (idx & 7));
}

void edb_bit_clear(unsigned char* bits, uint32_t idx) {
    bits[idx >> 3] &= (unsigned char)~(1u << (idx & 7));
}

/* ===== 动态数组 ===== */

int edb_ensure_cap(void** arr, uint32_t* cap, uint32_t needed, uint32_t elem_size) {
    if (needed <= *cap) return 0;
    uint32_t new_cap = *cap ? *cap : 16;
    while (new_cap < needed) new_cap *= 2;
    void* p = realloc(*arr, (size_t)new_cap * elem_size);
    if (!p) return -1;
    *arr = p;
    *cap = new_cap;
    return 0;
}

/* ===== UTF-8 ===== */

uint32_t edb_utf8_char_len(const uint8_t* s, size_t remain) {
    if (remain == 0) return 0;
    uint8_t b = s[0];
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return (remain >= 2) ? 2 : 1;
    if ((b & 0xF0) == 0xE0) return (remain >= 3) ? 3 : 1;
    if ((b & 0xF8) == 0xF0) return (remain >= 4) ? 4 : 1;
    return 1;
}

uint8_t edb_ascii_fold(uint8_t ch) {
    return (ch >= 'A' && ch <= 'Z') ? (uint8_t)(ch + ('a' - 'A')) : ch;
}

/* ===== 文件工具 ===== */

int64_t edb_util_file_size(FILE* fp) {
#if defined(_WIN32)
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return (int64_t)sz;
#else
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return (int64_t)sz;
#endif
}

int edb_file_sync(FILE* fp) {
    fflush(fp);
#if defined(_WIN32)
    return _commit(_fileno(fp));
#else
    return fsync(fileno(fp));
#endif
}

int edb_rename_atomic(const char* old_path, const char* new_path) {
#if defined(_WIN32)
    return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(old_path, new_path);
#endif
}

/* ===== 变长整数 ===== */

int edb_write_varuint(uint8_t* buf, uint32_t val) {
    int i = 0;
    while (val >= 0x80) {
        buf[i++] = (uint8_t)(val | 0x80);
        val >>= 7;
    }
    buf[i++] = (uint8_t)val;
    return i;
}

int edb_read_varuint(const uint8_t* buf, uint32_t max_len, uint32_t* val) {
    *val = 0;
    int shift = 0;
    for (uint32_t i = 0; i < max_len && i < 5; i++) {
        uint8_t b = buf[i];
        *val |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return (int)i + 1;
        shift += 7;
    }
    return -1;
}

int edb_varuint_size(uint32_t val) {
    int n = 0;
    do { n++; val >>= 7; } while (val);
    return n;
}
