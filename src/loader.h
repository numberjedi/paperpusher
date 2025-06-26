/* loader.h */
#pragma once

#include "paper.h"
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

G_BEGIN_DECLS

/**
 * Open the file at `path` for reading, or create an empty file if it doesn't
 * exist. Returns a newly opened FILE* on success, or NULL on failure (errno
 * set).
 */
FILE*
open_file_read_or_create(const char* path);

/**
 * Load papers from JSON file at db->path into the given PaperDatabase.
 * On success returns TRUE. If the file is empty or missing, returns TRUE.
 * On parse or I/O error, returns FALSE and sets *error.
 */
bool
load_papers_from_json(PaperDatabase* db, GError** error);

/**
 * Synchronously write the database out as JSON to db->path.
 * Returns TRUE on success, or FALSE on failure (sets *error).
 */
bool
write_json(const PaperDatabase* db, GError** error);

/**
 * Launch write_json() in a detached background thread.
 */
void
write_json_async(const PaperDatabase* db);

G_END_DECLS
