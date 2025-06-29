/* loader.c */
#define G_LOG_DOMAIN "loader"

#include "loader.h"
#include "cJSON/cJSON.h"
#include "paper.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

static GMutex json_mutex;

/**
 * Read entire file at db->path into memory and parse JSON.
 * Populate db with the resulting Paper objects.
 * TODO: use async task
 */
bool
load_papers_from_json(PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_autofree gchar* data = NULL; // freed on function return
    gsize length = 0;
    /* Read entire file into memory */
    g_mutex_lock(&json_mutex);
    if (!g_file_get_contents(db->path, &data, &length, error)) {
        g_mutex_unlock(&json_mutex);
        return FALSE;
    }
    g_mutex_unlock(&json_mutex);

    if (length == 0) {
        return TRUE;
    }

    cJSON* json = cJSON_Parse(data); // freed by cJSON_Delete()
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
        cJSON* item = cJSON_GetArrayItem(json, i); // freed by cJSON_Delete()

        gchar* title = cJSON_IsString(cJSON_GetObjectItem(
                         item, "title")) // freed by cJSON_Delete()
                         ? cJSON_GetObjectItem(item, "title")->valuestring
                         : NULL;

        /* Authors array */
        cJSON* arr =
          cJSON_GetObjectItem(item, "authors"); // freed by cJSON_Delete()
        int authors_count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        g_autofree gchar** authors = NULL; //
        if (authors_count > 0) {
            authors = g_new0(
              gchar*, authors_count); // auto freed before function return
            for (int j = 0; j < authors_count; ++j) {
                cJSON* a =
                  cJSON_GetArrayItem(arr, j); // freed by cJSON_Delete()
                authors[j] =                  // freed by cJSON_Delete()
                  a && a->valuestring ? a->valuestring : "";
            }
        }

        int year = cJSON_IsNumber(cJSON_GetObjectItem(item, "year"))
                     ? cJSON_GetObjectItem(item, "year")->valueint
                     : 0;

        /* Keywords array */
        arr = cJSON_GetObjectItem(item, "keywords"); // freed by cJSON_Delete()
        int kw_count = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
        g_autofree gchar** keywords = NULL; //
        if (kw_count > 0) {
            keywords =
              g_new0(gchar*, kw_count); // auto freed before function return
            for (int j = 0; j < kw_count; ++j) {
                cJSON* k =
                  cJSON_GetArrayItem(arr, j); // freed by cJSON_Delete()
                keywords[j] =                 // freed by cJSON_Delete()
                  k && k->valuestring ? k->valuestring : "";
            }
        }

        gchar* abstract = cJSON_IsString(cJSON_GetObjectItem(item, "abstract"))
                            ? cJSON_GetObjectItem(item, "abstract")->valuestring
                            : NULL; // freed by cJSON_Delete()
        gchar* arxiv_id = cJSON_IsString(cJSON_GetObjectItem(item, "arxiv_id"))
                            ? cJSON_GetObjectItem(item, "arxiv_id")->valuestring
                            : NULL; // freed by cJSON_Delete()
        gchar* doi = cJSON_IsString(cJSON_GetObjectItem(item, "doi"))
                       ? cJSON_GetObjectItem(item, "doi")->valuestring
                       : NULL; // freed by cJSON_Delete()
        const gchar* pdf_file =
          cJSON_IsString(cJSON_GetObjectItem(item, "pdf_file"))
            ? cJSON_GetObjectItem(item, "pdf_file")->valuestring
            : NULL; // freed by cJSON_Delete()

        Paper* p = create_paper(db, // hard copies values, so need free all
                                title,
                                authors,
                                authors_count,
                                year,
                                keywords,
                                kw_count,
                                abstract,
                                arxiv_id,
                                doi,
                                pdf_file,
                                error);

        /* Cleanup temporary arrays */
        if (authors) {
            // for (int j = 0; j < authors_count; ++j)
            //     g_free(authors[j]);
            // g_free(authors);
        }
        if (keywords) {
            // for (int j = 0; j < kw_count; ++j)
            //     g_free(keywords[j]);
            // g_free(keywords);
        }
        if (!p) {
            cJSON_Delete(json);
            return FALSE;
        }
    }

    cJSON_Delete(json);
    return TRUE;
}

/**
 * Write the database out as JSON to db->path.
 */
bool
write_json(const PaperDatabase* db, GError** error)
{
    g_return_val_if_fail(db != NULL, FALSE);

    g_debug("Writing JSON to %s\n", db->path);

    g_mutex_lock(&json_mutex);
    g_rw_lock_reader_lock((GRWLock*)&db->lock);

    cJSON* root = cJSON_CreateArray(); // freed by cJSON_Delete()
    for (int i = 0; i < db->count; ++i) {
        const Paper* p = db->papers[i];
        g_mutex_lock((GMutex*)&p->lock);
        cJSON* obj = cJSON_CreateObject(); // freed by cJSON_Delete()
        cJSON_AddStringToObject(obj, "title", p->title);

        /* Authors */
        cJSON* auth_arr = cJSON_CreateArray(); // freed by cJSON_Delete()
        for (int j = 0; j < p->authors_count; ++j)
            cJSON_AddItemToArray(auth_arr, cJSON_CreateString(p->authors[j]));
        cJSON_AddItemToObject(obj, "authors", auth_arr);

        cJSON_AddNumberToObject(obj, "year", p->year);

        /* Keywords */
        cJSON* kw_arr = cJSON_CreateArray(); // freed by cJSON_Delete()
        for (int j = 0; j < p->keyword_count; ++j)
            cJSON_AddItemToArray(kw_arr, cJSON_CreateString(p->keywords[j]));
        cJSON_AddItemToObject(obj, "keywords", kw_arr);

        cJSON_AddStringToObject(obj, "abstract", p->abstract);
        if (p->arxiv_id)
            cJSON_AddStringToObject(obj, "arxiv_id", p->arxiv_id);
        if (p->doi)
            cJSON_AddStringToObject(obj, "doi", p->doi);
        if (p->pdf_file)
            cJSON_AddStringToObject(obj, "pdf_file", p->pdf_file);

        cJSON_AddItemToArray(root, obj);
        g_mutex_unlock((GMutex*)&p->lock);
    }
    g_rw_lock_reader_unlock((GRWLock*)&db->lock);

    g_autofree char* text = cJSON_PrintUnformatted(root); // freed before return
    g_autofree GError* io_err = NULL; // freed before return
    if (!g_file_set_contents(db->path, text, strlen(text), &io_err)) {
        g_propagate_error(error, io_err);
        cJSON_Delete(root);
        g_mutex_unlock(&json_mutex);
        return FALSE;
    }
    cJSON_Delete(root);
    g_mutex_unlock(&json_mutex);
    g_debug("Successfully wrote JSON to %s\n", db->path);
    return TRUE;
}
