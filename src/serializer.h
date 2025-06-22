/* serializer.h */
#pragma once

#include "paper.h"
#include <glib.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * Check if the cache file is newer than the JSON file.
 * Returns TRUE if cache exists and its modification time >= JSON's.
 */
bool cache_up_to_date(const char *json_path, const char *cache_path);

/**
 * Write the in-memory PaperDatabase to a binary cache file.
 * On error, returns FALSE and sets *error.
 */
bool write_cache(const PaperDatabase *db, GError **error);

/**
 * Load papers from the binary cache file into the database.
 * On success returns TRUE; FALSE on error (sets *error) or if cache is empty.
 */
bool load_cache(PaperDatabase *db, GError **error);

/**
 * Return the number of entries in the cache, or 0 if empty/error.
 */
int load_cache_count(const PaperDatabase *db, GError **error);

G_END_DECLS
