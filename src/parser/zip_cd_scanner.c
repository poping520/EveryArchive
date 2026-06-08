#define WIN32_LEAN_AND_MEAN

#include "zip_cd_scanner.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP64_EOCD_SIG 0x06064b50u
#define ZIP64_LOCATOR_SIG 0x07064b50u
#define ZIP_CD_FILE_SIG 0x02014b50u

static uint16_t rd16(const unsigned char* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static uint32_t rd32(const unsigned char* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static uint64_t rd64(const unsigned char* p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32u);
}

static void set_error(char* error, uint32_t error_size, const char* text)
{
    if (!error || !error_size) return;
    snprintf(error, error_size, "%s", text ? text : "zip cd scan failed");
}

static uint64_t dos_time_to_filetime(uint16_t date, uint16_t time)
{
    FILETIME local_ft;
    FILETIME utc_ft;
    ULARGE_INTEGER value;
    memset(&local_ft, 0, sizeof(local_ft));
    memset(&utc_ft, 0, sizeof(utc_ft));
    value.QuadPart = 0;
    if (!DosDateTimeToFileTime(date, time, &local_ft)) return 0;
    if (!LocalFileTimeToFileTime(&local_ft, &utc_ft)) return 0;
    value.LowPart = utc_ft.dwLowDateTime;
    value.HighPart = utc_ft.dwHighDateTime;
    return value.QuadPart;
}

static int read_exact_at(FILE* fp, uint64_t offset, void* data, size_t size)
{
    if (_fseeki64(fp, (__int64)offset, SEEK_SET) != 0) return 0;
    return !size || fread(data, 1, size, fp) == size;
}

static int parse_zip64_extra(const unsigned char* extra, uint32_t extra_len,
                             int need_original, int need_compressed,
                             uint64_t* original_size, int64_t* compressed_size)
{
    uint32_t pos = 0;
    while (pos + 4u <= extra_len) {
        uint16_t tag = rd16(extra + pos);
        uint16_t size = rd16(extra + pos + 2u);
        pos += 4u;
        if (pos + size > extra_len) return 0;
        if (tag == 0x0001u) {
            uint32_t p = pos;
            if (need_original) {
                if (p + 8u > pos + size) return 0;
                *original_size = rd64(extra + p);
                p += 8u;
            }
            if (need_compressed) {
                if (p + 8u > pos + size) return 0;
                uint64_t value = rd64(extra + p);
                if (value > INT64_MAX) return 0;
                *compressed_size = (int64_t)value;
            }
            return 1;
        }
        pos += size;
    }
    return !(need_original || need_compressed);
}

int zip_cd_scan_entries(const wchar_t* path, ZipCdEntryCallback callback, void* user_data, char* error, uint32_t error_size)
{
    if (!path || !callback) {
        set_error(error, error_size, "invalid argument");
        return 0;
    }

    FILE* fp = _wfopen(path, L"rb");
    if (!fp) {
        set_error(error, error_size, "open failed");
        return 0;
    }
    if (_fseeki64(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        set_error(error, error_size, "seek end failed");
        return 0;
    }
    __int64 file_size_i = _ftelli64(fp);
    if (file_size_i < 0) {
        fclose(fp);
        set_error(error, error_size, "tell failed");
        return 0;
    }
    uint64_t file_size = (uint64_t)file_size_i;
    uint64_t tail_size = file_size < (uint64_t)(22u + 0xffffu) ? file_size : (uint64_t)(22u + 0xffffu);
    unsigned char* tail = (unsigned char*)malloc((size_t)(tail_size ? tail_size : 1u));
    if (!tail) {
        fclose(fp);
        set_error(error, error_size, "out of memory");
        return 0;
    }
    if (!read_exact_at(fp, file_size - tail_size, tail, (size_t)tail_size)) {
        free(tail);
        fclose(fp);
        set_error(error, error_size, "read eocd tail failed");
        return 0;
    }

    int64_t eocd_pos = -1;
    for (int64_t i = (int64_t)tail_size - 22; i >= 0; --i) {
        if (rd32(tail + i) == ZIP_EOCD_SIG) {
            uint16_t comment_len = rd16(tail + i + 20);
            if ((uint64_t)i + 22u + comment_len == tail_size) {
                eocd_pos = i;
                break;
            }
        }
    }
    if (eocd_pos < 0) {
        free(tail);
        fclose(fp);
        set_error(error, error_size, "eocd not found");
        return 0;
    }

    const unsigned char* eocd = tail + eocd_pos;
    uint64_t eocd_abs = file_size - tail_size + (uint64_t)eocd_pos;
    uint64_t entry_count = rd16(eocd + 10);
    uint64_t cd_size = rd32(eocd + 12);
    uint64_t cd_offset = rd32(eocd + 16);
    int needs_zip64 = entry_count == 0xffffu || cd_size == 0xffffffffu || cd_offset == 0xffffffffu;

    if (needs_zip64) {
        if (eocd_abs < 20u) {
            free(tail);
            fclose(fp);
            set_error(error, error_size, "zip64 locator missing");
            return 0;
        }
        unsigned char loc[20];
        if (!read_exact_at(fp, eocd_abs - 20u, loc, sizeof(loc)) || rd32(loc) != ZIP64_LOCATOR_SIG) {
            free(tail);
            fclose(fp);
            set_error(error, error_size, "zip64 locator read failed");
            return 0;
        }
        uint64_t zip64_eocd_offset = rd64(loc + 8);
        unsigned char z64[56];
        if (!read_exact_at(fp, zip64_eocd_offset, z64, sizeof(z64)) || rd32(z64) != ZIP64_EOCD_SIG) {
            free(tail);
            fclose(fp);
            set_error(error, error_size, "zip64 eocd read failed");
            return 0;
        }
        entry_count = rd64(z64 + 32);
        cd_size = rd64(z64 + 40);
        cd_offset = rd64(z64 + 48);
    }
    free(tail);

    if (cd_offset > file_size || cd_size > file_size || cd_offset + cd_size > file_size) {
        fclose(fp);
        set_error(error, error_size, "central directory range invalid");
        return 0;
    }
    uint64_t pos = cd_offset;
    for (uint64_t i = 0; i < entry_count; ++i) {
        unsigned char h[46];
        if (!read_exact_at(fp, pos, h, sizeof(h)) || rd32(h) != ZIP_CD_FILE_SIG) {
            fclose(fp);
            set_error(error, error_size, "central directory header invalid");
            return 0;
        }
        uint16_t flags = rd16(h + 8);
        uint16_t mod_time = rd16(h + 12);
        uint16_t mod_date = rd16(h + 14);
        uint32_t comp32 = rd32(h + 20);
        uint32_t uncomp32 = rd32(h + 24);
        uint16_t name_len = rd16(h + 28);
        uint16_t extra_len = rd16(h + 30);
        uint16_t comment_len = rd16(h + 32);
        uint64_t rec_size = 46u + (uint64_t)name_len + extra_len + comment_len;
        if (pos + rec_size > cd_offset + cd_size) {
            fclose(fp);
            set_error(error, error_size, "central directory entry range invalid");
            return 0;
        }
        unsigned char* name_extra = (unsigned char*)malloc((size_t)name_len + extra_len + 1u);
        if (!name_extra) {
            fclose(fp);
            set_error(error, error_size, "out of memory");
            return 0;
        }
        if (!read_exact_at(fp, pos + 46u, name_extra, (size_t)name_len + extra_len)) {
            free(name_extra);
            fclose(fp);
            set_error(error, error_size, "central directory name read failed");
            return 0;
        }
        name_extra[name_len] = 0;
        uint64_t original = uncomp32;
        int64_t compressed = (int64_t)comp32;
        int need_original = uncomp32 == 0xffffffffu;
        int need_compressed = comp32 == 0xffffffffu;
        if ((need_original || need_compressed) &&
            !parse_zip64_extra(name_extra + name_len, extra_len, need_original, need_compressed, &original, &compressed)) {
            free(name_extra);
            fclose(fp);
            set_error(error, error_size, "zip64 entry extra invalid");
            return 0;
        }

        ZipCdEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.raw_name = (const char*)name_extra;
        entry.raw_name_len = name_len;
        entry.flags = flags;
        entry.compressed_size = compressed;
        entry.original_size = original;
        entry.modified_time = dos_time_to_filetime(mod_date, mod_time);
        int keep_going = callback(&entry, user_data);
        free(name_extra);
        if (!keep_going) {
            fclose(fp);
            set_error(error, error_size, "entry callback failed");
            return 0;
        }
        pos += rec_size;
    }

    fclose(fp);
    return 1;
}
