#include "config.h"
#include "gui/gui.h"
#include "loader.h"
#include "paper.h"
#include "serializer.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#define MAX_RESULTS 10

int
main(int argc, char** argv)
{
    gtk_init(&argc, &argv);

    PaperDatabase* db = create_database(1, JSON_PATH, CACHE_PATH);

    GError* error = NULL;
    /* Load from cache or JSON file */
    if (!cache_up_to_date(JSON_PATH, CACHE_PATH) || !load_cache(db, &error)) {
        if (error) {
            g_printerr("Error loading cache '%s': %s\n", CACHE_PATH, error->message);
            g_clear_error(&error);
        } else
            g_printerr("Cache not up to date, attempting to load from JSON.\n");
        if (!load_papers_from_json(db, &error)) {
            g_printerr(
              "Error loading JSON '%s': %s\nContinuing with empty database.\n",
              JSON_PATH,
              error->message);
            g_clear_error(&error);
        }
    }

    /* sync JSON and cache */
    write_json_async(db);
    if (!write_cache(db, &error)) {
        g_printerr(
          "Error writing cache '%s': %s\n", CACHE_PATH, error->message);
        g_clear_error(&error);
    }

    gui_run(db);

    // Cleanup
    free_database(db);
    return EXIT_SUCCESS;
}
