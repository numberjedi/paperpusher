#include "paper.h"
#include "glib.h"

static void
add_paper(PaperDatabase* db, Paper* paper)
{
    // printf("adding paper\n");
    if (db->count >= db->capacity) {
        // make sure it's not zero
        db->capacity = (db->capacity < 1) ? 1 : db->capacity * 2;
        db->papers = g_realloc(db->papers, sizeof(Paper*) * db->capacity);
    }
    db->count++;
    paper->id_in_db = db->count - 1;
    db->papers[paper->id_in_db] = paper;
}

static void
free_paper_fields(Paper* p)
{
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
}

static void
free_paper(Paper* p)
{
    if (!p)
        return;
    free_paper_fields(p);
    g_free(p);
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
    Paper* p = g_new0(Paper, 1);
    // this paper belongs to the database now
    p->title = NULL;
    p->authors = NULL;
    p->authors_count = 0;
    p->year = 0;
    p->keyword_count = 0;
    p->keywords = NULL;
    p->abstract = NULL;
    p->arxiv_id = NULL;
    p->doi = NULL;
    p->pdf_file = g_strdup(pdf_file);
    add_paper(db, p);
    return p;
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

    PaperDatabase* db = g_new0(PaperDatabase, 1);
    db->papers = g_new0(Paper*, initial_capacity);
    db->count = 0;
    db->path = db_path;
    db->cache = db_cache;
    db->capacity = initial_capacity;

    return db;
}

void
update_paper(Paper* p,
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
    if (!p) {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_INVAL,
                    "Paper is NULL");
        return;
    }
    gchar* pdf_file = g_strdup(p->pdf_file);
    free_paper_fields(p);

    p->title = title ? g_strdup(title) : NULL;

    p->authors_count = (authors_count > 0) ? authors_count : 0;
    if (p->authors_count > 0) {
        p->authors = g_new0(gchar*, p->authors_count);
        for (int i = 0; i < authors_count; i++) {
            p->authors[i] = g_strdup(authors[i]);
        }
    }

    p->year = year ? year : 0;

    p->keyword_count = keyword_count ? keyword_count : 0;
    p->keywords = NULL;
    if (p->keyword_count > 0) {
        p->keywords = g_new0(gchar*, p->keyword_count);
        for (int i = 0; i < keyword_count; i++) {
            p->keywords[i] = g_strdup(keywords[i]);
        }
    }

    p->abstract = abstract ? g_strdup(abstract) : NULL;

    p->arxiv_id = arxiv_id ? g_strdup(arxiv_id) : NULL;

    p->doi = doi ? g_strdup(doi) : NULL;

    p->pdf_file = pdf_file;
}

void
remove_paper(PaperDatabase* db, Paper* paper)
{
    if (!db || !paper) {
        return;
    }
    // move last Paper in db to the spot of the removed one
    db->papers[paper->id_in_db] = db->papers[db->count - 1];
    db->papers[paper->id_in_db]->id_in_db = paper->id_in_db;
    db->papers[db->count - 1] = NULL;
    free_paper(paper);
    db->count--;
    return;
}

void
free_database(PaperDatabase* db)
{
    if (!db)
        return;
    if (db->papers) {
        for (int i = 0; i < db->count; i++) {
            free_paper(db->papers[i]);
        }
        g_free(db->papers);
    }
    g_free(db);
}
