#include "cmd_options.h"
#include "config.h"
#include "gio/gio.h"
#include "gui/gui.h"
#include "loom.h"
#include "paper.h"
#include <glib.h>
#include <gtk/gtk.h>

#define MAX_RESULTS 10

static void
free_options()
{
    g_strfreev(app_flags.import_paths);
    g_free(app_flags.paperparser_path);
    g_free(app_flags.cache_path);
    g_free(app_flags.json_path);
}
static void
on_activate(GApplication* app, gpointer user_data)
{
    pango_cairo_font_map_get_default();
    gdk_display_manager_get();
    PaperDatabase* db = user_data;

    // if HEADLESS_MODE
    // run_headless(app, db);
    // else
    load_database(db, app_flags.json_path, app_flags.cache_path);
    gui_run(GTK_APPLICATION(app), db);
}

// TODO: implement all flags
static int
on_command_line(GApplication* app,
                GApplicationCommandLine* cmdline,
                gpointer user_data)
{
    (void)cmdline;
    (void)user_data;
    //PaperDatabase* db = user_data;

    // GError* error = NULL;

    // debug flags
    if (debug_flags.version) {
        g_print("PaperPusher v%s\n", VERSION);
        return 0;
    }

    if (debug_flags.debug) {
        g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
        // TODO: implement debug mode
    }

    if (debug_flags.mock_data) {
        g_print("Using mock data\n");
        // TODO: implement mock data
    }

    /* Actual program logic is happening from here on */

    // load available data into db
    //load_database(db, app_flags.json_path, app_flags.cache_path);

    // app flags
    if (app_flags.import_paths != NULL) {
        for (int i = 0; app_flags.import_paths[i] != NULL; i++) {
            const gchar* path = app_flags.import_paths[i];
            if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
                g_print("Importing directory: %s\n", path);
                // import_directory(path);
            } else {
                g_print("Importing file: %s\n", path);
                // import_file(path);
            }
        }
        return -1;
    }
    g_application_activate(app);
    return 0;
}

static void
on_startup(GApplication* app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    g_debug("Startup hook!");
    g_set_application_name("PaperPusher");

    // Force GTK to initialize stuff on main thread
    // GtkWidget* tmp = gtk_label_new(NULL);
    // gtk_widget_destroy(tmp);
}

int
main(int argc, char** argv)
{
    GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
    GtkApplication* app =
      gtk_application_new("com.numberjedi.paperpusher", flags); // freed before function return

    PaperDatabase* db = create_database(1, JSON_PATH, CACHE_PATH); // freed before function return

    g_signal_connect(app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), db);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), db);
    // TODO: add open handler
    // g_signal_connect(app, "open", G_CALLBACK(on_open), db);

    // Register options
    g_application_add_main_option_entries(G_APPLICATION(app), cmd_options);

    /* Run the main GTK loop */
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    /* Cleanup */
    g_object_unref(app);
    // g_thread_pool_free(global_pool, FALSE, TRUE);
    free_database(db);
    free_options();
    return status;
}
