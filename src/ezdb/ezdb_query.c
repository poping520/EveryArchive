#include "ezdb_query.h"
#include "ezdb_core_internal.h"
#include "ezdb_entries.h"
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
        char* text = ezdb_strdup_range(p->text + start, len);
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
    char* text = ezdb_strdup_range(p->text + start, len);
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

/* Query execution */

typedef struct EzdbArchiveSearchAdapter {
    Ezdb* db;
    EzdbSearchV2Callback callback;
    void* user_data;
    uint32_t emitted;
} EzdbArchiveSearchAdapter;
typedef struct EzdbIdVec {
    uint32_t* ids;
    uint32_t count;
    uint32_t cap;
} EzdbIdVec;
static int record_contains_keyword(Ezdb* db, uint32_t id, const char* keyword, size_t key_len)
{
    EzdbDeltaRecord* delta = ezdb_find_delta_record(db, id);
    if (delta) {
        return delta->type != EZDB_DELTA_DELETE &&
               ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, key_len);
    }
    if (id >= db->header.base_file_count) return 0;
    if (ezdb_query_contains_ascii_casefold(ezdb_file_name_by_id(db, id), db->file_name_lens[id], keyword, key_len)) return 1;
    char* path = NULL;
    EzdbSearchResult result;
    if (ezdb_build_result_path(db, id, &result) != EZDB_OK) return 0;
    path = result.path;
    int matched = ezdb_query_contains_ascii_casefold(path, strlen(path), keyword, key_len);
    ezdb_free_result(&result);
    return matched;
}

static int mark_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    char* keyword = ezdb_strdup_range(literal, literal_len);
    if (!keyword) return EZDB_ERR_MEMORY;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
    if (rc != EZDB_OK) {
        free(keyword);
        return rc;
    }
    if (!key_count) {
        free(keys);
        free(keyword);
        return EZDB_OK;
    }

    uint32_t* file_ids = NULL;
    uint32_t file_count = 0;
    uint32_t* dir_ids = NULL;
    uint32_t dir_count = 0;
    rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->file_index, db->header.file_index_count, keys, key_count, &file_ids, &file_count);
    if (rc == EZDB_OK) rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->dir_index, db->header.dir_index_count, keys, key_count, &dir_ids, &dir_count);
    free(keys);
    if (rc != EZDB_OK) {
        free(file_ids);
        free(dir_ids);
        free(keyword);
        return rc;
    }

    for (uint32_t i = 0; i < file_count; ++i) {
        if (file_ids[i] < db->header.base_file_count &&
            ezdb_bitset_get(db->active_bits, file_ids[i]) &&
            !ezdb_bitset_get(db->covered_base_bits, file_ids[i])) {
            seen[file_ids[i] >> 3u] |= (unsigned char)(1u << (file_ids[i] & 7u));
            *any_marked = 1;
        }
    }
    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t dir_id = dir_ids[i];
        if (dir_id >= db->header.dir_count) continue;
        EzdbDiskDir* d = &db->dirs[dir_id];
        uint32_t end = d->first_file_id + d->file_count;
        if (end > db->header.base_file_count) end = (uint32_t)db->header.base_file_count;
        for (uint32_t id = d->first_file_id; id < end; ++id) {
            if (ezdb_bitset_get(db->active_bits, id) && !ezdb_bitset_get(db->covered_base_bits, id)) {
                seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                *any_marked = 1;
            }
        }
    }
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        EzdbDeltaRecord* delta = &db->deltas[i];
        if (ezdb_find_delta_record(db, delta->id) != delta) continue;
        if (delta->type == EZDB_DELTA_DELETE || !ezdb_bitset_get(db->active_bits, delta->id)) continue;
        if (ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, literal_len)) {
            seen[delta->id >> 3u] |= (unsigned char)(1u << (delta->id & 7u));
            *any_marked = 1;
        }
    }

    free(file_ids);
    free(dir_ids);
    free(keyword);
    return EZDB_OK;
}

int ezdb_bitset_or_into(unsigned char* dst, const unsigned char* src, size_t size)
{
    for (size_t i = 0; i < size; ++i) dst[i] |= src[i];
    return EZDB_OK;
}

int ezdb_bitset_and_into(unsigned char* dst, const unsigned char* src, size_t size)
{
    for (size_t i = 0; i < size; ++i) dst[i] &= src[i];
    return EZDB_OK;
}

int ezdb_bitset_any(const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (data[i]) return 1;
    }
    return 0;
}

static int query_build_candidate_bitset(Ezdb* db, EzdbQueryNode* node, unsigned char** out_bits, int* out_has_positive)
{
    *out_bits = NULL;
    *out_has_positive = 0;
    if (!node) return EZDB_OK;
    size_t bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    if (node->type == EZDB_QUERY_NOT) return EZDB_OK;
    if (node->type == EZDB_QUERY_TERM || node->type == EZDB_QUERY_WILDCARD) {
        const char* literal = NULL;
        size_t literal_len = 0;
        if (node->type == EZDB_QUERY_TERM) {
            literal = node->text;
            literal_len = node->text_len;
        } else if (!ezdb_query_longest_literal_from_wildcard(node->text, node->text_len, &literal, &literal_len)) {
            return EZDB_OK;
        }
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_literal_candidates(db, literal, literal_len, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_AND || node->type == EZDB_QUERY_OR) {
        unsigned char* left = NULL;
        unsigned char* right = NULL;
        int left_positive = 0;
        int right_positive = 0;
        int rc = query_build_candidate_bitset(db, node->left, &left, &left_positive);
        if (rc == EZDB_OK) rc = query_build_candidate_bitset(db, node->right, &right, &right_positive);
        if (rc != EZDB_OK) {
            free(left);
            free(right);
            return rc;
        }
        if (node->type == EZDB_QUERY_AND) {
            if (left_positive && right_positive) {
                ezdb_bitset_and_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else if (left_positive) {
                *out_bits = left;
                *out_has_positive = 1;
                free(right);
            } else if (right_positive) {
                *out_bits = right;
                *out_has_positive = 1;
                free(left);
            } else {
                free(left);
                free(right);
            }
        } else {
            if (left_positive && right_positive) {
                ezdb_bitset_or_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else {
                free(left);
                free(right);
                *out_bits = NULL;
                *out_has_positive = 0;
            }
        }
    }
    return EZDB_OK;
}
int ezdb_entry_is_searchable(Ezdb* db, uint32_t entry_id)
{
    if (!db || entry_id >= db->header.entry_count || !ezdb_bitset_get(db->active_entry_bits, entry_id)) return 0;
    uint32_t archive_id = db->entry_archive_ids[entry_id];
    return archive_id < db->header.file_count && ezdb_bitset_get(db->active_bits, archive_id);
}

static int mark_entry_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    int rc = EZDB_OK;
    if (db->entry_index && db->header.entry_index_count) {
        char* keyword = ezdb_strdup_range(literal, literal_len);
        if (!keyword) return EZDB_ERR_MEMORY;
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
        if (rc != EZDB_OK) {
            free(keyword);
            return rc;
        }
        if (key_count) {
            uint32_t* entry_ids = NULL;
            uint32_t entry_count = 0;
            rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->entry_index, db->header.entry_index_count, keys, key_count, &entry_ids, &entry_count);
            if (rc != EZDB_OK) {
                free(entry_ids);
                free(keys);
                free(keyword);
                return rc;
            }
            for (uint32_t i = 0; i < entry_count; ++i) {
                uint32_t id = entry_ids[i];
                if (ezdb_entry_is_searchable(db, id)) {
                    seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                    *any_marked = 1;
                }
            }
            free(entry_ids);
        }
        free(keys);
        free(keyword);
    }
    if (db->delta_entry_index_ready && db->delta_entry_index.entry_count) {
        char* keyword = ezdb_strdup_range(literal, literal_len);
        if (!keyword) return EZDB_ERR_MEMORY;
        uint32_t* keys = NULL;
        uint32_t key_count = 0;
        rc = ezdb_query_build_candidate_keys(keyword, literal_len, &keys, &key_count);
        if (rc != EZDB_OK) {
            free(keyword);
            return rc;
        }
        if (key_count) {
            uint32_t* entry_ids = NULL;
            uint32_t entry_count = 0;
            rc = ezdb_postings_load_intersected_memory(&db->delta_entry_index, keys, key_count, &entry_ids, &entry_count);
            if (rc != EZDB_OK) {
                free(entry_ids);
                free(keys);
                free(keyword);
                return rc;
            }
            for (uint32_t i = 0; i < entry_count; ++i) {
                uint32_t id = entry_ids[i];
                if (ezdb_entry_is_searchable(db, id) && ezdb_bitset_get(db->delta_entry_bits, id)) {
                    seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
                    *any_marked = 1;
                }
            }
            free(entry_ids);
        }
        free(keys);
        free(keyword);
    }
    return EZDB_OK;
}

static int mark_archive_literal_entry_candidates(Ezdb* db, const char* literal, size_t literal_len, unsigned char* seen, int* any_marked)
{
    size_t archive_bit_bytes = ((size_t)db->header.file_count + 7u) / 8u;
    unsigned char* archive_bits = (unsigned char*)calloc(archive_bit_bytes ? archive_bit_bytes : 1u, 1);
    if (!archive_bits) return EZDB_ERR_MEMORY;
    int archive_marked = 0;
    int rc = mark_literal_candidates(db, literal, literal_len, archive_bits, &archive_marked);
    if (rc != EZDB_OK) {
        free(archive_bits);
        return rc;
    }
    if (archive_marked) {
        for (uint32_t i = 0; i < db->header.entry_count; ++i) {
            if (ezdb_entry_is_searchable(db, i) &&
                (archive_bits[db->entry_archive_ids[i] >> 3u] & (unsigned char)(1u << (db->entry_archive_ids[i] & 7u)))) {
                seen[i >> 3u] |= (unsigned char)(1u << (i & 7u));
                *any_marked = 1;
            }
        }
    }
    free(archive_bits);
    return EZDB_OK;
}
static int mark_entry_scope_literal_candidates(Ezdb* db, const char* literal, size_t literal_len, uint32_t scope, unsigned char* seen, int* any_marked)
{
    int rc = EZDB_OK;
    if (scope & EZDB_SEARCH_ENTRY_PATH) {
        rc = mark_entry_literal_candidates(db, literal, literal_len, seen, any_marked);
    }
    if (rc == EZDB_OK && (scope & EZDB_SEARCH_COMBINED_PATH)) {
        rc = mark_entry_literal_candidates(db, literal, literal_len, seen, any_marked);
        if (rc == EZDB_OK) rc = mark_archive_literal_entry_candidates(db, literal, literal_len, seen, any_marked);
    }
    return rc;
}

static int query_build_entry_candidate_bitset(Ezdb* db, EzdbQueryNode* node, const char* fallback_keyword, uint32_t scope, unsigned char** out_bits, int* out_has_positive)
{
    *out_bits = NULL;
    *out_has_positive = 0;
    size_t bit_bytes = ((size_t)db->header.entry_count + 7u) / 8u;
    if (!node) {
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_entry_scope_literal_candidates(db, fallback_keyword, strlen(fallback_keyword), scope, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_NOT) return EZDB_OK;
    if (node->type == EZDB_QUERY_TERM || node->type == EZDB_QUERY_WILDCARD) {
        const char* literal = NULL;
        size_t literal_len = 0;
        if (node->type == EZDB_QUERY_TERM) {
            literal = node->text;
            literal_len = node->text_len;
        } else if (!ezdb_query_longest_literal_from_wildcard(node->text, node->text_len, &literal, &literal_len)) {
            return EZDB_OK;
        }
        unsigned char* bits = (unsigned char*)calloc(bit_bytes ? bit_bytes : 1u, 1);
        if (!bits) return EZDB_ERR_MEMORY;
        int any_marked = 0;
        int rc = mark_entry_scope_literal_candidates(db, literal, literal_len, scope, bits, &any_marked);
        if (rc != EZDB_OK) {
            free(bits);
            return rc;
        }
        *out_bits = bits;
        *out_has_positive = 1;
        return EZDB_OK;
    }
    if (node->type == EZDB_QUERY_AND || node->type == EZDB_QUERY_OR) {
        unsigned char* left = NULL;
        unsigned char* right = NULL;
        int left_positive = 0;
        int right_positive = 0;
        int rc = query_build_entry_candidate_bitset(db, node->left, fallback_keyword, scope, &left, &left_positive);
        if (rc == EZDB_OK) rc = query_build_entry_candidate_bitset(db, node->right, fallback_keyword, scope, &right, &right_positive);
        if (rc != EZDB_OK) {
            free(left);
            free(right);
            return rc;
        }
        if (node->type == EZDB_QUERY_AND) {
            if (left_positive && right_positive) {
                ezdb_bitset_and_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else if (left_positive) {
                *out_bits = left;
                *out_has_positive = 1;
                free(right);
            } else if (right_positive) {
                *out_bits = right;
                *out_has_positive = 1;
                free(left);
            } else {
                free(left);
                free(right);
            }
        } else {
            if (left_positive && right_positive) {
                ezdb_bitset_or_into(left, right, bit_bytes);
                free(right);
                *out_bits = left;
                *out_has_positive = 1;
            } else {
                free(left);
                free(right);
                *out_bits = NULL;
                *out_has_positive = 0;
            }
        }
    }
    return EZDB_OK;
}
static int ezdb_search_plain(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data)
{
    size_t key_len = strlen(keyword);
    if (!key_len) return EZDB_OK;
    uint32_t* keys = NULL;
    uint32_t key_count = 0;
    int rc = ezdb_postings_build_query_keys(keyword, &keys, &key_count);
    if (rc != EZDB_OK) return rc;

    uint32_t* file_ids = NULL;
    uint32_t file_count = 0;
    uint32_t* dir_ids = NULL;
    uint32_t dir_count = 0;
    rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->file_index, db->header.file_index_count, keys, key_count, &file_ids, &file_count);
    if (rc == EZDB_OK) rc = ezdb_postings_load_intersected(db->fp, db->header.postings_offset, db->dir_index, db->header.dir_index_count, keys, key_count, &dir_ids, &dir_count);
    free(keys);
    if (rc != EZDB_OK) {
        free(file_ids);
        free(dir_ids);
        return rc;
    }

    size_t seen_size = ((size_t)db->header.file_count + 7u) / 8u;
    unsigned char* seen = (unsigned char*)calloc(seen_size ? seen_size : 1u, 1);
    if (!seen && db->header.file_count) {
        free(file_ids);
        free(dir_ids);
        return EZDB_ERR_MEMORY;
    }
    for (uint32_t i = 0; i < file_count; ++i) {
        if (file_ids[i] < db->header.base_file_count &&
            ezdb_bitset_get(db->active_bits, file_ids[i]) &&
            !ezdb_bitset_get(db->covered_base_bits, file_ids[i])) {
            seen[file_ids[i] >> 3u] |= (unsigned char)(1u << (file_ids[i] & 7u));
        }
    }
    for (uint32_t i = 0; i < dir_count; ++i) {
        uint32_t dir_id = dir_ids[i];
        if (dir_id >= db->header.dir_count) continue;
        EzdbDiskDir* d = &db->dirs[dir_id];
        uint32_t end = d->first_file_id + d->file_count;
        if (end > db->header.base_file_count) end = (uint32_t)db->header.base_file_count;
        for (uint32_t id = d->first_file_id; id < end; ++id) {
            if (ezdb_bitset_get(db->active_bits, id) && !ezdb_bitset_get(db->covered_base_bits, id)) {
                seen[id >> 3u] |= (unsigned char)(1u << (id & 7u));
            }
        }
    }
    for (uint32_t i = 0; i < db->delta_count; ++i) {
        EzdbDeltaRecord* delta = &db->deltas[i];
        if (ezdb_find_delta_record(db, delta->id) != delta) continue;
        if (delta->type == EZDB_DELTA_DELETE || !ezdb_bitset_get(db->active_bits, delta->id)) continue;
        if (ezdb_query_contains_ascii_casefold(delta->path, delta->path_len, keyword, key_len)) {
            seen[delta->id >> 3u] |= (unsigned char)(1u << (delta->id & 7u));
        }
    }

    uint32_t emitted = 0;
    for (uint32_t id = 0; id < db->header.file_count; ++id) {
        if (limit && emitted >= limit) break;
        if (!(seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) continue;
        if (key_len > EZDB_GRAM3 && !record_contains_keyword(db, id, keyword, key_len)) continue;
        EzdbSearchResult result;
        rc = ezdb_build_result_path(db, id, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        callback(&result, user_data);
        ++emitted;
        ezdb_free_result(&result);
    }
    free(seen);
    free(file_ids);
    free(dir_ids);
    return rc;
}

int ezdb_search_path(Ezdb* db, const char* keyword, uint32_t limit, EzdbSearchCallback callback, void* user_data)
{
    if (!db || !keyword || !callback) return EZDB_ERR_ARG;
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;
    if (!*keyword) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    if (!root) return ezdb_search_plain(db, keyword, limit, callback, user_data);

    unsigned char* seen = NULL;
    int has_positive_candidates = 0;
    int rc = query_build_candidate_bitset(db, root, &seen, &has_positive_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t seen_size = ((size_t)db->header.file_count + 7u) / 8u;
    int full_scan = !has_positive_candidates;
    if (!full_scan && !ezdb_bitset_any(seen, seen_size)) {
        free(seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }

    uint32_t emitted = 0;
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.file_count; ++id) {
        if (limit && emitted >= limit) break;
        if (full_scan) {
            if (!ezdb_bitset_get(db->active_bits, id)) continue;
        } else if (!(seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        EzdbSearchResult result;
        rc = ezdb_build_result_path(db, id, &result);
        if (rc == EZDB_ERR_NOT_FOUND) {
            rc = EZDB_OK;
            continue;
        }
        if (rc != EZDB_OK) break;
        if (ezdb_query_match_path(root, result.path, strlen(result.path))) {
            callback(&result, user_data);
            ++emitted;
        }
        ezdb_free_result(&result);
    }

    free(seen);
    ezdb_query_node_free(root);
    return rc;
}

static void ezdb_archive_search_adapter_cb(const EzdbSearchResult* result, void* user_data)
{
    EzdbArchiveSearchAdapter* adapter = (EzdbArchiveSearchAdapter*)user_data;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ARCHIVE;
    out.id = result->id;
    out.archive_id = result->id;
    out.archive_path = ezdb_strdup_range(result->path, strlen(result->path));
    out.file_size = result->size;
    out.modified_time = result->modified_time;
    if (adapter->db && result->id < adapter->db->header.base_file_count && adapter->db->archive_meta) {
        EzdbDiskArchiveMeta* meta = &adapter->db->archive_meta[result->id];
        out.drive_letter = (char)meta->drive_letter;
        out.file_ref_number = meta->file_ref_number;
        out.usn = meta->usn;
    }
    adapter->callback(&out, adapter->user_data);
    adapter->emitted += 1u;
    ezdb_free_search_v2_result(&out);
}

static int ezdb_emit_entry_result(Ezdb* db, uint32_t id, EzdbSearchV2Callback callback, void* user_data)
{
    EzdbEntryResult entry;
    int rc = ezdb_get_entry(db, id, &entry);
    if (rc != EZDB_OK) return rc;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ENTRY;
    out.id = id;
    out.archive_id = entry.archive_id;
    out.archive_path = entry.archive_path;
    out.entry_path = entry.entry_path;
    out.entry_raw_path = entry.entry_raw_path;
    out.entry_raw_path_len = entry.entry_raw_path_len;
    out.compressed_size = entry.compressed_size;
    out.original_size = entry.original_size;
    out.modified_time = entry.modified_time;
    entry.archive_path = NULL;
    entry.entry_path = NULL;
    entry.entry_raw_path = NULL;
    callback(&out, user_data);
    ezdb_free_search_v2_result(&out);
    memset(&entry, 0, sizeof(entry));
    return EZDB_OK;
}

static int ezdb_emit_entry_result_with_path(Ezdb* db, uint32_t id, char* entry_path, EzdbSearchV2Callback callback, void* user_data)
{
    if (!entry_path) return ezdb_emit_entry_result(db, id, callback, user_data);
    EzdbDiskEntry detail;
    EzdbEntryDetailStore store = ezdb_entry_detail_store(db);
    int rc = ezdb_entries_load_detail(&store, id, &detail);
    if (rc != EZDB_OK) return rc;
    EzdbSearchResult archive;
    rc = ezdb_build_result_path(db, detail.archive_id, &archive);
    if (rc != EZDB_OK) return rc;
    EzdbSearchV2Result out;
    memset(&out, 0, sizeof(out));
    out.kind = EZDB_RESULT_ENTRY;
    out.id = id;
    out.archive_id = detail.archive_id;
    out.archive_path = archive.path;
    out.entry_path = entry_path;
    out.compressed_size = detail.compressed_size;
    out.original_size = detail.original_size;
    out.modified_time = detail.modified_time;
    if (detail.raw_len) {
        EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
        out.entry_raw_path = ezdb_entries_copy_raw_path(&path_store, id, &detail);
        if (!out.entry_raw_path) {
            archive.path = NULL;
            ezdb_free_search_v2_result(&out);
            return EZDB_ERR_MEMORY;
        }
        out.entry_raw_path_len = detail.raw_len;
    }
    archive.path = NULL;
    callback(&out, user_data);
    ezdb_free_search_v2_result(&out);
    return EZDB_OK;
}

int ezdb_search(Ezdb* db, const char* keyword, uint32_t scope, uint32_t limit, EzdbSearchV2Callback callback, void* user_data)
{
    if (!db || !keyword || !callback) return EZDB_ERR_ARG;
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;
    if (!*keyword) return EZDB_OK;
    if (!scope) scope = EZDB_SEARCH_ARCHIVE_PATH;

    uint32_t emitted = 0;
    int rc = EZDB_OK;
    if (scope & EZDB_SEARCH_ARCHIVE_PATH) {
        EzdbArchiveSearchAdapter adapter;
        adapter.db = db;
        adapter.callback = callback;
        adapter.user_data = user_data;
        adapter.emitted = 0;
        rc = ezdb_search_path(db, keyword, limit, ezdb_archive_search_adapter_cb, &adapter);
        if (rc != EZDB_OK) return rc;
        emitted = adapter.emitted;
        if (limit && emitted >= limit) {
            return EZDB_OK;
        }
    }

    if (!(scope & (EZDB_SEARCH_ENTRY_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    unsigned char* entry_seen = NULL;
    int has_entry_candidates = 0;
    rc = query_build_entry_candidate_bitset(db, root, keyword, scope, &entry_seen, &has_entry_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t entry_seen_size = ((size_t)db->header.entry_count + 7u) / 8u;
    int full_entry_scan = !has_entry_candidates;
    if (!full_entry_scan && !ezdb_bitset_any(entry_seen, entry_seen_size)) {
        free(entry_seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }
    EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.entry_count; ++id) {
        if (limit && emitted >= limit) break;
        if (full_entry_scan) {
            if (!ezdb_bitset_get(db->active_entry_bits, id)) continue;
        } else if (!(entry_seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        uint32_t archive_id = db->entry_archive_ids[id];
        if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) continue;
        char* entry_path = ezdb_entries_copy_path(&path_store, id);
        if (!entry_path) continue;
        uint32_t entry_path_len = db->entry_path_lens[id];
        int matched = 0;
        if (scope & EZDB_SEARCH_ENTRY_PATH) {
            matched = ezdb_query_matches_text(root, keyword, entry_path, entry_path_len);
        }
        if (!matched && (scope & EZDB_SEARCH_COMBINED_PATH)) {
            EzdbSearchResult archive;
            rc = ezdb_build_result_path(db, archive_id, &archive);
            if (rc == EZDB_ERR_NOT_FOUND) {
                rc = EZDB_OK;
                free(entry_path);
                continue;
            }
            if (rc != EZDB_OK) {
                free(entry_path);
                break;
            }
            size_t archive_len = strlen(archive.path);
            size_t combo_len = archive_len + 1u + entry_path_len;
            char* combo = (char*)malloc(combo_len + 1u);
            if (!combo) {
                ezdb_free_result(&archive);
                free(entry_path);
                rc = EZDB_ERR_MEMORY;
                break;
            }
            memcpy(combo, archive.path, archive_len);
            combo[archive_len] = '\n';
            memcpy(combo + archive_len + 1u, entry_path, entry_path_len);
            combo[combo_len] = '\0';
            matched = ezdb_query_matches_text(root, keyword, combo, combo_len);
            free(combo);
            ezdb_free_result(&archive);
        }
        if (matched) {
            rc = ezdb_emit_entry_result_with_path(db, id, entry_path, callback, user_data);
            if (rc == EZDB_OK) {
                entry_path = NULL;
                ++emitted;
            }
        }
        free(entry_path);
    }
    free(entry_seen);
    ezdb_query_node_free(root);
    return rc;
}
static int ezdb_id_vec_push(EzdbIdVec* vec, uint32_t id)
{
    if (vec->count == vec->cap) {
        uint32_t next = vec->cap ? vec->cap * 2u : 1024u;
        uint32_t* ids = (uint32_t*)realloc(vec->ids, sizeof(uint32_t) * (size_t)next);
        if (!ids) return EZDB_ERR_MEMORY;
        vec->ids = ids;
        vec->cap = next;
    }
    vec->ids[vec->count++] = id;
    return EZDB_OK;
}

static int ezdb_entry_matches_query_scope(Ezdb* db,
                                          EzdbQueryNode* root,
                                          const char* keyword,
                                          uint32_t scope,
                                          uint32_t archive_id,
                                          const char* entry_path,
                                          uint32_t entry_path_len,
                                          int* out_matched)
{
    *out_matched = 0;
    if (scope & EZDB_SEARCH_ENTRY_PATH) {
        if (ezdb_query_matches_text(root, keyword, entry_path, entry_path_len)) {
            *out_matched = 1;
            return EZDB_OK;
        }
    }
    if (!(scope & (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbSearchResult archive;
    int rc = ezdb_build_result_path(db, archive_id, &archive);
    if (rc == EZDB_ERR_NOT_FOUND) return EZDB_OK;
    if (rc != EZDB_OK) return rc;

    if (scope & EZDB_SEARCH_ARCHIVE_PATH) {
        *out_matched = ezdb_query_matches_text(root, keyword, archive.path, strlen(archive.path));
    }
    if (!*out_matched && (scope & EZDB_SEARCH_COMBINED_PATH)) {
        size_t archive_len = strlen(archive.path);
        size_t combo_len = archive_len + 1u + entry_path_len;
        char* combo = (char*)malloc(combo_len + 1u);
        if (!combo) {
            ezdb_free_result(&archive);
            return EZDB_ERR_MEMORY;
        }
        memcpy(combo, archive.path, archive_len);
        combo[archive_len] = '\n';
        memcpy(combo + archive_len + 1u, entry_path, entry_path_len);
        combo[combo_len] = '\0';
        *out_matched = ezdb_query_matches_text(root, keyword, combo, combo_len);
        free(combo);
    }
    ezdb_free_result(&archive);
    return EZDB_OK;
}

static int ezdb_collect_matching_entry_ids(Ezdb* db, const char* keyword, uint32_t scope, EzdbIdVec* vec)
{
    if (!(scope & (EZDB_SEARCH_ARCHIVE_PATH | EZDB_SEARCH_ENTRY_PATH | EZDB_SEARCH_COMBINED_PATH))) return EZDB_OK;

    EzdbQueryNode* root = ezdb_query_parse(keyword);
    unsigned char* entry_seen = NULL;
    int has_entry_candidates = 0;
    uint32_t candidate_scope = scope;
    if (scope & EZDB_SEARCH_ARCHIVE_PATH) candidate_scope |= EZDB_SEARCH_COMBINED_PATH;
    int rc = query_build_entry_candidate_bitset(db, root, keyword, candidate_scope, &entry_seen, &has_entry_candidates);
    if (rc != EZDB_OK) {
        ezdb_query_node_free(root);
        return rc;
    }
    size_t entry_seen_size = ((size_t)db->header.entry_count + 7u) / 8u;
    int full_entry_scan = !has_entry_candidates;
    if (!full_entry_scan && !ezdb_bitset_any(entry_seen, entry_seen_size)) {
        free(entry_seen);
        ezdb_query_node_free(root);
        return EZDB_OK;
    }

    EzdbEntryPathStore path_store = ezdb_entry_path_store(db);
    for (uint32_t id = 0; rc == EZDB_OK && id < db->header.entry_count; ++id) {
        if (full_entry_scan) {
            if (!ezdb_bitset_get(db->active_entry_bits, id)) continue;
        } else if (!(entry_seen[id >> 3u] & (unsigned char)(1u << (id & 7u)))) {
            continue;
        }
        uint32_t archive_id = db->entry_archive_ids[id];
        if (archive_id >= db->header.file_count || !ezdb_bitset_get(db->active_bits, archive_id)) continue;
        char* entry_path = ezdb_entries_copy_path(&path_store, id);
        if (!entry_path) continue;
        int matched = 0;
        rc = ezdb_entry_matches_query_scope(db,
                                            root,
                                            keyword,
                                            scope,
                                            archive_id,
                                            entry_path,
                                            db->entry_path_lens[id],
                                            &matched);
        free(entry_path);
        if (rc == EZDB_OK && matched) rc = ezdb_id_vec_push(vec, id);
    }
    free(entry_seen);
    ezdb_query_node_free(root);
    return rc;
}

static const char* ezdb_basename(const char* path)
{
    const char* name = path;
    for (const char* p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') name = p + 1;
    }
    return name ? name : "";
}

static char* ezdb_entry_sort_string(Ezdb* db, uint32_t id, int sort_column)
{
    EzdbEntryResult entry;
    if (ezdb_get_entry(db, id, &entry) != EZDB_OK) return ezdb_strdup_range("", 0);
    const char* src = "";
    if (sort_column == 0) {
        src = ezdb_basename(entry.entry_path ? entry.entry_path : "");
    } else if (sort_column == 1) {
        src = entry.archive_path ? entry.archive_path : "";
    } else if (sort_column == 2) {
        src = entry.entry_path ? entry.entry_path : "";
    }
    char* out = ezdb_strdup_range(src, strlen(src));
    ezdb_free_entry_result(&entry);
    return out;
}

static int ezdb_compare_entry_ids(Ezdb* db, uint32_t lhs, uint32_t rhs, int sort_column, int sort_ascending)
{
    int cmp = 0;
    if (sort_column == 0 || sort_column == 1 || sort_column == 2) {
        char* a = ezdb_entry_sort_string(db, lhs, sort_column);
        char* b = ezdb_entry_sort_string(db, rhs, sort_column);
        if (!a || !b) {
            free(a);
            free(b);
            cmp = 0;
        } else {
            cmp = strcmp(a, b);
            free(a);
            free(b);
        }
    } else if (sort_column == 3 || sort_column == 4 || sort_column == 5) {
        EzdbDiskEntry ad, bd;
        EzdbEntryDetailStore store = ezdb_entry_detail_store(db);
        if (ezdb_entries_load_detail(&store, lhs, &ad) != EZDB_OK ||
            ezdb_entries_load_detail(&store, rhs, &bd) != EZDB_OK) {
            cmp = 0;
        } else if (sort_column == 3) {
            int a_missing = ad.compressed_size < 0;
            int b_missing = bd.compressed_size < 0;
            if (a_missing != b_missing) cmp = a_missing ? 1 : -1;
            else if (ad.compressed_size < bd.compressed_size) cmp = -1;
            else if (ad.compressed_size > bd.compressed_size) cmp = 1;
        } else if (sort_column == 4) {
            if (ad.original_size < bd.original_size) cmp = -1;
            else if (ad.original_size > bd.original_size) cmp = 1;
        } else {
            if (ad.modified_time < bd.modified_time) cmp = -1;
            else if (ad.modified_time > bd.modified_time) cmp = 1;
        }
    } else {
        if (lhs < rhs) cmp = -1;
        else if (lhs > rhs) cmp = 1;
    }
    if (cmp == 0) {
        if (lhs < rhs) cmp = -1;
        else if (lhs > rhs) cmp = 1;
    }
    return sort_ascending ? cmp : -cmp;
}

static void ezdb_sort_entry_ids(Ezdb* db, uint32_t* ids, uint32_t count, int sort_column, int sort_ascending)
{
    if (!ids || count < 2) return;
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t value = ids[i];
        uint32_t j = i;
        while (j > 0 && ezdb_compare_entry_ids(db, ids[j - 1u], value, sort_column, sort_ascending) > 0) {
            ids[j] = ids[j - 1u];
            --j;
        }
        ids[j] = value;
    }
}

int ezdb_query_entries(Ezdb* db, const EzdbEntryQuery* query, EzdbEntryQueryPage* out_page)
{
    if (!db || !query || !out_page) return EZDB_ERR_ARG;
    memset(out_page, 0, sizeof(*out_page));

    EzdbIdVec vec;
    memset(&vec, 0, sizeof(vec));
    const char* keyword = query->keyword ? query->keyword : "";
    while (ezdb_query_is_space((unsigned char)*keyword)) ++keyword;

    int rc = EZDB_OK;
    if (!*keyword) {
        for (uint32_t i = 0; i < db->header.entry_count; ++i) {
            if (ezdb_entry_is_searchable(db, i)) {
                rc = ezdb_id_vec_push(&vec, i);
                if (rc != EZDB_OK) break;
            }
        }
    } else {
        uint32_t scope = query->scope ? query->scope : EZDB_SEARCH_COMBINED_PATH;
        rc = ezdb_collect_matching_entry_ids(db, keyword, scope, &vec);
    }
    if (rc != EZDB_OK) {
        free(vec.ids);
        return rc;
    }

    if (query->sort_column >= 0 || !query->sort_ascending) {
        ezdb_sort_entry_ids(db, vec.ids, vec.count, query->sort_column, query->sort_ascending != 0);
    }

    out_page->total_count = vec.count;
    uint32_t offset = query->offset;
    uint32_t available = offset < vec.count ? vec.count - offset : 0;
    uint32_t wanted = query->limit ? query->limit : available;
    if (wanted > available) wanted = available;
    if (wanted) {
        out_page->ids = (uint32_t*)malloc(sizeof(uint32_t) * (size_t)wanted);
        if (!out_page->ids) {
            free(vec.ids);
            memset(out_page, 0, sizeof(*out_page));
            return EZDB_ERR_MEMORY;
        }
        memcpy(out_page->ids, vec.ids + offset, sizeof(uint32_t) * (size_t)wanted);
        out_page->returned_count = wanted;
    }
    free(vec.ids);
    return EZDB_OK;
}

int ezdb_get_entries_batch(Ezdb* db, const uint32_t* ids, uint32_t count, EzdbEntryResult* out_results)
{
    if (!db || (!ids && count) || (!out_results && count)) return EZDB_ERR_ARG;
    for (uint32_t i = 0; i < count; ++i) memset(&out_results[i], 0, sizeof(out_results[i]));
    for (uint32_t i = 0; i < count; ++i) {
        int rc = ezdb_get_entry(db, ids[i], &out_results[i]);
        if (rc != EZDB_OK) {
            for (uint32_t j = 0; j < i; ++j) ezdb_free_entry_result(&out_results[j]);
            return rc;
        }
    }
    return EZDB_OK;
}

void ezdb_free_entry_query_page(EzdbEntryQueryPage* page)
{
    if (!page) return;
    free(page->ids);
    memset(page, 0, sizeof(*page));
}

