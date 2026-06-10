#pragma once

#include "edb.h"
#include "edb_format.h"
#include "edb_postings.h"
#include <stdint.h>

/* ===== 查询 AST ===== */

typedef enum {
    EDB_QUERY_TERM     = 1,
    EDB_QUERY_WILDCARD = 2,
    EDB_QUERY_NOT      = 3,
    EDB_QUERY_AND      = 4,
    EDB_QUERY_OR       = 5
} EdbQueryNodeType;

typedef struct EdbQueryNode {
    EdbQueryNodeType type;
    char*  text;
    size_t text_len;
    struct EdbQueryNode* left;
    struct EdbQueryNode* right;
} EdbQueryNode;

/* 解析搜索关键词为 AST */
EdbQueryNode* edb_query_parse(const char* keyword);

/* 释放 AST */
void edb_query_free(EdbQueryNode* node);

/* ===== 搜索评估 ===== */

/* 在内存中的 postings + index 上执行查询，返回匹配的 ID 列表 */
int edb_search_eval(EdbQueryNode* ast,
                     const unsigned char* postings_base, uint64_t postings_size,
                     EdbDiskIndex* index, uint32_t index_count,
                     uint32_t universe_count,
                     uint32_t** out_ids, uint32_t* out_count);

/* 通配符匹配（支持 * 和 ?） */
int edb_wildcard_match(const char* pattern, size_t pat_len,
                        const char* text, size_t text_len);

/* 提取字面子串的最长连续序列的 gram keys */
int edb_search_extract_literal_grams(const char* text, size_t len,
                                      uint32_t** out_keys, uint32_t* out_count);
