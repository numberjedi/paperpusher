#pragma once

#include <glib.h>

typedef struct
{
    gint id_in_db;
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
    // gchar* hash; // TODO: add hash field for duplicate detection
} Paper;

typedef struct
{
    Paper** papers;
    gint count;
    gint capacity;
    gchar* path;
    gchar* cache;
} PaperDatabase;

/**
 * Creates an empty Paper struct, adds it to @db,
 * and returns a pointer to it.
 */
Paper*
initialize_paper(PaperDatabase* db, const gchar* pdf_file, GError** error);

/**
 * Creates a Paper struct with the given parameters, adds it to @db,
 * and returns a pointer to it.
 * On failure, returns NULL and sets *error accordingly.
 */
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
             GError** error);

/**
 * Creates a PaperDatabase struct with the given initial capacity, json path and
 * cache path, returns pointer to it.
 */
PaperDatabase*
create_database(int initial_capacity, gchar* path, gchar* cache);

/**
 * Updates @paper with the given non-null parameters.
 */
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
             GError** error);

/**
 * Removes a Paper from the PaperDatabase.
 */
void
remove_paper(PaperDatabase* db, Paper* paper);

/**
 * Frees a PaperDatabase struct and all its Papers.
 */
void
free_database(PaperDatabase* db);
