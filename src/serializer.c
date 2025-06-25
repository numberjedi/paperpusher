/* serializer.c */
#include "serializer.h"
#include "paper.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Helper: append a length-prefixed string to a GByteArray */
static void
append_string_to_buffer(GByteArray* buffer, const char* s)
{
    uint32_t len = s ? (uint32_t)strlen(s) : 0;
    g_byte_array_append(buffer, (const guint8*)&len, sizeof(len));
    if (len)
        g_byte_array_append(buffer, (const guint8*)s, len);
}

/* Helper: read a length-prefixed string from data blob */
static gboolean
read_string_from_data(const guchar* data,
                      gsize length,
                      gsize* offset,
                      gchar** out)
{
    if (*offset + sizeof(uint32_t) > length)
        return FALSE;
    uint32_t len;
    memcpy(&len, data + *offset, sizeof(len));
    *offset += sizeof(len);
    if (*offset + len > length)
        return FALSE;
    if (len > 0) {
        *out = g_strndup((const gchar*)(data + *offset), len);
    } else {
        *out = NULL;
    }
    *offset += len;
    return TRUE;
}

bool
cache_up_to_date(const char* json_path, const char* cache_path)
{
    struct stat js, cs;
    return (stat(cache_path, &cs) == 0 && stat(json_path, &js) == 0 &&
            cs.st_mtime >= js.st_mtime);
}

bool
write_cache(const PaperDatabase* db, GError** error)
{
    /* Build a binary buffer of cache contents */
    GByteArray* buffer = g_byte_array_new();
/* Helper to append raw data */
#define APPEND(data) g_byte_array_append(buffer, (guint8*)&(data), sizeof(data))

    /* Number of entries */
    uint32_t count = (uint32_t)db->count;
    APPEND(count);

    for (int i = 0; i < db->count; ++i) {
        const Paper* p = db->papers[i];
        /* Year */
        uint32_t year = (uint32_t)p->year;
        APPEND(year);

        append_string_to_buffer(buffer, p->title);

        /* Authors array */
        uint32_t ac = (uint32_t)p->authors_count;
        APPEND(ac);
        for (int j = 0; j < p->authors_count; ++j)
            append_string_to_buffer(buffer, p->authors[j]);

        /* Keywords array */
        uint32_t kc = (uint32_t)p->keyword_count;
        APPEND(kc);
        for (int j = 0; j < p->keyword_count; ++j)
            append_string_to_buffer(buffer, p->keywords[j]);

        /* Other fields */
        append_string_to_buffer(buffer, p->abstract);
        append_string_to_buffer(buffer, p->arxiv_id);
        append_string_to_buffer(buffer, p->doi);
        append_string_to_buffer(buffer, p->pdf_file);
    }
#undef APPEND

    /* Write buffer atomically to cache file */
    if (!g_file_set_contents(
          db->cache, (const char*)buffer->data, buffer->len, error)) {
        g_byte_array_unref(buffer);
        return FALSE;
    }
    g_byte_array_unref(buffer);
    return TRUE;
}

int
load_cache_count(const PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, 0);

    g_autofree gchar* data = NULL;
    gsize length = 0;
    if (!g_file_get_contents(db->cache, &data, &length, error)) {
        // if cache is missing create empty cache file
        if (error && *error &&
            g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(error);
            g_file_set_contents(db->cache, "", 0, NULL);
            return 0;
        }
        return 0;
    }
    if (length < sizeof(uint32_t))
        return 0;

    uint32_t count;
    memcpy(&count, data, sizeof(count));
    return (int)count;
}

bool
load_cache(PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_autofree gchar* data = NULL;
    gsize length = 0;
    if (!g_file_get_contents(db->cache, &data, &length, error)) {
        if (error && *error &&
            g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            // g_clear_error(error);
            /* Create empty cache file */
            g_file_set_contents(db->cache, "", 0, NULL);
            return FALSE;
        }
        // g_file_get_contents() alredy set error for I/O
        return FALSE;
    }
    if (length < sizeof(uint32_t)) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Cache '%s' is too small (%zu bytes)",
                    db->cache,
                    length);
        return FALSE;
    }

    const guchar* blob = (const guchar*)data;
    gsize offset = 0;

    uint32_t count;
    memcpy(&count, blob + offset, sizeof(count));
    offset += sizeof(count);
    if (count == 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Count is zero, nothing read.");
        return FALSE;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(uint32_t) > length)
            break;
        uint32_t year;
        memcpy(&year, blob + offset, sizeof(year));
        offset += sizeof(year);

        gchar* title = NULL;
        if (!read_string_from_data(blob, length, &offset, &title))
            break;

        /* Authors */
        if (offset + sizeof(uint32_t) > length)
            break;
        uint32_t ac;
        memcpy(&ac, blob + offset, sizeof(ac));
        offset += sizeof(ac);
        int authors_count = (int)ac;
        gchar** authors = NULL;
        if (authors_count > 0) {
            authors = g_new0(gchar*, authors_count);
            for (int j = 0; j < authors_count; ++j) {
                if (!read_string_from_data(blob, length, &offset, &authors[j]))
                    break;
            }
        }

        /* Keywords */
        if (offset + sizeof(uint32_t) > length)
            break;
        uint32_t kc;
        memcpy(&kc, blob + offset, sizeof(kc));
        offset += sizeof(kc);
        int keyword_count = (int)kc;
        gchar** keywords = NULL;
        if (keyword_count > 0) {
            keywords = g_new0(gchar*, keyword_count);
            for (int j = 0; j < keyword_count; ++j) {
                if (!read_string_from_data(blob, length, &offset, &keywords[j]))
                    break;
            }
        }

        gchar* abstract = NULL;
        read_string_from_data(blob, length, &offset, &abstract);
        gchar* arxiv_id = NULL;
        read_string_from_data(blob, length, &offset, &arxiv_id);
        gchar* doi = NULL;
        read_string_from_data(blob, length, &offset, &doi);
        gchar* pdf_file = NULL;
        read_string_from_data(blob, length, &offset, &pdf_file);

        Paper* p = create_paper(db,
                                title,
                                authors,
                                authors_count,
                                (int)year,
                                keywords,
                                keyword_count,
                                abstract,
                                arxiv_id,
                                doi,
                                pdf_file,
                                error);
        if (!p)
            return FALSE;

        /* Cleanup locals */
        g_free(title);
        if (authors) {
            for (int j = 0; j < authors_count; ++j)
                g_free(authors[j]);
            g_free(authors);
        }
        if (keywords) {
            for (int j = 0; j < keyword_count; ++j)
                g_free(keywords[j]);
            g_free(keywords);
        }
        g_free(abstract);
        g_free(arxiv_id);
        g_free(doi);
        g_free(pdf_file);
    }

    return TRUE;
}
