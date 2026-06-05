#include "ezdb_query.h"
#include "ezdb_internal.h"
#include "ezdb_postings.h"

#include <stdlib.h>
#include <string.h>

typedef struct EzdbQueryParser {
    const char* text;
    size_t len;
    size_t pos;
    int error;
} EzdbQueryParser;

static unsigned char ezdb_query_fold_ascii_byte(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (unsigned char)(ch + ('a' - 'A')) : ch;
}

static char* ezdb_query_strdup_range(const char* text, size_t len)
{
    char* out = (char*)malloc(len + 1u);
    if (!out) return NULL;
    if (len) memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

int ezdb_query_contains_ascii_casefold(const char* text, size_t text_len, const char* needle, size_t needle_len)
{
    if (!needle_len) return 1;
    if (needle_len > text_len) return 0;
    for (size_t i = 0; i + needle_len <= text_len; ++i) {
        size_t j = 0;
        while (j < needle_len &&
               ezdb_query_fold_ascii_byte((unsigned char)text[i + j]) == ezdb_query_fold_ascii_byte((unsigned char)needle[j])) ++j;
        if (j == needle_len) return 1;
    }
    return 0;
}

int ezdb_query_is_space(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static void ezdb_query_skip_spaces(EzdbQueryParser* p)
{
    while (p->pos < p->len && ezdb_query_is_space((unsigned char)p->text[p->pos])) ++p->pos;
}

static EzdbQueryNode* ezdb_query_node_new(EzdbQueryNodeType type,
                                          EzdbQueryNode* left,
                                          EzdbQueryNode* right,
                                          char* text,
                                          size_t text_len)
{
    EzdbQueryNode* node = (EzdbQueryNode*)calloc(1, sizeof(EzdbQueryNode));
    if (!node) {
        free(text);
        return NULL;
    }
    node->type = type;
    node->left = left;
    node->right = right;
    node->text = text;
    node->text_len = text_len;
    return node;
}

void ezdb_query_node_free(EzdbQueryNode* node)
{
    if (!node) return;
    ezdb_query_node_free(node->left);
    ezdb_query_node_free(node->right);
    free(node->text);
    free(node);
}

static EzdbQueryNode* ezdb_query_parse_or(EzdbQueryParser* p);

static int ezdb_query_starts_primary(EzdbQueryParser* p)
{
    ezdb_query_skip_spaces(p);
    if (p->pos >= p->len) return 0;
    return p->text[p->pos] != ')' && p->text[p->pos] != '|';
}

static int ezdb_query_text_has_wildcard(const char* text, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '*' || text[i] == '?') return 1;
    }
    return 0;
}

static EzdbQueryNode* ezdb_query_parse_primary(EzdbQueryParser* p)
{
    ezdb_query_skip_spaces(p);
    if (p->pos >= p->len) {
        p->error = 1;
        return NULL;
    }
    if (p->text[p->pos] == '(') {
        ++p->pos;
        EzdbQueryNode* inner = ezdb_query_parse_or(p);
        ezdb_query_skip_spaces(p);
        if (!inner || p->pos >= p->len || p->text[p->pos] != ')') {
            ezdb_query_node_free(inner);
            p->error = 1;
            return NULL;
        }
        ++p->pos;
        return inner;
    }
    if (p->text[p->pos] == '"') {
        size_t start = ++p->pos;
        while (p->pos < p->len && p->text[p->pos] != '"') ++p->pos;
        if (p->pos >= p->len) {
            p->error = 1;
            return NULL;
        }
        size_t len = p->pos - start;
        char* text = ezdb_query_strdup_range(p->text + start, len);
        ++p->pos;
        if (!text) {
            p->error = 1;
            return NULL;
        }
        return ezdb_query_node_new(EZDB_QUERY_TERM, NULL, NULL, text, len);
    }
    if (p->text[p->pos] == ')' || p->text[p->pos] == '|') {
        p->error = 1;
        return NULL;
    }
    size_t start = p->pos;
    while (p->pos < p->len &&
           !ezdb_query_is_space((unsigned char)p->text[p->pos]) &&
           p->text[p->pos] != '(' &&
           p->text[p->pos] != ')' &&
           p->text[p->pos] != '|' &&
           p->text[p->pos] != '!') {
        ++p->pos;
    }
    if (p->pos == start) {
        p->error = 1;
        return NULL;
    }
    size_t len = p->pos - start;
    char* text = ezdb_query_strdup_range(p->text + start, len);
    if (!text) {
        p->error = 1;
        return NULL;
    }
    return ezdb_query_node_new(ezdb_query_text_has_wildcard(text, len) ? EZDB_QUERY_WILDCARD : EZDB_QUERY_TERM,
                               NULL,
                               NULL,
                               text,
                               len);
}

static EzdbQueryNode* ezdb_query_parse_not(EzdbQueryParser* p)
{
    ezdb_query_skip_spaces(p);
    if (p->pos < p->len && p->text[p->pos] == '!') {
        ++p->pos;
        EzdbQueryNode* child = ezdb_query_parse_not(p);
        if (!child) {
            p->error = 1;
            return NULL;
        }
        EzdbQueryNode* node = ezdb_query_node_new(EZDB_QUERY_NOT, child, NULL, NULL, 0);
        if (!node) {
            ezdb_query_node_free(child);
            p->error = 1;
        }
        return node;
    }
    return ezdb_query_parse_primary(p);
}

static EzdbQueryNode* ezdb_query_parse_and(EzdbQueryParser* p)
{
    EzdbQueryNode* left = ezdb_query_parse_not(p);
    if (!left) return NULL;
    while (!p->error && ezdb_query_starts_primary(p)) {
        EzdbQueryNode* right = ezdb_query_parse_not(p);
        if (!right) {
            ezdb_query_node_free(left);
            return NULL;
        }
        EzdbQueryNode* parent = ezdb_query_node_new(EZDB_QUERY_AND, left, right, NULL, 0);
        if (!parent) {
            ezdb_query_node_free(left);
            ezdb_query_node_free(right);
            p->error = 1;
            return NULL;
        }
        left = parent;
    }
    return left;
}

static EzdbQueryNode* ezdb_query_parse_or(EzdbQueryParser* p)
{
    EzdbQueryNode* left = ezdb_query_parse_and(p);
    if (!left) return NULL;
    for (;;) {
        ezdb_query_skip_spaces(p);
        if (p->pos >= p->len || p->text[p->pos] != '|') break;
        ++p->pos;
        EzdbQueryNode* right = ezdb_query_parse_and(p);
        if (!right) {
            ezdb_query_node_free(left);
            return NULL;
        }
        EzdbQueryNode* parent = ezdb_query_node_new(EZDB_QUERY_OR, left, right, NULL, 0);
        if (!parent) {
            ezdb_query_node_free(left);
            ezdb_query_node_free(right);
            p->error = 1;
            return NULL;
        }
        left = parent;
    }
    return left;
}

EzdbQueryNode* ezdb_query_parse(const char* keyword)
{
    EzdbQueryParser p;
    memset(&p, 0, sizeof(p));
    p.text = keyword;
    p.len = strlen(keyword);
    ezdb_query_skip_spaces(&p);
    if (p.pos >= p.len) return NULL;
    EzdbQueryNode* root = ezdb_query_parse_or(&p);
    ezdb_query_skip_spaces(&p);
    if (p.error || p.pos != p.len) {
        ezdb_query_node_free(root);
        return NULL;
    }
    return root;
}

static int ezdb_query_wildcard_match_here(const char* text, size_t text_len, const char* pattern, size_t pattern_len)
{
    size_t ti = 0;
    size_t pi = 0;
    size_t star_pi = SIZE_MAX;
    size_t star_ti = 0;
    while (ti < text_len) {
        if (pi >= pattern_len) return 1;
        if (pi < pattern_len && pattern[pi] == '*') {
            while (pi < pattern_len && pattern[pi] == '*') ++pi;
            if (pi >= pattern_len) return 1;
            star_pi = pi;
            star_ti = ti;
            continue;
        }
        if (pi < pattern_len && pattern[pi] == '?') {
            ti += (size_t)ezdb_postings_utf8_token_len((const unsigned char*)text + ti, text_len - ti);
            ++pi;
            continue;
        }
        if (pi < pattern_len &&
            ezdb_query_fold_ascii_byte((unsigned char)text[ti]) == ezdb_query_fold_ascii_byte((unsigned char)pattern[pi])) {
            ++ti;
            ++pi;
            continue;
        }
        if (star_pi != SIZE_MAX) {
            star_ti += (size_t)ezdb_postings_utf8_token_len((const unsigned char*)text + star_ti, text_len - star_ti);
            ti = star_ti;
            pi = star_pi;
            continue;
        }
        return 0;
    }
    while (pi < pattern_len && pattern[pi] == '*') ++pi;
    return pi == pattern_len;
}

static int ezdb_query_wildcard_contains_ascii_casefold(const char* text, size_t text_len, const char* pattern, size_t pattern_len)
{
    if (!pattern_len) return 1;
    if (pattern[0] == '*') return ezdb_query_wildcard_match_here(text, text_len, pattern, pattern_len);
    for (size_t i = 0; i <= text_len; ) {
        if (ezdb_query_wildcard_match_here(text + i, text_len - i, pattern, pattern_len)) return 1;
        if (i == text_len) break;
        i += (size_t)ezdb_postings_utf8_token_len((const unsigned char*)text + i, text_len - i);
    }
    return 0;
}

int ezdb_query_match_path(const EzdbQueryNode* node, const char* path, size_t path_len)
{
    if (!node) return 1;
    switch (node->type) {
    case EZDB_QUERY_TERM:
        return ezdb_query_contains_ascii_casefold(path, path_len, node->text, node->text_len);
    case EZDB_QUERY_WILDCARD:
        return ezdb_query_wildcard_contains_ascii_casefold(path, path_len, node->text, node->text_len);
    case EZDB_QUERY_NOT:
        return !ezdb_query_match_path(node->left, path, path_len);
    case EZDB_QUERY_AND:
        return ezdb_query_match_path(node->left, path, path_len) && ezdb_query_match_path(node->right, path, path_len);
    case EZDB_QUERY_OR:
        return ezdb_query_match_path(node->left, path, path_len) || ezdb_query_match_path(node->right, path, path_len);
    default:
        return 0;
    }
}

int ezdb_query_matches_text(EzdbQueryNode* root, const char* keyword, const char* text, size_t text_len)
{
    if (root) return ezdb_query_match_path(root, text, text_len);
    return ezdb_query_contains_ascii_casefold(text, text_len, keyword, strlen(keyword));
}

int ezdb_query_longest_literal_from_wildcard(const char* text, size_t len, const char** out_text, size_t* out_len)
{
    size_t best_start = 0;
    size_t best_len = 0;
    size_t start = 0;
    size_t cur_len = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '*' || text[i] == '?') {
            if (cur_len > best_len) {
                best_start = start;
                best_len = cur_len;
            }
            start = i + 1u;
            cur_len = 0;
        } else {
            ++cur_len;
        }
    }
    if (cur_len > best_len) {
        best_start = start;
        best_len = cur_len;
    }
    if (!best_len) return 0;
    *out_text = text + best_start;
    *out_len = best_len;
    return 1;
}

int ezdb_query_build_candidate_keys(const char* text, size_t len, uint32_t** out_keys, uint32_t* out_count)
{
    if (!len || len > UINT32_MAX) {
        *out_keys = NULL;
        *out_count = 0;
        return EZDB_OK;
    }
    return ezdb_postings_build_query_keys(text, out_keys, out_count);
}
