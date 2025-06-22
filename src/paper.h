#pragma once

#include <glib.h>

typedef struct
{
    gchar* title;
    gchar** authors;
    gint authors_count;
    gint year;
    gchar** keywords;
    gint keyword_count;
    gchar* abstract;
    gchar* arxiv_id;
    gchar* doi;
    gchar* pdf_file;
} Paper;

typedef struct
{
    Paper** papers;
    gint count;
    gint capacity;
    gchar* path;
    gchar* cache;
} PaperDatabase;

Paper*
create_empty_paper();

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
             const gchar* pdf_file);

PaperDatabase*
create_database(int initial_capacity, gchar* path, gchar* cache);

void
add_paper(PaperDatabase* db, Paper* paper);

void
free_paper(Paper* p);

void
free_database(PaperDatabase* db);
