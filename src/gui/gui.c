#include "gui.h"
#include "loom.h"
#include "paper.h"
#include "parser.h"
#include "pdf_viewer.h"
#include "search.h"
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <pango/pango.h>
#include <unistd.h>

/* App-global database reference (set in gui_run) */
static PaperDatabase* s_db;
static Loom* gui_loom;

static GtkEntry* search_entry;
static GtkListBox* results_list;
static GtkLabel* pdf_preview;

/**
 * disassemble Loom object when app is shutting down
 */
static void
on_shutdown(GApplication* app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    loom_disassemble(gui_loom);
}

/* ensure label text is valid UTF-8 */
static gchar*
sanitize_label_text(const char* orig)
{
    if (!orig)
        return g_strdup("");
    if (g_utf8_validate(orig, -1, NULL))
        return g_strdup(orig);
    return g_utf8_make_valid(orig, -1);
}

/* Search event: repopulate result list */
// TODO: make this async?
static void
on_search_changed(GtkEntry* entry, gpointer user_data)
{
    (void)user_data;

    const char* q = gtk_entry_get_text(entry); // owned by widget
    const Paper* results[MAX_RESULTS];
    int found = search_papers(s_db, s_db->count, q, results, MAX_RESULTS);

    gtk_list_box_unselect_all(results_list);

    GList* children = gtk_container_get_children(
      GTK_CONTAINER(results_list)); // freed before return
    // clear old results
    for (GList* c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    // add new results
    for (int i = 0; i < found; ++i) {
        Paper* p = (Paper*)results[i];
        GtkWidget* row = gtk_list_box_row_new(); // owned by box

        // vbox
        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_container_add(GTK_CONTAINER(row), vbox); // vbox ownewd by row now

        // title
        gchar* safe_title =
          sanitize_label_text(p->title); // freed before return
        GtkWidget* title = gtk_label_new(safe_title);
        g_free(safe_title);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(title, TRUE);
        gtk_box_pack_start(
          GTK_BOX(vbox), title, FALSE, TRUE, 0); // title owned by vbox now

        // hbox
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_hexpand(hbox, TRUE);
        gtk_box_pack_start(
          GTK_BOX(vbox), hbox, FALSE, TRUE, 0); // hbox owned by vbox now

        // authors
        GString* safe_authors_string =
          g_string_new(NULL); // freed before return
        for (int j = 0; j < p->authors_count; j++) {
            gchar* safe_author =
              sanitize_label_text(p->authors[j]); // freed before return
            g_string_append(safe_authors_string, safe_author); // epic
            g_free(safe_author);
        }
        GtkWidget* authors = gtk_label_new(safe_authors_string->str);
        gtk_label_set_xalign(GTK_LABEL(authors), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(authors), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(
          GTK_BOX(hbox), authors, FALSE, FALSE, 0); // authors owned by hbox now
        g_string_free(safe_authors_string, TRUE);

        // year
        char yearbuf[16];
        snprintf(yearbuf, sizeof(yearbuf), "(%d)", p->year);
        GtkWidget* year = gtk_label_new(yearbuf);
        gtk_label_set_xalign(GTK_LABEL(year), 0.0);
        gtk_box_pack_start(
          GTK_BOX(hbox), year, TRUE, TRUE, 0); // year owned by hbox now

        g_object_set_data(G_OBJECT(row), "paper", p);

        gtk_list_box_insert(results_list, row, -1);
        gtk_widget_show_all(row);
    }

    if (found > 0) {
        GtkListBoxRow* first =
          gtk_list_box_get_row_at_index(results_list, 0); // owned by box
        gtk_list_box_select_row(results_list, first);
    }
    gtk_widget_show_all(GTK_WIDGET(results_list));
}

/* Result selection: update PDF preview */
static void
on_results_row_selected(GtkListBox* box, GtkListBoxRow* row, gpointer user_data)
{
    (void)box;
    (void)user_data;

    if (!row) {
        pdf_viewer_load(NULL);
        return;
    }
    // TODO: check for NULL (after deletion or sth)
    Paper* p = g_object_get_data(G_OBJECT(row), "paper"); // (owned by db)
    pdf_viewer_load(p->pdf_file);
}

/* Keyboard navigation helpers */
static GtkListBoxRow*
get_adjacent_row(GtkListBox* box, GtkListBoxRow* row, gboolean next)
{
    GList* children =
      gtk_container_get_children(GTK_CONTAINER(box)); // freed before return
    GtkListBoxRow* res = NULL;
    if (!row) {
        // no current selection. pick fist (or last if prev)
        if (children) {
            if (next)
                res = GTK_LIST_BOX_ROW(children->data); // owned by box
            else {
                GList* last = g_list_last(children); // freed with children
                res = GTK_LIST_BOX_ROW(last->data);
            }
        }
    } else {
        // find row in list, then pick sibling
        for (GList* l = children; l; l = l->next) {
            if (l->data == row) {
                if (next && l->next)
                    res = GTK_LIST_BOX_ROW(l->next->data);
                else if (!next && l->prev)
                    res = GTK_LIST_BOX_ROW(l->prev->data);
                break;
            }
        }
    }
    g_list_free(children);
    return res;
}

static void
navigate(GtkListBox* box, gboolean next)
{
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(box); // owned by box
    GtkListBoxRow* adj = get_adjacent_row(box, sel, next);   // owned by box
    if (adj) {
        gtk_list_box_select_row(box, adj);
        // Keep focus in search entry
        gtk_widget_grab_focus(GTK_WIDGET(search_entry));
    }
}

static void
open_system_viewer()
{
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(results_list);
    if (!sel)
        return;
    Paper* p = g_object_get_data(G_OBJECT(sel), "paper"); // owned by db
    gchar* pdf_file = p->pdf_file;                        // owned by p
    g_debug("Opening '%s'\n", pdf_file);
    // if (g_str_has_prefix(pdf_file, "file://"))
    //     pdf_file += 7;
    GError* error = NULL; // handled before return
    g_autofree gchar* uri = g_filename_to_uri(pdf_file, NULL, &error);
    if (!error) {
        g_app_info_launch_default_for_uri(uri, NULL, &error);
        // g_autofree gchar* quoted = g_shell_quote(pdf_file);
        // g_autofree gchar* cmd = g_strconcat("xdg-open ", quoted, NULL);
        // g_spawn_command_line_async(cmd, &error);
    }
    if (error) {
        g_printerr("Error launching default app for URI '%s': %s\n",
                   uri,
                   error->message);
        g_clear_error(&error);
    }
}

static void 
remove_entry_from_db()
{
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(results_list);
    if (!sel)
        return;
    Paper* p = g_object_get_data(G_OBJECT(sel), "paper"); // owned by db
    gtk_list_box_unselect_row(results_list, sel);
    remove_paper(s_db, p);
    g_signal_emit_by_name(search_entry, "changed");
}

static gboolean
on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data)
{
    (void)widget;
    (void)user_data;

    guint key = event->keyval;
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;

    if (key == GDK_KEY_Down || (ctrl && key == GDK_KEY_n)) {
        navigate(results_list, TRUE);
        return TRUE;
    } else if (key == GDK_KEY_Up || (ctrl && key == GDK_KEY_p)) {
        navigate(results_list, FALSE);
        return TRUE;
    }
    // pdf viewer scrolling, if a pdf is loaded
    else if ((ctrl && key == GDK_KEY_f) || key == GDK_KEY_Page_Down) {
        pdf_viewer_scroll_by(1.0);
        return TRUE;
    } else if ((ctrl && key == GDK_KEY_b) || key == GDK_KEY_Page_Up) {
        pdf_viewer_scroll_by(-1.0);
        return TRUE;
    } else if (ctrl && key == GDK_KEY_o) {
        open_system_viewer();
        return TRUE;
    } else if (ctrl && key == GDK_KEY_d) {
        remove_entry_from_db();
        return TRUE;
    }
    return FALSE;
}

/**
 * Main-loop callback for parser completion. Takes db as argument (not global
 * s_db). Handles the propagated GError*
 */
static void
parser_task_callback(PaperDatabase* db,
                     Paper* p, // pass paper id instead?
                     gpointer user_data,
                     GError* error)
{
    (void)user_data;
    (void)db;

    if (error) {
        gchar* pdf_file = NULL;
        if (p && p->pdf_file)
            pdf_file = p->pdf_file;
        else
            pdf_file = "<N/A>";

        g_printerr(
          "Error parsing PDF metadata for file: %s.\nError message: %s.\n",
          pdf_file,
          error->message);
        g_error_free(error);
    }

    if (!p) {
        g_printerr("Error parsing file, result is NULL.\n");
        return;
    }
    // logic to update the progress bar goes here.
    // For now just print success message in the terminal.
    g_debug("Successfully parsed '%s'.\n", p->pdf_file);
    // TODO: update progress bar

    // (p is owned by the PaperDatabase now, do not free)
}

/* Background parser thread */
static void
fire_parser_task(gchar* path)
{
    if (!path)
        return;
    g_debug("Parsing '%s'...\n", path);
    async_parser_run(
      s_db, path, parser_task_callback, NULL); // takes ownership of path
}

/* Drag-and-drop: process dropped PDF URIs */
static void
on_pdf_dropped(GtkWidget* widget,
               GdkDragContext* context,
               gint x,
               gint y,
               GtkSelectionData* selection_data,
               guint info,
               guint time,
               gpointer user_data)
{
    (void)widget;
    (void)x;
    (void)y;
    (void)info;
    (void)user_data;

    gchar** uris = gtk_selection_data_get_uris(
      selection_data); // freed before function return
    if (!uris)
        return;

    // fire off a task for each URI
    for (int i = 0; uris && uris[i]; ++i) {
        // free path when the task returns
        gchar* path = g_filename_from_uri(
          uris[i], NULL, NULL); // freed by parser_task_callback()
        fire_parser_task(path); // takes ownership of path
    }

    g_strfreev(uris);
    gtk_drag_finish(context, TRUE, FALSE, time);
}

/* Launch the GUI main loop */
void
gui_run(GtkApplication* app, PaperDatabase* db)
{
    s_db = db;
    // int max_threads = g_settings_get_int(app_flags.settings, "gui-threads");
    int max_threads = MIN(4, g_get_num_processors() / 2);
    // g_idle_add((GSourceFunc)loom_new, GINT_TO_POINTER(max_threads));
    gui_loom = loom_new(max_threads);
    // gui_loom = loom_get_default();

    // load Glade UI
    GtkBuilder* b = gtk_builder_new_from_file("src/gui/main_window.ui");
    GtkWidget* w = GTK_WIDGET(gtk_builder_get_object(b, "main_window"));
    gtk_window_set_application(GTK_WINDOW(w), app);

    // setup PDF viewer
    pdf_viewer_setup(gui_loom, app, b, "pdf_scrollbar", "pdf_view");

    // grab widgets
    search_entry = GTK_ENTRY(gtk_builder_get_object(b, "search_entry"));
    results_list = GTK_LIST_BOX(gtk_builder_get_object(b, "results_list"));
    pdf_preview = GTK_LABEL(gtk_builder_get_object(b, "pdf_placeholder"));
    gtk_widget_grab_focus(GTK_WIDGET(search_entry));

    // connect handlers
    g_signal_connect(
      search_entry, "changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(
      results_list, "row-selected", G_CALLBACK(on_results_row_selected), NULL);
    g_signal_connect(w, "key-press-event", G_CALLBACK(on_key_press), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // enable drag and drop for PDF files
    GtkTargetEntry target = { "text/uri-list", 0, 0 };
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_COPY);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_pdf_dropped), NULL);

    // show & run
    gtk_widget_show_all(w);
    gtk_main();
}
