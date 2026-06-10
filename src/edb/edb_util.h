#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* ===== 线程抽象 ===== */

typedef struct {
#if defined(_WIN32)
    void* handle;
#else
    unsigned long id;
#endif
} EdbThread;

typedef void* (*edb_thread_fn)(void* arg);

int edb_thread_create(EdbThread* t, edb_thread_fn fn, void* arg);
int edb_thread_join(EdbThread* t);

/* ===== 哈希 ===== */

uint32_t edb_fnv1a(const void* data, size_t len);
uint32_t edb_murmur3_final(uint32_t h);

/* ===== 位图 ===== */

int  edb_bit_get(const unsigned char* bits, uint32_t idx);
void edb_bit_set(unsigned char* bits, uint32_t idx);
void edb_bit_clear(unsigned char* bits, uint32_t idx);

/* ===== 动态数组 ===== */

int edb_ensure_cap(void** arr, uint32_t* cap, uint32_t needed, uint32_t elem_size);

/* ===== UTF-8 ===== */

uint32_t edb_utf8_char_len(const uint8_t* s, size_t remain);
uint8_t  edb_ascii_fold(uint8_t ch);

/* ===== 文件工具 ===== */

int64_t edb_util_file_size(FILE* fp);
int     edb_file_sync(FILE* fp);
int     edb_rename_atomic(const char* old_path, const char* new_path);

/* ===== 变长整数 ===== */

int edb_write_varuint(uint8_t* buf, uint32_t val);
int edb_read_varuint(const uint8_t* buf, uint32_t max_len, uint32_t* val);
int edb_varuint_size(uint32_t val);
