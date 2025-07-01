// key_handler.c
#define G_LOG_DOMAIN "key_handler"

#include "gui/key_handler.h"
#include "gui/gui.h"
#include "gui/pdf_viewer.h"

#include <gdk/gdkkeysyms.h>
#include <glib.h>
#include <gtk/gtk.h>

// --- Modes ---
typedef enum
{
    MODE_NORMAL,
    MODE_INSERT
} AppMode;

typedef struct
{
    AppMode mode;
    guint keychain[MAX_KEYCHAIN_LEN]; // stores Gdk keyvals
    int chain_len;
    GtkWidget* key_hint_label; // the keybind hint display at bottom
    GtkWidget* mode_label;
} KeyState;

static KeyState app_keys;
static AppContext* context;

Keybinding normal_mode_bindings[MAX_BINDINGS];
int normal_mode_bindings_count = 0;

// --- Utility ---
static gboolean
is_leader(guint key)
{
    return key == LEADER_KEY;
}
/* Checks if key is <Space>. */

/**
 * Converts a sequence string like "<leader>p l" into a keyval array.
 *
 * @param sequence   The input string (e.g. "<leader>p l")
 * @param out_keys   Output array of guint keyvals (e.g. [space, p, l])
 * @param max_len    Max keys allowed
 * @return Number of keys parsed, or -1 on error.
 */
static int
parse_sequence_to_keychain(const char* sequence, guint* out_keys, int max_len)
{
    int count = 0;
    const char* p = sequence;
    // make sure no garbage is in the array
    memset(out_keys, 0, sizeof(guint) * max_len);
    while (*p && count < max_len) {
        // skip whitespace
        p += strspn(p, " ");

        if (strncmp(p, "<leader>", 8) == 0) {
            out_keys[count++] = LEADER_KEY;
            p += 8;
            continue;
        }

        if (*p == '\0')
            break;

        // parse single char as key
        char str[2] = { *p, '\0' };
        guint keyval = gdk_keyval_from_name(str);
        if (keyval == GDK_KEY_VoidSymbol) {
            g_warning("Invalid key '%s' in sequence '%s'", str, sequence);
            return -1;
        }

        out_keys[count++] = keyval;
        p++;
        // g_debug("Parsed key '%s' as %d", str, keyval);
    }
    return count;
}

static void
add_normal_binding(const gchar* sequence,
                   ActionFn action,
                   const gchar* description)
{
    if (normal_mode_bindings_count >= MAX_BINDINGS)
        return;
    normal_mode_bindings[normal_mode_bindings_count] = (Keybinding){
        .sequence = sequence, .action = action, .description = description
    };
    parse_sequence_to_keychain(
      sequence,
      normal_mode_bindings[normal_mode_bindings_count].keychain,
      MAX_KEYCHAIN_LEN);
    normal_mode_bindings_count++;
}

// --- Hint Bar Updates ---
static void
update_hint_bar()
{
    // g_debug("update_hint_bar");
    gchar mode_label_text[7] = { 0 };
    switch (app_keys.mode) {
        case MODE_NORMAL:
            g_strlcpy(mode_label_text, "NORMAL", 7);
            break;
        case MODE_INSERT:
            g_strlcpy(mode_label_text, "SEARCH", 7);
            break;
    }
    gtk_label_set_text(GTK_LABEL(app_keys.mode_label), mode_label_text);

    if (app_keys.mode == MODE_INSERT) {
        gtk_label_set_text(GTK_LABEL(app_keys.key_hint_label),
                           "Esc (Search mode active)");
        return;
    }

    GString* hint_text = g_string_new(NULL); // freed before return
    GHashTable* seen_keys = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (int i = 0; i < normal_mode_bindings_count; ++i) {
        const Keybinding* kb = &normal_mode_bindings[i];

        // get length of this binding's sequence
        guint keys[MAX_KEYCHAIN_LEN];
        int len =
          parse_sequence_to_keychain(kb->sequence, keys, MAX_KEYCHAIN_LEN);
        // g_debug("Checking binding '%s'(%i,%i,%i)",
        //         kb->sequence,
        //         kb->keychain[0],
        //         kb->keychain[1],
        //         kb->keychain[2]);
        if (len <= app_keys.chain_len)
            continue;

        // does the current keychain match this binding's prefix?
        if (memcmp(
              keys, app_keys.keychain, app_keys.chain_len * sizeof(guint)) != 0)
            continue;

        // Get the next key in the chain
        guint next_key = keys[app_keys.chain_len];
        const gchar* keyname = gdk_keyval_name(next_key); // static string

        // g_debug("Appending binding '%s' (next: %i)", kb->sequence, next_key);
        gboolean is_final = (app_keys.chain_len == len - 1);
        // Append to the hint string: key - description
        if (is_final)
            g_string_append_printf(
              hint_text, "\t%s - %s", keyname, kb->description);
        else if (!g_hash_table_contains(seen_keys, GINT_TO_POINTER(next_key))) {
            // g_debug("Not final, not in hash table");
            g_string_append_printf(hint_text, "\t%s+", keyname);
            g_hash_table_add(seen_keys, GINT_TO_POINTER(next_key));
        }
    }

    // use KeyState to extract available next keys from normal_mode_bindings

    // add next keys to key_hint_label with their descriptions from
    // normal_mode_bindings

    gtk_label_set_text(GTK_LABEL(app_keys.key_hint_label), hint_text->str);
    g_string_free(hint_text, TRUE);
    g_hash_table_destroy(seen_keys);
}
/* Based on current mode + chain, show possible next options. */

// --- Chain Handling ---
static void
reset_keychain()
{
    app_keys.chain_len = 0;
    memset(app_keys.keychain, 0, sizeof(app_keys.keychain));
}

static gboolean
is_final_key(guint key)
{
    // check if key is final in any normal mode bindings
    for (int i = 0; i < normal_mode_bindings_count; ++i) {
        if (key == normal_mode_bindings[i].keychain[app_keys.chain_len])
            return TRUE;
    }
    return FALSE;
}

/**
 * Checks if key is valid next in any normal mode bindings.
 * Returns TRUE if key is valid next, FALSE otherwise.
 */
static gboolean
is_valid_next_key(guint key)
{
    // check if key is valid next in any normal mode bindings
    for (int i = 0; i < normal_mode_bindings_count; ++i) {
        // make sure the chains up to new key are the same
        if (memcmp(normal_mode_bindings[i].keychain,
                   app_keys.keychain,
                   app_keys.chain_len * sizeof(guint)) != 0)
            continue;
        // then compare new key with next key in chain
        if (key == normal_mode_bindings[i].keychain[app_keys.chain_len])
            return TRUE;
    }
    return FALSE;
}

static gboolean
push_key_to_chain(guint key)
{
    // only if valid key stroke
    // if not, do nothing
    gchar* keyname = gdk_keyval_name(key); // static string
    if (is_valid_next_key(key)) {
        g_debug("push_key_to_chain %s", keyname);
        app_keys.keychain[app_keys.chain_len++] = key;
        return TRUE;
    } else {
        g_debug("Invalid key %s, doing nothing", keyname);
        return FALSE;
        // reset_keychain();
    }
}

/**
 * Attempts to match the current chain of keys against the registered
 * keybindings. If an exact match is found, executes the associated action and
 * returns true. If no match is found, returns false.
 */
static gboolean
try_execute_chain()
{
    for (int i = 0; i < normal_mode_bindings_count; ++i) {
        // make sure the chains up to new key are the same
        if (memcmp(normal_mode_bindings[i].keychain,
                   app_keys.keychain,
                   app_keys.chain_len * sizeof(guint)) != 0)
            continue;
        // check if chain is complete
        if (!(normal_mode_bindings[i].keychain[app_keys.chain_len] == 0))
            break;
        // execute action
        g_debug("Executing action for binding '%s'",
                normal_mode_bindings[i].sequence);
        if (normal_mode_bindings[i].action)
            normal_mode_bindings[i].action();
        return TRUE;
    }

    return FALSE;
}

// --- Mode Switching ---
static void
enter_insert_mode()
{
    app_keys.mode = MODE_INSERT;
    focus_search_entry();
    reset_keychain();
    update_hint_bar();
}

static void
enter_normal_mode()
{
    app_keys.mode = MODE_NORMAL;
    focus_main_window();
    reset_keychain();
    update_hint_bar();
}

// --- Actions ---
static void
act_open_pdf()
{
    g_debug("act_open_pdf");
    open_system_viewer();
}
static void
act_delete_entry()
{
    remove_entry_from_db();
    g_debug("act_delete_entry");
}
static void
act_edit_metadata()
{
    g_debug("act_edit_metadata");
}
static void
act_fetch_metadata()
{
    g_debug("act_fetch_metadata");
}
// static void act_toggle_project_mode(){}
static void
act_summarize()
{
    g_debug("act_summarize");
}
static void
act_export_bib()
{
    g_debug("act_export_bib");
}
static void
act_add_to_project()
{
    g_debug("act_add_to_project");
}
static void
act_remove_from_project()
{
    g_debug("act_remove_from_project");
}
static void
act_project_create()
{
    g_debug("act_project_create");
}
static void
act_project_list()
{
    g_debug("act_project_list");
}
static void
act_project_delete()
{
    g_debug("act_project_delete");
}
static void
act_project_view()
{
    g_debug("act_project_view");
}
static void
act_project_export_bib()
{
    g_debug("act_project_export_bib");
}
static void
act_reset_database()
{
    gui_reset_database();
    g_debug("act_reset_database");
}
/* These get called when key chains resolve. */

// --- Init ---
static void
register_keybindings()
{
    normal_mode_bindings_count = 0;

    // Papers
    add_normal_binding("<leader>o", act_open_pdf, "Open selected paper");
    add_normal_binding("<leader>d", act_delete_entry, "Delete from database");
    add_normal_binding("<leader>e", act_edit_metadata, "Edit metadata");
    add_normal_binding(
      "<leader>m", act_fetch_metadata, "Fetch metadata (arXiv/DOI)");
    add_normal_binding("<leader>a", act_add_to_project, "Add to project");
    add_normal_binding(
      "<leader>r", act_remove_from_project, "Remove from project");
    add_normal_binding("<leader>x", act_export_bib, "Export bibliography");
    add_normal_binding("<leader>s", act_summarize, "Summarize using LLM");

    // Projects
    add_normal_binding("<leader>p c", act_project_create, "Create new project");
    add_normal_binding("<leader>p l", act_project_list, "List all projects");
    add_normal_binding("<leader>p d", act_project_delete, "Delete project");
    add_normal_binding("<leader>p v", act_project_view, "View project papers");
    add_normal_binding(
      "<leader>p x", act_project_export_bib, "Export project bibliography");
    add_normal_binding("<leader>D y", act_reset_database, "yes, reset database");
    add_normal_binding("<leader>D n", NULL, "no, gtfoh");
}

// --- Main Handler ---
gboolean
handle_key_event(GtkWidget* widget, GdkEventKey* event, gpointer user_data)
{
    /* Replaces on_key_press — route key events here.
       Handles mode switching, key chaining, and action execution. */
    (void)widget;
    (void)user_data;

    GtkListBox* results_list = context->results_list;

    guint keyval = event->keyval;
    gboolean ctrl = (event->state & GDK_CONTROL_MASK) != 0;

    // always-on keybinds
    if (keyval == GDK_KEY_Down || (ctrl && keyval == GDK_KEY_n)) {
        navigate(results_list, TRUE);
        return TRUE;
    } else if (keyval == GDK_KEY_Up || (ctrl && keyval == GDK_KEY_p)) {
        navigate(results_list, FALSE);
        return TRUE;
    }
    // pdf viewer scrolling, if a pdf is loaded
    else if ((ctrl && keyval == GDK_KEY_f) || keyval == GDK_KEY_Page_Down) {
        pdf_viewer_scroll_by(1.0);
        return TRUE;
    } else if ((ctrl && keyval == GDK_KEY_b) || keyval == GDK_KEY_Page_Up) {
        pdf_viewer_scroll_by(-1.0);
        return TRUE;
    } else if (ctrl && keyval == GDK_KEY_o) {
        open_system_viewer();
        return TRUE;
    } else if (ctrl && keyval == GDK_KEY_d) {
        remove_entry_from_db();
        return TRUE;
    }

    // Insert mode logic
    if (app_keys.mode == MODE_INSERT) {
        if (keyval == GDK_KEY_Escape) {
            enter_normal_mode();
            return TRUE;
        }

        // Let insert mode widgets (like search entry) handle typing
        return FALSE;
    }

    // enter insert & type mode only if chain is empty
    if (app_keys.chain_len == 0 && keyval == GDK_KEY_i) {
        enter_insert_mode();
        return FALSE;
    }
    // enter insert mode & type on non-<leader>/esc if chain is empty
    if (app_keys.chain_len == 0 && !is_leader(keyval) &&
        keyval != GDK_KEY_Escape) {
        enter_insert_mode();
        return FALSE;
    }

    if (keyval == GDK_KEY_Escape) {
        reset_keychain();
        update_hint_bar();
        return TRUE;
    }

    // push key to current chain
    // execute chain if it is complete
    if (push_key_to_chain(keyval)) {
        if (try_execute_chain()) {
            reset_keychain();
            update_hint_bar();
            return TRUE;
        }
        update_hint_bar();
    }

    // Try to match a complete chain
    // if (try_execute_chain(keyval)) {
    //     reset_keychain();
    //     return TRUE;
    // }

    // Still building chain → update hint bar with next options
    // update_hint_bar();

    // Always capture keys in normal mode to block accidental typing
    return TRUE;
}

// --- Init ---
/**
 * Called in gui_run. Initializes mode and fetches hint bar widget.
 * Also sets up necessary state.
 */
void
init_keybinding_system(AppContext* ctx)
{
    /* Called in gui_run. Initializes mode and fetches hint bar widget.
       Also sets up necessary state. */
    app_keys.mode = MODE_NORMAL;
    app_keys.chain_len = 0;
    context = ctx;
    // zero out
    memset(app_keys.keychain, 0, sizeof(app_keys.keychain));

    register_keybindings();

    // Bind label from .ui
    GtkWidget* hint_bar =
      GTK_WIDGET(gtk_builder_get_object(context->builder, "key_hint_label"));
    if (!hint_bar) {
        g_warning("Failed to get key_hint_bar widget from .ui file");
        return;
    }
    GtkWidget* mode_label =
      GTK_WIDGET(gtk_builder_get_object(context->builder, "mode_label"));
    if (!mode_label) {
        g_warning("Failed to get mode_label widget from .ui file");
        return;
    }

    app_keys.key_hint_label = hint_bar;
    app_keys.mode_label = mode_label;
    update_hint_bar();
}
