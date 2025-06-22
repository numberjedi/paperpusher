/* parser.c */
#include "parser.h"
#include "cJSON/cJSON.h"
#include "config.h"
#include "loader.h"
#include "paper.h"
#include "serializer.h"

#include <glib.h>
#include <limits.h>
#include <unistd.h>

/* Helper: safely extract string from JSON or return NULL */
static const gchar*
get_str(cJSON* obj, const gchar* key)
{
    g_return_val_if_fail(obj != NULL && key != NULL, NULL);
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
        gchar *candidate = g_strdup(envp);
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

/* Populate metadata fields from JSON spans onto Paper */
static void
populate_metadata(Paper* p, cJSON* spans)
{
    g_return_if_fail(p != NULL && spans != NULL);
    cJSON* span = NULL;
    cJSON_ArrayForEach(span, spans)
    {
        const gchar* entity = get_str(span, "entity");
        const gchar* text = get_str(span, "text");

        if (g_strcmp0(entity, "TITLE") == 0) {
            p->title = text ? g_strdup(text) : NULL;
        } else if (g_strcmp0(entity, "AUTHOR") == 0) {
            /* Append author */
            p->authors =
              g_realloc(p->authors, sizeof(gchar*) * (p->authors_count + 1));
            p->authors[p->authors_count++] = text ? g_strdup(text) : NULL;
        } else if (g_strcmp0(entity, "YEAR") == 0 && text) {
            p->year = atoi(text);
        } else if (g_strcmp0(entity, "ARXIV_ID") == 0) {
            p->arxiv_id = text ? g_strdup(text) : NULL;
        } else if (g_strcmp0(entity, "DOI") == 0) {
            p->doi = text ? g_strdup(text) : NULL;
        } else if (g_strcmp0(entity, "ABSTRACT") == 0) {
            p->abstract = text ? g_strdup(text) : NULL;
        }
    }
}

/**
 * See parser.h for docs.
 */
Paper*
parser_run(PaperDatabase* db, const gchar* pdf_path, GError** error)
{
    g_return_val_if_fail(db != NULL, NULL);
    g_return_val_if_fail(pdf_path != NULL, NULL);

    /* Find parser executable */
    g_autofree gchar* parser_path = find_paperparser_path(error);
    if (!parser_path)
        return NULL;

    /* Spawn and capture JSON output */
    g_autofree gchar* cmd = g_strdup_printf("%s '%s'", parser_path, pdf_path);
    g_autofree gchar* stdout_buf = NULL;
    gint exit_status = 0;
    if (!g_spawn_command_line_sync(cmd, &stdout_buf, NULL, &exit_status, error))
        return NULL;

    if (exit_status != 0 || stdout_buf == NULL) {
        if (!*error) {
            g_set_error(error,
                        G_SPAWN_ERROR,
                        G_SPAWN_ERROR_FAILED,
                        "paperparser failed (exit code %d)",
                        exit_status);
        }
        return NULL;
    }

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
    Paper* p = create_paper(
      NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, pdf_path);

    /* Populate metadata */
    populate_metadata(p, spans);

    cJSON_Delete(json);

    /* Update database */
    add_paper(db, p);
    write_json_async(db);
    write_cache(db, error);

    return p;
}
