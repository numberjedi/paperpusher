/* parser.c */
#define _POSIX_C_SOURCE 200809L // for readlink()

#include "parser.h"
#include "cJSON/cJSON.h"
#include "config.h"
#include "loader.h"
#include "paper.h"
#include "serializer.h"

#include <gio/gio.h>
#include <glib.h>
#include <limits.h>
#include <unistd.h>

/* Helper: safely extract string from JSON or return NULL */
static gchar*
get_str(cJSON* obj, const gchar* key)
{
    // g_return_val_if_fail(obj != NULL && key != NULL, NULL);
    cJSON* e = cJSON_GetObjectItem(obj, key);
    return (e && e->valuestring) ? e->valuestring : NULL;
}

/* Locate the `paperparser` binary. On failure, set error */
static gchar*
find_paperparser_path(GError** error)
{
    /* 1) Relative to this binary */
    gchar exe_path[PATH_MAX + 1] = { 0 };
    ssize_t len = readlink(SELF_EXE_PATH, exe_path, PATH_MAX);
    if (len >= 0) {
        exe_path[len] = '\0';
        g_autofree gchar* app_dir = g_path_get_dirname(exe_path);
        gchar* candidate =
          g_build_filename(app_dir, PAPERPARSER_REL_PATH, NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;
        /* No executable here, try next */
        g_free(candidate);
    }
    /* 2) Environment override */
    const gchar* envp = g_getenv(PAPERPARSER_ENVVAR);
    if (envp && *envp) {
        gchar* candidate = g_strdup(envp);
        if (g_file_test(envp, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;
        g_free(candidate);
    }
    /* 3) PATH lookup */
    gchar* found = g_find_program_in_path(PAPERPARSER_EXE_NAME);
    if (found)
        return found;

    /* If all attempts failed */
    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_NOENT,
                "Could not locate 'paperparser' anywhere");
    return NULL;
}

/**
 * Run the external `paperparser` executable on `pdf_path`,
 * Writes the stdout buffer to @stdout_buf.
 * Returns TRUE on success, FALSE on error.
 */
static gboolean
run_paperparser_on_pdf(const gchar* pdf_path, gchar** stdout_buf, GError** error)
{
    /* Find parser executable */
    g_autofree gchar* parser_path = find_paperparser_path(error);
    if (!parser_path)
        return FALSE;

    /* Spawn and capture JSON output */
    g_autofree gchar* cmd = g_strdup_printf("%s '%s'", parser_path, pdf_path);
    gint exit_status = 0;
    if (!g_spawn_command_line_sync(cmd, stdout_buf, NULL, &exit_status, error))
        return FALSE;

    if (exit_status != 0 || *stdout_buf == NULL) {
        if (!*error) {
            g_set_error(error,
                        G_SPAWN_ERROR,
                        G_SPAWN_ERROR_FAILED,
                        "paperparser failed (exit code %d)",
                        exit_status);
        }
        return FALSE;
    }
    return TRUE;
}

/* Populate metadata fields from JSON spans onto Paper */
// TODO: move the checking & assigning logic into paper.c,
// only do conversion from output JSON
static gboolean
populate_metadata(Paper* p, cJSON* spans, GError** error)
{
    if (!p)
        return FALSE;
    gchar* title = NULL;
    gchar** authors = NULL;
    gint authors_count = 0;
    gint year = 0;
    gchar** keywords = NULL;
    gint keyword_count = 0;
    gchar* abstract = NULL;
    gchar* arxiv_id = NULL;
    gchar* doi = NULL;

    cJSON* span = NULL;
    cJSON_ArrayForEach(span, spans)
    {
        // get freed on cJSON_Delete
        gchar* entity = get_str(span, "entity");
        gchar* text = get_str(span, "text");

        if (g_strcmp0(entity, "TITLE") == 0) {
            title = text;
        } else if (g_strcmp0(entity, "AUTHOR") == 0) {
            /* Append author */
            authors = g_realloc(authors, sizeof(gchar*) * (authors_count + 1));
            authors[authors_count++] = text;
        } else if (g_strcmp0(entity, "YEAR") == 0 && text) {
            year = atoi(text);
        } else if (g_strcmp0(entity, "ARXIV_ID") == 0) {
            arxiv_id = text;
        } else if (g_strcmp0(entity, "DOI") == 0) {
            doi = text;
        } else if (g_strcmp0(entity, "ABSTRACT") == 0) {
            abstract = text;
        }
    }
    update_paper(p,
                 title,
                 authors,
                 authors_count,
                 year,
                 keywords,
                 keyword_count,
                 abstract,
                 arxiv_id,
                 doi,
                 error);

    // free the allocated pointer memory, not the strings they point to
    // (those belong to the cJSON object)
    g_free(authors);

    return TRUE;
}

/**
 * Runs the external `paperparser` executable on @pdf_path,
 * parses the JSON output, creates a Paper from it and transfers
 * ownership of the Paper to @db.
 * Returns a pointer to the Paper on success, NULL on error.
 */
Paper*
parser_run(PaperDatabase* db, const gchar* pdf_path, GError** error)
{
    g_autofree gchar* stdout_buf = NULL;
    run_paperparser_on_pdf(pdf_path, &stdout_buf, error);
    if (!stdout_buf)
        return NULL;

    /* Parse JSON */
    cJSON* json = cJSON_Parse(stdout_buf);
    if (!json) {
        const char* errptr = cJSON_GetErrorPtr();
        gint offset = errptr ? (gint)(errptr - stdout_buf) : -1;
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Failed to parse JSON at offset %d",
                    offset);
        return NULL;
    }

    cJSON* spans = cJSON_GetObjectItem(json, "predicted_spans");
    Paper* p = initialize_paper(db, pdf_path, error);

    /* Populate metadata */
    gboolean success = populate_metadata(p, spans, error);
    cJSON_Delete(json);
    if (!success || !p)
        return NULL;

    /* Update database */
    write_json_async(db);
    write_cache(db, error);

    return p;
}

typedef struct
{
    gchar* pdf_path;
    PaperDatabase* db;
} AsyncParserRunData;

typedef struct
{
    gpointer user_data;
    void (*callback)(PaperDatabase*, Paper*, gpointer, GError*);
} AsyncParserCallbackData;

static void
parser_task_callback(GObject* source_object,
                     GAsyncResult* result,
                     gpointer user_data)
{
    (void)source_object;
    GTask* task = G_TASK(result);
    AsyncParserCallbackData* callback_data = user_data;
    AsyncParserRunData* worker_data = g_task_get_task_data(task);

    GError* error = NULL;
    Paper* p = g_task_propagate_pointer(task, &error);
    PaperDatabase* db = NULL;
    if (worker_data->db)
        db = worker_data->db;
    callback_data->callback(db, p, callback_data->user_data, error);

    // data->pdf_path was hard copied into p->pdf_file
    // in initialize_paper(), so it needs to be freed here.
    g_free(worker_data->pdf_path);
    g_free(worker_data);
    g_free(callback_data);
}

static void
parser_task_worker(GTask* task,
                   gpointer source_object,
                   gpointer task_data,
                   GCancellable* cancellable)
{
    (void)source_object;
    (void)cancellable;
    (void)task_data;
    AsyncParserRunData* data = g_task_get_task_data(task);

    GError* error = NULL;
    if (!data) {
        g_set_error(
          &error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Task data is NULL");
        g_task_return_error(task, error);
        return;
    }
    if (!data->db) {
        g_set_error(
          &error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Database is NULL");
        g_task_return_error(task, error);
        return;
    }
    if (!data->pdf_path) {
        g_set_error(
          &error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "PDF path is NULL");
        g_task_return_error(task, error);
        return;
    }
    g_message("Parsing '%s'...", data->pdf_path);
    Paper* p = parser_run(data->db, data->pdf_path, &error);
    if (error) {
        g_task_return_error(task, error);
    }
    g_task_return_pointer(task, p, NULL);
    // GTask will free on its own, no need to g_object_unref(task)
}

/**
 * Takes ownership of pdf_path.
 */
void
async_parser_run(PaperDatabase* db,
                 gchar* pdf_path,
                 void (*callback)(PaperDatabase*, Paper*, gpointer, GError*),
                 gpointer user_data)
{
    AsyncParserRunData* worker_data = g_new0(AsyncParserRunData, 1);
    worker_data->pdf_path = pdf_path;
    worker_data->db = db;
    AsyncParserCallbackData* callback_data = g_new0(AsyncParserCallbackData, 1);
    callback_data->callback = callback;
    callback_data->user_data = user_data;

    GTask* task = g_task_new(NULL, NULL, parser_task_callback, callback_data);
    g_task_set_task_data(task, worker_data, NULL);
    g_task_run_in_thread(task, parser_task_worker);
}
