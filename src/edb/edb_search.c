#include "edb_search.h"
#include "edb_util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ===== 通配符匹配 ===== */

int edb_wildcard_match(const char* pattern, size_t pat_len,
                        const char* text, size_t text_len) {
    /* 简化版递归匹配 */
    size_t pi = 0, ti = 0;
    size_t star_pi = (size_t)-1, star_ti = (size_t)-1;

    while (ti < text_len) {
        if (pi < pat_len && (pattern[pi] == '?' ||
            edb_ascii_fold((uint8_t)pattern[pi]) == edb_ascii_fold((uint8_t)text[ti]))) {
            pi++; ti++;
        } else if (pi < pat_len && pattern[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (star_pi != (size_t)-1) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return 0;
        }
    }
    while (pi < pat_len && pattern[pi] == '*') pi++;
    return pi == pat_len ? 1 : 0;
}

/* ===== AST 节点创建 ===== */

static EdbQueryNode* edb_node_new(EdbQueryNodeType type) {
    EdbQueryNode* n = (EdbQueryNode*)calloc(1, sizeof(EdbQueryNode));
    if (n) n->type = type;
    return n;
}

void edb_query_free(EdbQueryNode* node) {
    if (!node) return;
    edb_query_free(node->left);
    edb_query_free(node->right);
    free(node->text);
    free(node);
}

/* ===== 递归下降解析器 ===== */

typedef struct {
    const char* src;
    size_t      len;
    size_t      pos;
} EdbParser;

static char edb_parser_peek(EdbParser* p) {
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static char edb_parser_next(EdbParser* p) {
    return p->pos < p->len ? p->src[p->pos++] : '\0';
}

static int edb_is_special(char c) {
    return c == '|' || c == '(' || c == ')' || c == '!' || c == '"' || c == '*';
}

/* 解析原子：关键词、带引号短语、通配符 */
static EdbQueryNode* edb_parse_atom(EdbParser* p) {
    /* 跳过空白 */
    while (p->pos < p->len && p->src[p->pos] == ' ') p->pos++;
    if (p->pos >= p->len) return NULL;

    char c = edb_parser_peek(p);

    /* 括号 */
    if (c == '(') {
        edb_parser_next(p);
        EdbQueryNode* inner = edb_query_parse(p->src + p->pos);
        /* 在实际中我们需要递归解析，这里简化：找到匹配 ) */
        /* 重新设计：返回到调用者，由 parse_or 处理 */
        return NULL; /* TODO: 括号支持在完整实现中 */
    }

    /* 引号短语 */
    if (c == '"') {
        edb_parser_next(p);
        size_t start = p->pos;
        while (p->pos < p->len && p->src[p->pos] != '"') p->pos++;
        size_t len = p->pos - start;
        if (p->pos < p->len) p->pos++; /* skip closing " */

        EdbQueryNode* n = edb_node_new(EDB_QUERY_TERM);
        if (n) {
            n->text = (char*)malloc(len + 1);
            if (n->text) { memcpy(n->text, p->src + start, len); n->text[len] = '\0'; }
            n->text_len = len;
        }
        return n;
    }

    /* 普通关键词或通配符 */
    size_t start = p->pos;
    int has_wildcard = 0;
    while (p->pos < p->len) {
        char ch = p->src[p->pos];
        if (ch == ' ' || ch == '|' || ch == '(' || ch == ')' || ch == '"') break;
        if (ch == '*' || ch == '?') has_wildcard = 1;
        p->pos++;
    }
    size_t len = p->pos - start;
    if (len == 0) return NULL;

    EdbQueryNode* n = edb_node_new(has_wildcard ? EDB_QUERY_WILDCARD : EDB_QUERY_TERM);
    if (n) {
        n->text = (char*)malloc(len + 1);
        if (n->text) { memcpy(n->text, p->src + start, len); n->text[len] = '\0'; }
        n->text_len = len;
    }
    return n;
}

/* 解析 NOT 前缀 */
static EdbQueryNode* edb_parse_not(EdbParser* p) {
    while (p->pos < p->len && p->src[p->pos] == ' ') p->pos++;

    if (p->pos < p->len && p->src[p->pos] == '!') {
        edb_parser_next(p);
        EdbQueryNode* child = edb_parse_atom(p);
        if (!child) return NULL;
        EdbQueryNode* n = edb_node_new(EDB_QUERY_NOT);
        if (n) { n->left = child; }
        else { edb_query_free(child); }
        return n;
    }
    return edb_parse_atom(p);
}

/* 解析隐式 AND */
static EdbQueryNode* edb_parse_and(EdbParser* p) {
    EdbQueryNode* left = edb_parse_not(p);
    if (!left) return NULL;

    while (1) {
        /* 跳过空白 */
        size_t save = p->pos;
        while (p->pos < p->len && p->src[p->pos] == ' ') p->pos++;

        /* 检查是否有下一个 token */
        if (p->pos >= p->len || p->src[p->pos] == '|' || p->src[p->pos] == ')') {
            p->pos = save;
            break;
        }
        /* 如果下一个字符是 ! 后跟其他，或者 " 或者非特殊字符，则是隐式 AND */
        char next = p->src[p->pos];
        if (next == '!' || next == '"' || next == '(' ||
            (next != '|' && next != ')' && next != ' ')) {
            EdbQueryNode* right = edb_parse_not(p);
            if (!right) break;
            EdbQueryNode* and_node = edb_node_new(EDB_QUERY_AND);
            if (and_node) {
                and_node->left = left;
                and_node->right = right;
                left = and_node;
            } else {
                edb_query_free(right);
                break;
            }
        } else {
            p->pos = save;
            break;
        }
    }
    return left;
}

/* 解析 OR */
static EdbQueryNode* edb_parse_or(EdbParser* p) {
    EdbQueryNode* left = edb_parse_and(p);
    if (!left) return NULL;

    while (1) {
        while (p->pos < p->len && p->src[p->pos] == ' ') p->pos++;
        if (p->pos >= p->len || p->src[p->pos] != '|') break;
        edb_parser_next(p); /* skip | */

        EdbQueryNode* right = edb_parse_and(p);
        if (!right) break;
        EdbQueryNode* or_node = edb_node_new(EDB_QUERY_OR);
        if (or_node) {
            or_node->left = left;
            or_node->right = right;
            left = or_node;
        } else {
            edb_query_free(right);
            break;
        }
    }
    return left;
}

EdbQueryNode* edb_query_parse(const char* keyword) {
    if (!keyword || !*keyword) return NULL;
    EdbParser p;
    p.src = keyword;
    p.len = strlen(keyword);
    p.pos = 0;
    return edb_parse_or(&p);
}

/* ===== 搜索评估 ===== */

/* 提取字面子串 gram keys（用于 TERM 节点） */
int edb_search_extract_literal_grams(const char* text, size_t len,
                                      uint32_t** out_keys, uint32_t* out_count) {
    EdbQueryKeyCollector col = {NULL, 0, 0};
    EdbGramKeyCallback cb;
    cb.emit = edb_collect_query_key;
    cb.user_data = &col;

    /* 构造临时 null-terminated 字符串 */
    char* tmp = (char*)malloc(len + 1);
    if (!tmp) return EDB_ERR_MEMORY;
    memcpy(tmp, text, len);
    tmp[len] = '\0';

    int rc = edb_postings_enumerate_gram_keys(tmp, &cb);
    free(tmp);
    if (rc < 0) { free(col.keys); return rc; }

    *out_keys = col.keys;
    *out_count = col.count;
    return EDB_OK;
}

/* 提取通配符模式中字面子串的 gram keys */
static int edb_extract_wildcard_grams(const char* text, size_t len,
                                       uint32_t** out_keys, uint32_t* out_count) {
    /* 找到最长不含 * ? 的连续子串 */
    size_t best_start = 0, best_len = 0;
    size_t cur_start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i < len && text[i] != '*' && text[i] != '?') {
            /* 继续 */
        } else {
            size_t cur_len = i - cur_start;
            if (cur_len > best_len) {
                best_start = cur_start;
                best_len = cur_len;
            }
            cur_start = i + 1;
        }
    }

    if (best_len == 0) {
        *out_keys = NULL;
        *out_count = 0;
        return EDB_OK;
    }

    return edb_search_extract_literal_grams(text + best_start, best_len, out_keys, out_count);
}

/* 并集操作 */
static uint32_t* edb_union_sorted(const uint32_t* a, uint32_t a_count,
                                    const uint32_t* b, uint32_t b_count,
                                    uint32_t* out_count) {
    uint32_t cap = a_count + b_count;
    uint32_t* out = (uint32_t*)malloc(cap * sizeof(uint32_t));
    if (!out) { *out_count = 0; return NULL; }

    uint32_t i = 0, j = 0, k = 0;
    while (i < a_count && j < b_count) {
        if (a[i] < b[j]) out[k++] = a[i++];
        else if (a[i] > b[j]) out[k++] = b[j++];
        else { out[k++] = a[i++]; j++; }
    }
    while (i < a_count) out[k++] = a[i++];
    while (j < b_count) out[k++] = b[j++];

    *out_count = k;
    return out;
}

/* 差集操作 */
static uint32_t* edb_subtract_sorted(const uint32_t* a, uint32_t a_count,
                                       const uint32_t* b, uint32_t b_count,
                                       uint32_t* out_count) {
    uint32_t* out = (uint32_t*)malloc(a_count * sizeof(uint32_t));
    if (!out) { *out_count = 0; return NULL; }

    uint32_t i = 0, j = 0, k = 0;
    while (i < a_count) {
        while (j < b_count && b[j] < a[i]) j++;
        if (j < b_count && b[j] == a[i]) { i++; j++; }
        else out[k++] = a[i++];
    }
    *out_count = k;
    return out;
}

int edb_search_eval(EdbQueryNode* ast,
                     const unsigned char* postings_base, uint64_t postings_size,
                     EdbDiskIndex* index, uint32_t index_count,
                     uint32_t universe_count,
                     uint32_t** out_ids, uint32_t* out_count) {
    (void)universe_count;
    if (!ast) { *out_ids = NULL; *out_count = 0; return EDB_OK; }

    switch (ast->type) {
    case EDB_QUERY_TERM: {
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        int rc = edb_search_extract_literal_grams(ast->text, ast->text_len, &keys, &key_count);
        if (rc != EDB_OK) return rc;

        rc = edb_postings_load_intersected(postings_base, postings_size,
                                            index, index_count,
                                            keys, key_count, out_ids, out_count);
        free(keys);
        return rc;
    }

    case EDB_QUERY_WILDCARD: {
        /* 先用字面子串 gram 缩小候选集 */
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        int rc = edb_extract_wildcard_grams(ast->text, ast->text_len, &keys, &key_count);
        if (rc != EDB_OK) return rc;

        uint32_t* candidates = NULL;
        uint32_t cand_count = 0;

        if (key_count > 0) {
            rc = edb_postings_load_intersected(postings_base, postings_size,
                                                index, index_count,
                                                keys, key_count, &candidates, &cand_count);
            free(keys);
            if (rc != EDB_OK) return rc;
        } else {
            /* 无 literal 子串，返回空 */
            free(keys);
            *out_ids = NULL;
            *out_count = 0;
            return EDB_OK;
        }

        /* 通配符后过滤：需要外部提供路径访问，这里只返回候选集 */
        /* 实际过滤在调用方完成 */
        *out_ids = candidates;
        *out_count = cand_count;
        return EDB_OK;
    }

    case EDB_QUERY_AND: {
        uint32_t* left_ids = NULL, *right_ids = NULL;
        uint32_t left_count = 0, right_count = 0;
        int rc = edb_search_eval(ast->left, postings_base, postings_size,
                                  index, index_count, universe_count,
                                  &left_ids, &left_count);
        if (rc != EDB_OK) return rc;
        rc = edb_search_eval(ast->right, postings_base, postings_size,
                              index, index_count, universe_count,
                              &right_ids, &right_count);
        if (rc != EDB_OK) { free(left_ids); return rc; }

        uint32_t* result = (uint32_t*)malloc(
            (left_count < right_count ? left_count : right_count) * sizeof(uint32_t));
        if (!result) { free(left_ids); free(right_ids); return EDB_ERR_MEMORY; }
        *out_count = edb_intersect_sorted(left_ids, left_count, right_ids, right_count, result);
        free(left_ids);
        free(right_ids);
        *out_ids = result;
        return EDB_OK;
    }

    case EDB_QUERY_OR: {
        uint32_t* left_ids = NULL, *right_ids = NULL;
        uint32_t left_count = 0, right_count = 0;
        int rc = edb_search_eval(ast->left, postings_base, postings_size,
                                  index, index_count, universe_count,
                                  &left_ids, &left_count);
        if (rc != EDB_OK) return rc;
        rc = edb_search_eval(ast->right, postings_base, postings_size,
                              index, index_count, universe_count,
                              &right_ids, &right_count);
        if (rc != EDB_OK) { free(left_ids); return rc; }

        *out_ids = edb_union_sorted(left_ids, left_count, right_ids, right_count, out_count);
        free(left_ids);
        free(right_ids);
        return *out_ids ? EDB_OK : EDB_ERR_MEMORY;
    }

    case EDB_QUERY_NOT: {
        /* NOT 需要上下文（全集），这里返回匹配的 ID，由调用方做减法 */
        return edb_search_eval(ast->left, postings_base, postings_size,
                                index, index_count, universe_count,
                                out_ids, out_count);
    }
    }

    *out_ids = NULL;
    *out_count = 0;
    return EDB_OK;
}
