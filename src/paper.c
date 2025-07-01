#define G_LOG_DOMAIN "paper"

#include "paper.h"
#include "glib.h"
#include "loader.h"
#include "loom.h"
#include "serializer.h"

static void
add_paper(PaperDatabase* db, Paper* paper)
{
    g_debug("adding paper\n");
    WITH_DB_WRITE_LOCK(db, {
        if (db->count >= db->capacity) {
            // make sure it's not zero
            db->capacity = (db->capacity < 1) ? 1 : db->capacity * 2;
            db->papers = g_realloc(db->papers,
                                   sizeof(Paper*) *
                                     db->capacity); // freed by free_database()
        }
        db->count++;
        paper->id_in_db = db->count - 1;
        g_debug("adding paper id:%d, capacity:%d, count:%d\n",
                paper->id_in_db,
                db->capacity,
                db->count);
        db->papers[paper->id_in_db] = paper;
    });
}

static void
free_paper_fields(Paper* p)
{
    WITH_PAPER_LOCK(p, {
        if (!p)
            return;
        g_free(p->title);
        for (int i = 0; i < p->authors_count; i++) {
            g_free(p->authors[i]);
        }
        g_free(p->authors);
        for (int i = 0; i < p->keyword_count; i++) {
            g_free(p->keywords[i]);
        }
        g_free(p->keywords);
        g_free(p->abstract);
        g_free(p->arxiv_id);
        g_free(p->doi);
        g_free(p->pdf_file);
    });
}

static void
free_paper(Paper* p)
{
    if (!p)
        return;
    free_paper_fields(p);
    g_mutex_clear(&p->lock);
    g_free(p);
    p = NULL;
}

static gpointer
write_json_shuttle(gpointer worker_data, GError** error)
{
    PaperDatabase* db = worker_data;
    write_json(db, error);
    return NULL;
}

static void
write_json_knot(gpointer callback_data,
                gpointer worker_data,
                gpointer result,
                GError* error)
{
    (void)callback_data;
    (void)result;
    (void)worker_data;
    if (error) {
        g_warning("Error writing JSON: %s\n", error->message);
        g_clear_error(&error);
    }
}

static gpointer
write_cache_shuttle(gpointer worker_data, GError** error)
{
    PaperDatabase* db = worker_data;
    write_cache(db, error);
    return NULL;
}

static void
write_cache_knot(gpointer callback_data,
                 gpointer worker_data,
                 gpointer result,
                 GError* error)
{
    (void)callback_data;
    (void)result;
    (void)worker_data;
    if (error) {
        g_warning("Error writing cache: %s\n", error->message);
        g_clear_error(&error);
    }
}

void
sync_json_and_cache(PaperDatabase* db)
{
    Loom* loom = loom_get_default();
    LoomThreadSpec json_spec = loom_thread_spec_default();
    json_spec.tag = "write-json";
    json_spec.shuttle = write_json_shuttle;
    json_spec.shuttle_data = db;
    json_spec.knot = write_json_knot;
    json_spec.priority = 5;
    static const gchar* json_deps[] = { "parser", NULL };
    json_spec.dependencies = json_deps;

    LoomThreadSpec cache_spec = loom_thread_spec_default();
    cache_spec.tag = "write-cache";
    cache_spec.shuttle = write_cache_shuttle;
    cache_spec.shuttle_data = db;
    cache_spec.knot = write_cache_knot;
    cache_spec.priority = 5;
    static const gchar* cache_deps[] = { "write-json", "parser", NULL };
    cache_spec.dependencies = cache_deps;

    loom_queue_thread(loom, &json_spec, NULL);
    loom_queue_thread(loom, &cache_spec, NULL);
}

Paper*
initialize_paper(PaperDatabase* db, const gchar* pdf_file, GError** error)
{
    if (!db || !pdf_file) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "Database or pdf_file is NULL");
        return NULL;
    }
    Paper* paper = g_new0(Paper, 1); // freed by free_paper()
    paper->title = NULL;
    paper->authors = NULL;
    paper->authors_count = 0;
    paper->year = 0;
    paper->keyword_count = 0;
    paper->keywords = NULL;
    paper->abstract = NULL;
    paper->arxiv_id = NULL;
    paper->doi = NULL;
    paper->pdf_file = g_strdup(pdf_file);
    g_mutex_init(&paper->lock); // freed by free_paper()
    add_paper(db, paper);
    // Paper belongs to the database now
    return paper;
}

Paper*
create_paper(PaperDatabase* db,
             gchar* title,
             gchar** authors,
             gint authors_count,
             gint year,
             gchar** keywords,
             gint keyword_count,
             gchar* abstract,
             gchar* arxiv_id,
             gchar* doi,
             const gchar* pdf_file,
             GError** error)
{
    Paper* p = initialize_paper(db, pdf_file, error);

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

    return p;
}

PaperDatabase*
create_database(int initial_capacity, gchar* db_path, gchar* db_cache)
{
    if (initial_capacity < 1)
        initial_capacity = 1;

    PaperDatabase* db = g_new0(PaperDatabase, 1);  // freed by free_database()
    db->papers = g_new0(Paper*, initial_capacity); // freed by free_database()
    db->count = 0;
    db->path = g_strdup(db_path);   // freed by free_database()
    db->cache = g_strdup(db_cache); // freed by free_database()
    db->capacity = initial_capacity;
    g_rw_lock_init(&db->lock); // freed by free_database()

    return db;
}

// TODO: shold this be async?
gboolean
load_database(PaperDatabase* db,
              const gchar* json_path,
              const gchar* cache_path)
{
    GError* error = NULL;

    WITH_DB_WRITE_LOCK(db, {
        db->path = json_path ? g_strdup(json_path)
                             : db->path; // freed by free_database()
        db->cache = cache_path ? g_strdup(cache_path)
                               : db->cache; // freed by free_database()
    });

    /* Load from cache or JSON file */
    // TODO: async
    if (!cache_up_to_date(db->path, db->cache) || !load_cache(db, &error)) {
        if (error) {
            g_warning(
              "Error loading cache '%s': %s\n", cache_path, error->message);
            g_clear_error(&error);
        } else
            g_message("Cache not up to date, attempting to load from JSON.\n");
        if (!load_papers_from_json(db, &error)) {
            g_warning(
              "Error loading JSON '%s': %s\nContinuing with empty database.\n",
              json_path,
              error->message);
            g_clear_error(&error);
        }
    }

    /* sync JSON and cache */
    sync_json_and_cache(db);
    return TRUE;
}

void
update_paper(Paper* paper,
             gchar* title,
             gchar** authors,
             gint authors_count,
             gint year,
             gchar** keywords,
             gint keyword_count,
             gchar* abstract,
             gchar* arxiv_id,
             gchar* doi,
             GError** error)
{
    // TODO: handle error
    if (!paper) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "Paper is NULL");
        return;
    }

    // all freed by free_paper()
    gchar* pdf_file = g_strdup(paper->pdf_file);
    free_paper_fields(paper);

    WITH_PAPER_LOCK(paper, {
        paper->title = title ? g_strdup(title) : NULL;

        paper->authors_count = (authors_count > 0) ? authors_count : 0;
        if (paper->authors_count > 0) {
            paper->authors = g_new0(gchar*, paper->authors_count);
            for (int i = 0; i < authors_count; i++) {
                paper->authors[i] = g_strdup(authors[i]);
            }
        }

        paper->year = year ? year : 0;

        paper->keyword_count = keyword_count ? keyword_count : 0;
        paper->keywords = NULL;
        if (paper->keyword_count > 0) {
            paper->keywords = g_new0(gchar*, paper->keyword_count);
            for (int i = 0; i < keyword_count; i++) {
                paper->keywords[i] = g_strdup(keywords[i]);
            }
        }

        paper->abstract = abstract ? g_strdup(abstract) : NULL;

        paper->arxiv_id = arxiv_id ? g_strdup(arxiv_id) : NULL;

        paper->doi = doi ? g_strdup(doi) : NULL;

        paper->pdf_file = pdf_file;
    });
}

void
remove_paper(PaperDatabase* db, Paper* paper)
{
    if (!db || !paper) {
        return;
    }
    // move last Paper in db to the spot of the removed one
    WITH_DB_WRITE_LOCK(db, {
        db->papers[paper->id_in_db] = db->papers[db->count - 1];
        db->papers[paper->id_in_db]->id_in_db = paper->id_in_db;
        db->papers[db->count - 1] = NULL;
        free_paper(paper);
        db->count--;
    });
    return;
}

void
reset_database(PaperDatabase* db)
{
    if (!db)
        return;
    WITH_DB_WRITE_LOCK(db, {
        if (db->papers) {
            for (int i = 0; i < db->count; i++)
                free_paper(db->papers[i]);
            g_free(db->papers);
            db->papers = g_new0(Paper*, 1); // freed by free_database()
        }
        db->capacity = 1;
        db->count = 0;
    });
    // TODO: sync json and cache
}

void
free_database(PaperDatabase* db)
{
    if (!db)
        return;
    WITH_DB_WRITE_LOCK(db, {
        if (db->papers) {
            for (int i = 0; i < db->count; i++) {
                free_paper(db->papers[i]);
            }
            g_free(db->papers);
        }
    });
    g_free(db->path);
    g_free(db->cache);
    g_rw_lock_clear(&db->lock);
    g_free(db);
}
