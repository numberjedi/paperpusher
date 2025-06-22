/* loader.c */
#include "loader.h"
#include "cJSON/cJSON.h"
#include "paper.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

/**
 * Read entire file at db->path into memory and parse JSON.
 * Populate db with the resulting Paper objects.
 */
bool
load_papers_from_json(PaperDatabase *db, GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_autofree gchar *data = NULL;
    gsize length = 0;
    /* Read entire file into memory */
    if (!g_file_get_contents(db->path, &data, &length, error))
        return FALSE;

    if (length == 0)
        return TRUE;

    cJSON *json = cJSON_Parse(data);
    if (!json) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Failed to parse JSON: %s",
                    cJSON_GetErrorPtr());
        return FALSE;
    }

    int count = cJSON_GetArraySize(json);
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(json, i);

        const gchar *title =
            cJSON_IsString(cJSON_GetObjectItem(item, "title")) ?
            cJSON_GetObjectItem(item, "title")->valuestring : NULL;

        /* Authors array */
        cJSON *arr = cJSON_GetObjectItem(item, "authors");
        int authors_count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        gchar **authors = NULL;
        if (authors_count > 0) {
            authors = g_new0(gchar*, authors_count);
            for (int j = 0; j < authors_count; ++j) {
                cJSON *a = cJSON_GetArrayItem(arr, j);
                authors[j] = a && a->valuestring
                             ? g_strdup(a->valuestring)
                             : g_strdup("");
            }
        }

        int year = cJSON_IsNumber(cJSON_GetObjectItem(item, "year")) ?
                   cJSON_GetObjectItem(item, "year")->valueint : 0;

        /* Keywords array */
        arr = cJSON_GetObjectItem(item, "keywords");
        int kw_count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        gchar **keywords = NULL;
        if (kw_count > 0) {
            keywords = g_new0(gchar*, kw_count);
            for (int j = 0; j < kw_count; ++j) {
                cJSON *k = cJSON_GetArrayItem(arr, j);
                keywords[j] = k && k->valuestring
                              ? g_strdup(k->valuestring)
                              : g_strdup("");
            }
        }

        const gchar *abstract =
            cJSON_IsString(cJSON_GetObjectItem(item, "abstract")) ?
            cJSON_GetObjectItem(item, "abstract")->valuestring : NULL;
        const gchar *arxiv_id =
            cJSON_IsString(cJSON_GetObjectItem(item, "arxiv_id")) ?
            cJSON_GetObjectItem(item, "arxiv_id")->valuestring : NULL;
        const gchar *doi =
            cJSON_IsString(cJSON_GetObjectItem(item, "doi")) ?
            cJSON_GetObjectItem(item, "doi")->valuestring : NULL;
        const gchar *pdf_file =
            cJSON_IsString(cJSON_GetObjectItem(item, "pdf_file")) ?
            cJSON_GetObjectItem(item, "pdf_file")->valuestring : NULL;

        Paper *p = create_paper(title,
                                authors,
                                authors_count,
                                year,
                                keywords,
                                kw_count,
                                abstract,
                                arxiv_id,
                                doi,
                                pdf_file);
        add_paper(db, p);

        /* Cleanup temporary arrays */
        if (authors) {
            for (int j = 0; j < authors_count; ++j)
                g_free(authors[j]);
            g_free(authors);
        }
        if (keywords) {
            for (int j = 0; j < kw_count; ++j)
                g_free(keywords[j]);
            g_free(keywords);
        }
    }

    cJSON_Delete(json);
    return TRUE;
}

/**
 * Synchronously write the database out as JSON to db->path.
 */
bool
write_json(const PaperDatabase *db, GError **error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < db->count; ++i) {
        const Paper *p = db->papers[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "title", p->title);

        /* Authors */
        cJSON *auth_arr = cJSON_CreateArray();
        for (int j = 0; j < p->authors_count; ++j)
            cJSON_AddItemToArray(auth_arr,
                cJSON_CreateString(p->authors[j]));
        cJSON_AddItemToObject(obj, "authors", auth_arr);

        cJSON_AddNumberToObject(obj, "year", p->year);

        /* Keywords */
        cJSON *kw_arr = cJSON_CreateArray();
        for (int j = 0; j < p->keyword_count; ++j)
            cJSON_AddItemToArray(kw_arr,
                cJSON_CreateString(p->keywords[j]));
        cJSON_AddItemToObject(obj, "keywords", kw_arr);

        cJSON_AddStringToObject(obj, "abstract", p->abstract);
        if (p->arxiv_id)
            cJSON_AddStringToObject(obj, "arxiv_id", p->arxiv_id);
        if (p->doi)
            cJSON_AddStringToObject(obj, "doi", p->doi);
        if (p->pdf_file)
            cJSON_AddStringToObject(obj, "pdf_file", p->pdf_file);

        cJSON_AddItemToArray(root, obj);
    }

    g_autofree char *text = cJSON_PrintUnformatted(root);
    g_autofree GError *io_err = NULL;
    if (!g_file_set_contents(db->path, text, strlen(text), &io_err)) {
        g_propagate_error(error, io_err);
        cJSON_Delete(root);
        return FALSE;
    }
    cJSON_Delete(root);
    return TRUE;
}

/* Background thread: sync write_json() */
static gpointer
thread_write_json(gpointer data)
{
    write_json((const PaperDatabase*)data, NULL);
    return NULL;
}

/**
 * Launch write_json() in a background thread and detach.
 */
void
write_json_async(const PaperDatabase *db)
{
    GThread *thr = g_thread_new("write-json", thread_write_json, (gpointer)db);
    g_thread_unref(thr);
}

