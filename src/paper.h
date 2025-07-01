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
    GMutex lock;
    // gchar* hash; // TODO: add hash field for duplicate detection
} Paper;

typedef struct
{
    Paper** papers;
    gint count;
    gint capacity;
    gchar* path;
    gchar* cache;
    GRWLock lock;
} PaperDatabase;

/* Macros */
#define WITH_PAPER_LOCK(p, code_block)                                         \
    do {                                                                       \
        g_mutex_lock(&(p)->lock);                                              \
        code_block;                                                            \
        g_mutex_unlock(&(p)->lock);                                            \
    } while (0)

#define WITH_DB_WRITE_LOCK(db, code_block)                                     \
    do {                                                                       \
        g_rw_lock_writer_lock(&(db)->lock);                                    \
        code_block;                                                            \
        g_rw_lock_writer_unlock(&(db)->lock);                                  \
    } while (0)

#define WITH_DB_READ_LOCK(db, code_block)                                      \
    do {                                                                       \
        g_rw_lock_reader_lock(&(db)->lock);                                    \
        code_block;                                                            \
        g_rw_lock_reader_unlock(&(db)->lock);                                  \
    } while (0)

/**
 * Creates an empty Paper struct, adds it to @db,
 * and returns a pointer to it.
 * @db owns the returned Paper.
 */
Paper*
initialize_paper(PaperDatabase* db, const gchar* pdf_file, GError** error);

/**
 * Creates a Paper struct with the given parameters, adds it to @db,
 * and returns a pointer to it.
 * On failure, returns NULL and sets *error accordingly.
 * @db owns the returned Paper.
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
 * Caller takes ownership.
 */
PaperDatabase*
create_database(int initial_capacity, gchar* path, gchar* cache);

/**
 * Loads a PaperDatabase from the given json_path and cache_path into @db.
 * On failure, returns FALSE. Handles errors internally.
 * TODO: use async task
 * TODO: maybe don't handle errors internally?
 */
gboolean
load_database(PaperDatabase* db,
              const gchar* json_path,
              const gchar* cache_path);

/**
 * Asynchronously write the database out as JSON to db->path and cache to
 * db->cache.
 */
void
sync_json_and_cache(PaperDatabase* db);

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
 * Resets the database to its initial state.
 * All Papers are removed, and the database is empty.
 */
void
reset_database(PaperDatabase* db);

/**
 * Frees a PaperDatabase struct and all its Papers.
 */
void
free_database(PaperDatabase* db);
