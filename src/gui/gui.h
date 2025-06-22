#pragma once

#include "paper.h"
#include <glib.h>

#define MAX_RESULTS 10

G_BEGIN_DECLS

// Launch the GTK event loop on this database.
// Returns when the user closes the window.
void gui_run(PaperDatabase *db);

G_END_DECLS
