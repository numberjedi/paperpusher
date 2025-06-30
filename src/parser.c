/* parser.c */
#define _POSIX_C_SOURCE 200809L // for readlink()
#define G_LOG_DOMAIN "parser"

#include "parser.h"
#include "cJSON/cJSON.h"
#include "config.h"
#include "loom.h"
#include "paper.h"

#include <gio/gio.h>
#include <glib.h>
#include <limits.h>
#include <unistd.h>

/**
 * Helper: safely extract string from JSON or return NULL
 * cJSON owns the returned string
 */
static gchar*
get_str(cJSON* obj, const gchar* key)
{
    cJSON* e = cJSON_GetObjectItem(obj, key);
    return (e && e->valuestring) ? e->valuestring
                                 : NULL; // freed by cJSON_Delete()
}

/* Locate the `paperparser` binary. On failure, set error. Caller owns the
 * returned string. */
static gchar*
find_paperparser_path(GError** error)
{
    /* 1) Relative to this binary */
    gchar exe_path[PATH_MAX + 1] = { 0 };
    ssize_t len = readlink(SELF_EXE_PATH, exe_path, PATH_MAX);
    if (len >= 0) {
        exe_path[len] = '\0';
        g_autofree gchar* app_dir =
          g_path_get_dirname(exe_path); // freed before function return
        gchar* candidate = g_build_filename(
          app_dir, PAPERPARSER_REL_PATH, NULL); // owned by caller
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;
        /* No executable here, try next */
        g_free(candidate);
    }
    /* 2) Environment override */
    const gchar* envp = g_getenv(PAPERPARSER_ENVVAR); // GLib owns this
    if (envp && *envp) {
        gchar* candidate = g_strdup(envp); // owned by caller
        if (g_file_test(envp, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;
        g_free(candidate);
    }
    /* 3) PATH lookup */
    gchar* found =
      g_find_program_in_path(PAPERPARSER_EXE_NAME); // owned by caller
    if (found)
        return found;
    g_free(found);

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
run_paperparser_on_pdf(const gchar* pdf_path,
                       gchar** stdout_buf,
                       GError** error)
{
    /* Find parser executable */
    g_autofree gchar* parser_path =
      find_paperparser_path(error); // freed before function return
    if (!parser_path)
        return FALSE; // caller handles error

    /* Spawn and capture JSON output */
    g_autofree gchar* cmd = g_strdup_printf(
      "%s '%s'", parser_path, pdf_path); // freed before function return
    gint exit_status = 0;
    if (!g_spawn_command_line_sync(cmd, stdout_buf, NULL, &exit_status, error))
        return FALSE; // caller handles error

    if (exit_status != 0 || *stdout_buf == NULL) {
        if (!*error) {
            g_set_error(error,
                        G_SPAWN_ERROR,
                        G_SPAWN_ERROR_FAILED,
                        "paperparser failed (exit code %d)",
                        exit_status);
        }
        return FALSE; // caller handles error
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
            authors =
              g_realloc(authors,
                        sizeof(gchar*) *
                          (authors_count + 1)); // freed before function return
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
 * parses the JSON output, creates a Paper from it.
 * Returns a pointer to the Paper on success, NULL on error.
 * @db owns the returned Paper.
 */
static Paper*
parser_run(PaperDatabase* db, const gchar* pdf_path, GError** error)
{
    g_autofree gchar* stdout_buf = NULL;
    run_paperparser_on_pdf(
      pdf_path, &stdout_buf, error); // freed before function return
    if (!stdout_buf)
        return NULL; // caller handles error

    /* Parse JSON */
    cJSON* json =
      cJSON_Parse(stdout_buf); // freed by cJSON_Delete() before function return
    if (!json) {
        const char* errptr = cJSON_GetErrorPtr(); // owned by cJSON
        gint offset = errptr ? (gint)(errptr - stdout_buf) : -1;
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Failed to parse JSON at offset %d",
                    offset);
        g_debug("%s\n", stdout_buf);
        return NULL; // caller handles error
    }

    cJSON* spans = cJSON_GetObjectItem(json, "predicted_spans");

    Paper* p = initialize_paper(
      db, pdf_path, error); // owned by @db, freed by free_paper()

    /* Populate metadata */
    gboolean success = populate_metadata(p, spans, error);
    cJSON_Delete(json);
    if (!success || !p)
        return NULL; // caller handles error

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
parser_task_callback(gpointer callback_data,
                     gpointer worker_data,
                     gpointer result,
                     GError* error)
{
    g_debug("parser_task_callback\n");
    Paper* paper = result;
    g_debug("paper title: %s\n", paper->title);
    AsyncParserCallbackData* data = callback_data;
    AsyncParserRunData* run_data = worker_data;

    PaperDatabase* db = NULL;
    if (run_data->db)
        db = run_data->db;
    data->callback(db, paper, data->user_data, error);

    //// data->pdf_path was hard copied into p->pdf_file
    //// in initialize_paper(), so it needs to be freed here.
    g_free(run_data->pdf_path);
    g_free(run_data);
    g_free(data);
}

static gpointer
parser_task_worker(gpointer worker_data, GError** error)
{
    AsyncParserRunData* data = worker_data;

    if (!data) {
        g_set_error(
          error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Task data is NULL");
        return NULL; // caller handles error
    }
    if (!data->db) {
        g_set_error(
          error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Database is NULL");
        return NULL; // caller handles error
    }
    if (!data->pdf_path) {
        g_set_error(
          error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "PDF path is NULL");
        return NULL; // caller handles error
    }

    Paper* p = parser_run(data->db, data->pdf_path, error);
    return p; // caller handles error
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
    AsyncParserRunData* worker_data =
      g_new0(AsyncParserRunData, 1); // freed by callback
    worker_data->pdf_path =
      pdf_path; // freed by callback (worker_data is not returned to *callback)
    worker_data->db = db;
    AsyncParserCallbackData* callback_data =
      g_new0(AsyncParserCallbackData, 1); // freed by callback
    callback_data->callback = callback;
    callback_data->user_data = user_data;

    Loom* loom = loom_get_default();

    LoomThreadSpec spec = loom_thread_spec_default();
    spec.tag = "parser";
    spec.shuttle = parser_task_worker;
    spec.shuttle_data = worker_data;
    spec.knot = parser_task_callback;
    spec.knot_data = callback_data;

    loom_queue_thread(loom, &spec, NULL);
}
