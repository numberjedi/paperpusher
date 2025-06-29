/* serializer.c */
#define G_LOG_DOMAIN "serializer"

#include "serializer.h"
#include "paper.h"

#include <gio/gio.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static GMutex cache_mutex;

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
        *out =
          g_strndup((const gchar*)(data + *offset), len); // caller owns out
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
    g_debug("Writing cache to %s\n", db->cache);
    g_mutex_lock(&cache_mutex);
    /* Build a binary buffer of cache contents */
    GByteArray* buffer = g_byte_array_new(); // freed before return
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
        g_mutex_unlock(&cache_mutex);
        return FALSE;
    }
    g_byte_array_unref(buffer);
    g_mutex_unlock(&cache_mutex);
    g_debug("Successfully wrote cache to %s\n", db->cache);
    return TRUE;
}

/**
 * Count the number of entries in the cache file.
 * Returns the number of entries on success, 0 on error.
 */
int
load_cache_count(const PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, 0);

    g_mutex_lock(&cache_mutex);

    g_autofree gchar* data = NULL; // freed on function return
    gsize length = 0;
    if (!g_file_get_contents(db->cache, &data, &length, error)) {
        // if cache is missing create empty cache file
        if (error && *error &&
            g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_clear_error(error);
            g_file_set_contents(db->cache, "", 0, NULL);
            g_mutex_unlock(&cache_mutex);
            return 0;
        }
        g_mutex_unlock(&cache_mutex);
        return 0;
    }
    // check if file is too small
    if (length < sizeof(uint32_t)) {
        g_mutex_unlock(&cache_mutex);
        return 0;
    }

    uint32_t count;
    memcpy(&count, data, sizeof(count)); // stack-allocated
    g_mutex_unlock(&cache_mutex);
    return (int)count;
}

bool
load_cache(PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_mutex_lock(&cache_mutex);
    g_autofree gchar* data = NULL; // freed on function return
    gsize length = 0;
    if (!g_file_get_contents(db->cache, &data, &length, error)) {
        if (error && *error &&
            g_error_matches(*error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            // g_clear_error(error);
            /* Create empty cache file */
            g_file_set_contents(db->cache, "", 0, NULL);
            g_mutex_unlock(&cache_mutex);
            return FALSE;
        }
        // g_file_get_contents() alredy set error for I/O
        g_mutex_unlock(&cache_mutex);
        return FALSE;
    }
    if (length < sizeof(uint32_t)) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Cache '%s' is too small (%zu bytes)",
                    db->cache,
                    length);
        g_mutex_unlock(&cache_mutex);
        return FALSE;
    }

    const guchar* blob = (const guchar*)data;
    gsize offset = 0;

    // walk through blob and read entries
    uint32_t count;
    memcpy(&count, blob + offset, sizeof(count));
    offset += sizeof(count);
    if (count == 0) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Count is zero, nothing read.");
        g_mutex_unlock(&cache_mutex);
        return FALSE;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (offset + sizeof(uint32_t) > length)
            break;
        uint32_t year;
        memcpy(&year, blob + offset, sizeof(year));
        offset += sizeof(year);

        gchar* title = NULL;
        if (!read_string_from_data(
              blob, length, &offset, &title)) // freed before return
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
            authors = g_new0(gchar*, authors_count); // freed before return
            for (int j = 0; j < authors_count; ++j) {
                if (!read_string_from_data(blob,
                                           length,
                                           &offset,
                                           &authors[j])) // freed before return
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
            keywords = g_new0(gchar*, keyword_count); // freed before return
            for (int j = 0; j < keyword_count; ++j) {
                if (!read_string_from_data(blob,
                                           length,
                                           &offset,
                                           &keywords[j])) // freed before return
                    break;
            }
        }

        gchar* abstract = NULL;
        read_string_from_data(
          blob, length, &offset, &abstract); // freed before return
        gchar* arxiv_id = NULL;
        read_string_from_data(
          blob, length, &offset, &arxiv_id); // freed before return
        gchar* doi = NULL;
        read_string_from_data(
          blob, length, &offset, &doi); // freed before return
        gchar* pdf_file = NULL;
        read_string_from_data(
          blob, length, &offset, &pdf_file); // freed before return

        Paper* p = create_paper(db, // hard copies values, so need free all
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
        // TODO: handle corrupted cache
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

        if (!p) {
            g_mutex_unlock(&cache_mutex);
            return FALSE;
        }

    }

    g_mutex_unlock(&cache_mutex);
    return TRUE;
}
