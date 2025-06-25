/* parser.h */
#pragma once

#include "paper.h"
#include <glib.h>

G_BEGIN_DECLS // allows inclusion from C++

/**
 * Run the external `paperparser` executable on `pdf_path`, parse its JSON
 * output, populate a new Paper, add it to the database, and write out updates.
 *
 * On success, returns a newly allocated Paper* (transfer full ownership).
 * On error, returns NULL and sets *error accordingly.
 */
Paper *parser_run(PaperDatabase *db, const char *pdf_path, GError **error);

/**
 * Asynchronously run the external `paperparser` executable on `pdf_path`,
 * parse its JSON output, populate a new Paper, add it to the database,
 * and write out updates.
 *
 * On success, calls `callback` with @db, a newly allocated Paper* (owned by @db),
 * @user_data and a GError* set to NULL.
 * On error, calls `callback` with @db, NULL, @user_data and a GError* set.
 */
void async_parser_run(PaperDatabase *db,
                      char *pdf_path,
                      void (*callback)(PaperDatabase *, Paper *, gpointer, GError *),
                      gpointer user_data);

G_END_DECLS
