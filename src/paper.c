#include "paper.h"
#include <stdlib.h>
#include <string.h>

Paper*
create_empty_paper()
{
    return create_paper(
      NULL, NULL, 0, 0, NULL, 0, NULL, NULL, NULL, NULL);
}

Paper*
create_paper(const gchar* title,
             const gchar** authors,
             gint authors_count,
             gint year,
             const gchar** keywords,
             gint keyword_count,
             const gchar* abstract,
             const gchar* arxiv_id,
             const gchar* doi,
             const gchar* pdf_file)
{
    Paper* p = g_new0(Paper, 1);

    p->title = title ? g_strdup(title) : NULL;
    p->authors_count = authors_count ? authors_count : 0;
    p->authors = NULL;
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
    p->pdf_file = pdf_file ? g_strdup(pdf_file) : NULL;

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
add_paper(PaperDatabase* db, Paper* paper)
{
    // printf("adding paper\n");
    if (db->count >= db->capacity) {
        //    printf("count: %i, capacity: %i\n", db->count, db->capacity);
        db->capacity *= 2;
        db->papers = realloc(db->papers, sizeof(Paper*) * db->capacity);
    }
    db->papers[db->count] = paper;
    db->count++;
}

void
free_paper(Paper* p)
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
    g_free(p);
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
