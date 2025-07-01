#pragma once

#include "paper.h"
#include <glib.h>
#include <gtk/gtk.h>

#define MAX_RESULTS 10

G_BEGIN_DECLS

// Launch the GTK event loop on this database.
// Returns when the user closes the window.
void
gui_run(GtkApplication* app, PaperDatabase* db);

void
navigate(GtkListBox* box, gboolean next);

void
focus_search_entry();

void
focus_main_window();

void
open_system_viewer();

void
remove_entry_from_db();

void
gui_reset_database();

G_END_DECLS
