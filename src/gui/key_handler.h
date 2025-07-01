// key_handler.h
#pragma once

#include <gtk/gtk.h>

#define MAX_BINDINGS 128
#define LEADER_KEY GDK_KEY_space
#define MAX_KEYCHAIN_LEN 8

typedef struct
{
    GtkListBox* results_list;
    GtkBuilder* builder;
} AppContext;

typedef void (*ActionFn)();

typedef struct
{
    const gchar* sequence;            // e.g. " <leader>o"
    guint keychain[MAX_KEYCHAIN_LEN]; // stores Gdk keyvals
    ActionFn action;
    const gchar* description;
} Keybinding;

void
init_keybinding_system(AppContext* context);

gboolean
handle_key_event(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
