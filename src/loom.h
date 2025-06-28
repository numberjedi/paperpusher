// loom.h
#pragma once

#include "gio/gio.h"
#include <glib.h>

typedef struct _Loom
{
    GThreadPool* pool;
    guint max_threads;
    GHashTable* running_threads; // tag -> LoomActiveTask*
    GHashTable* completed_tags;  // tag -> GINT_TO_POINTER(TRUE)
    GQueue* queued_threads;      // Queue of LoomTaskSpec*
    // gboolean is_lifo;
    GMutex lock;
} Loom;

typedef gpointer (*LoomShuttleFunc)(gpointer shuttle_data, GError** error);
typedef void (*LoomKnotFunc)(gpointer knot_data,
                             gpointer shuttle_data,
                             gpointer result,
                             GError* error);
typedef void (*LoomProgressFunc)(gpointer progress_data,
                                 gpointer worker_data,
                                 gpointer callback_data,
                                 GError* error);

typedef struct
{
    const gchar* tag;           // Unique task tag
    const gchar** dependencies; // NULL-terminated array of tags
    gint priority; // Lower is higher (kek). GUI threads should be negative, IO
                   // threads non-negative
    LoomShuttleFunc shuttle; // Runs the thread
    LoomKnotFunc knot;       // Ties off the thread
    LoomProgressFunc progress;
    gpointer shuttle_data; // Passed to shuttle/tie_off
    GDestroyNotify task_data_destroy;
    guint timeout_ms;   // 0 = no timeout
    gpointer knot_data; // Passed to tie_off/progress
    gboolean is_lifo;
} LoomThreadSpec;

/**
 * Returns a default LoomThreadSpec.
 */
LoomThreadSpec
loom_thread_spec_default(void);

/**
 * Returns the default Loom object.
 * If it doesn't exist yet, it is created.
 */
Loom*
loom_get_default(void);

/**
 * Creates a new Loom object with the given number of threads.
 * If max_threads <= 0, the number of threads is auto-detected.
 */
Loom*
loom_new(gint max_threads);

/**
 * Runs the given thread_spec on the Loom object.
 *
 * If the thread_spec has dependencies, they are blocked until they are
 * completed. If the thread_spec has a timeout, it is cancelled after the
 * timeout. If the thread_spec has a progress callback, it is called with the
 * progress data. If the thread_spec has a knot callback, it is called when the
 * thread is tied off.
 * @param loom Loom object
 * @param spec LoomThreadSpec struct
 * @param out_cancellable If non-NULL, a pointer to a GCancellable object will
 * be set to the thread's cancellable object.
 */
void
loom_queue_thread(Loom* loom,
                  const LoomThreadSpec* spec,
                  GCancellable** out_cancellable);
/**
 * Cancels the thread with the given tag.
 */
void
loom_snip(Loom* loom, const char* tag);

/**
 * Disassembles the Loom object, freeing all memory.
 */
void
loom_disassemble(Loom* loom);
