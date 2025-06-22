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

G_END_DECLS
