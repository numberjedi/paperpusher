#include "gui.h"
#include "../parser.h"
#include "../search.h"
#include "gui/pdf_viewer.h"
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <pango/pango.h>
#include <unistd.h>

/* App-global database reference (set in gui_run) */
static PaperDatabase* s_db;

static GtkEntry* search_entry;
static GtkListBox* results_list;
static GtkLabel* pdf_preview;

/* Forward declarations */
static void
on_search_changed(GtkEntry*, gpointer);
static void
on_results_row_selected(GtkListBox*, GtkListBoxRow*, gpointer);
static gboolean
on_key_press(GtkWidget*, GdkEventKey*, gpointer);
static void
on_pdf_dropped(GtkWidget*,
               GdkDragContext*,
               gint,
               gint,
               GtkSelectionData*,
               guint,
               guint,
               gpointer);
static void
parser_task(GTask*, gpointer, gpointer, GCancellable*);
static void
parser_ready(GObject*, GAsyncResult*, gpointer);
static GtkListBoxRow*
get_adjacent_row(GtkListBox*, GtkListBoxRow*, gboolean);
static void
navigate(GtkListBox*, gboolean);

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
static void
on_search_changed(GtkEntry* entry, gpointer user_data)
{
    (void)user_data;

    const char* q = gtk_entry_get_text(entry);
    const Paper* results[MAX_RESULTS];
    int found =
      search_papers(s_db->papers, s_db->count, q, results, MAX_RESULTS);

    gtk_list_box_unselect_all(results_list);

    GList* children = gtk_container_get_children(GTK_CONTAINER(results_list));
    for (GList* c = children; c; c = c->next)
        gtk_widget_destroy(GTK_WIDGET(c->data));
    g_list_free(children);

    for (int i = 0; i < found; ++i) {
        Paper* p = (Paper*)results[i];
        GtkWidget* row = gtk_list_box_row_new();

        // vbox
        GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_container_add(GTK_CONTAINER(row), vbox);

        // title
        gchar* safe_title = sanitize_label_text(p->title);
        GtkWidget* title = gtk_label_new(safe_title);
        g_free(safe_title);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(title, TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, TRUE, 0);

        // hbox
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_hexpand(hbox, TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

        // authors
        GString* safe_authors_string = g_string_new(NULL);
        for (int j = 0; j < p->authors_count; j++) {
            gchar* safe_author = sanitize_label_text(p->authors[j]);
            g_string_append(safe_authors_string, safe_author);
            g_free(safe_author);
        }
        GtkWidget* authors = gtk_label_new(safe_authors_string->str);
        gtk_label_set_xalign(GTK_LABEL(authors), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(authors), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(hbox), authors, FALSE, FALSE, 0);
        g_string_free(safe_authors_string, TRUE);

        // year
        char yearbuf[16];
        snprintf(yearbuf, sizeof(yearbuf), "(%d)", p->year);
        GtkWidget* year = gtk_label_new(yearbuf);
        gtk_label_set_xalign(GTK_LABEL(year), 0.0);
        gtk_box_pack_start(GTK_BOX(hbox), year, TRUE, TRUE, 0);

        g_object_set_data(G_OBJECT(row), "paper", p);

        gtk_list_box_insert(results_list, row, -1);
        gtk_widget_show_all(row);
    }

    if (found > 0) {
        GtkListBoxRow* first = gtk_list_box_get_row_at_index(results_list, 0);
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
    Paper* p = g_object_get_data(G_OBJECT(row), "paper");
    pdf_viewer_load(p->pdf_file);
}

/* Keyboard navigation helpers */
static GtkListBoxRow*
get_adjacent_row(GtkListBox* box, GtkListBoxRow* row, gboolean next)
{
    GList* children = gtk_container_get_children(GTK_CONTAINER(box));
    GtkListBoxRow* res = NULL;
    if (!row) {
        // no current selection. pick fist (or last if prev)
        if (children) {
            if (next)
                res = GTK_LIST_BOX_ROW(children->data);
            else {
                GList* last = g_list_last(children);
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
    GtkListBoxRow* sel = gtk_list_box_get_selected_row(box);
    GtkListBoxRow* adj = get_adjacent_row(box, sel, next);
    if (adj) {
        gtk_list_box_select_row(box, adj);
        // Keep focus in search entry
        gtk_widget_grab_focus(GTK_WIDGET(search_entry));
    }
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
    }
    if (key == GDK_KEY_Up || (ctrl && key == GDK_KEY_p)) {
        navigate(results_list, FALSE);
        return TRUE;
    }

    // pdf viewer scrolling, if a pdf is loaded
    if ((ctrl && key == GDK_KEY_f) || key == GDK_KEY_Page_Down) {
        pdf_viewer_scroll_by(1.0);
        return TRUE;
    } else if ((ctrl && key == GDK_KEY_b) || key == GDK_KEY_Page_Up) {
        pdf_viewer_scroll_by(-1.0);
        return TRUE;
    }
    return FALSE;
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

    gchar** uris = gtk_selection_data_get_uris(selection_data);
    if (!uris)
        return;

    for (int i = 0; uris && uris[i]; ++i) {
        gchar* path = g_filename_from_uri(uris[i], NULL, NULL);
        if (!path)
            continue;

        GTask* task = g_task_new(NULL, NULL, parser_ready, NULL);
        g_task_set_task_data(task, path, g_free);
        g_task_run_in_thread(task, parser_task);
        g_object_unref(task);
    }

    g_strfreev(uris);
    gtk_drag_finish(context, TRUE, FALSE, time);
}

/* Background parser thread */
static void
parser_task(GTask* task,
            gpointer source_object,
            gpointer task_data,
            GCancellable* cancellable)
{
    (void)source_object;
    (void)cancellable;

    gchar* path = task_data;
    GError* err = NULL;
    Paper* p = parser_run(s_db, path, &err);
    if (!p) {
        g_task_return_error(task, err);
    } else {
        g_task_return_pointer(task, p, NULL);
    }
    // GTask will free on its own
}

/* Main-loop callback for parser completion */
static void
parser_ready(GObject* source_object, GAsyncResult* result, gpointer user_data)
{
    (void)source_object;
    (void)user_data;

    GError* error = NULL;
    Paper* p = g_task_propagate_pointer(G_TASK(result), &error);
    if (error) {
        g_printerr("Error parsing PDF metadata: %s\n", error->message);
        g_error_free(error);
    }
    // (p is owned by the PaperDatabase now, no manual free)
}

/* --- Launch the GUI main loop --- */
void
gui_run(PaperDatabase* db)
{
    s_db = db;

    // load Glade UI
    GtkBuilder* b = gtk_builder_new_from_file("src/gui/main_window.ui");
    GtkWidget* w = GTK_WIDGET(gtk_builder_get_object(b, "main_window"));
    pdf_viewer_setup(b, "pdf_scrollbar", "pdf_view");

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
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    // enable drag and drop for PDF files
    GtkTargetEntry target = { "text/uri-list", 0, 0 };
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_COPY);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_pdf_dropped), NULL);

    // show & run
    gtk_widget_show_all(w);
    gtk_main();
}
