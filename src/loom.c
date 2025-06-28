// loom.c

#define G_LOG_DOMAIN "loom"

#include "loom.h"
#include <gio/gio.h>
#include <glib.h>

typedef struct
{
    Loom* owning_loom;
    const LoomThreadSpec* spec;
    GTask* thread;
    GCancellable* snippable;
    guint timeout_id;
} LoomActiveThread;

/* Global Loom object */
static Loom* global_loom = NULL;

/* Static helper functions */

// Forward declarations
static void
loom_tie_off(GObject* source, GAsyncResult* result, gpointer knot_data);

static void
free_loom_thread_spec(const LoomThreadSpec* spec)
{
    g_free((gchar*)spec->tag);
    g_strfreev((gchar**)spec->dependencies);
    g_free((gpointer)spec);
}

static void
free_loom_active_thread(LoomActiveThread* active_thread)
{
    // TODO: do I need to free the thread?
    free_loom_thread_spec(active_thread->spec);
    g_free(active_thread);
}

// TODO: test this
static gboolean
loom_thread_snapped(gpointer user_data)
{
    LoomActiveThread* active_thread = user_data;
    g_task_return_error(
      active_thread->thread,
      g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Task timed out"));
    return G_SOURCE_REMOVE;
}

/**
 * Shuttles the thread through the Loom using the given shuttle.
 */
static void
loom_shuttle_wrapper(GTask* thread,
                     gpointer source_object,
                     gpointer task_data,
                     GCancellable* cancellable)
{
    (void)source_object;
    (void)task_data;
    // TODO: implement cancellable
    (void)cancellable;
    LoomActiveThread* active_thread = g_task_get_task_data(thread);

    GError* error = NULL;
    // TODO: progress
    // If you want progress, do it manually in the worker
    gpointer result =
      active_thread->spec->shuttle(active_thread->spec->shuttle_data, &error);

    if (active_thread->timeout_id)
        g_source_remove(active_thread->timeout_id);

    if (error)
        g_task_return_error(thread, error);
    else
        g_task_return_pointer(thread, result, NULL);
    //  GTask will free on its own, no need to g_object_unref(task)
}

/**
 * Weaves the given active_thread into the Loom pool.
 * The active_thread is freed when the thread is tied off.
 * If the thread times out, it is snapped and tied off.
 * Caller must hold loom->lock.
 */
static void
loom_weave(Loom* loom, LoomThreadSpec* thread_spec)
{
    // Create the thread
    LoomActiveThread* active_thread = g_new0(LoomActiveThread, 1);
    active_thread->spec = thread_spec;
    active_thread->owning_loom = loom;

    active_thread->snippable = g_cancellable_new();
    active_thread->thread =
      g_task_new(NULL, active_thread->snippable, loom_tie_off, active_thread);

    // start timeout
    if (thread_spec->timeout_ms > 0) {
        active_thread->timeout_id = g_timeout_add(
          thread_spec->timeout_ms, loom_thread_snapped, active_thread);
    }

    g_task_set_task_data(active_thread->thread, active_thread, NULL);

    g_debug("Weaving thread '%s'\n", active_thread->spec->tag);
    g_hash_table_insert(
      loom->running_threads, g_strdup(active_thread->spec->tag), active_thread);

    // pass thread off to shuttle
    g_task_run_in_thread(active_thread->thread, loom_shuttle_wrapper);
}

/**
 * Picks up threads from the queued_threads hash table.
 * If a thread is ready, it is weaved into the Loom pool.
 * Caller must hold loom->lock.
 */
static void
loom_pick_up_ready(Loom* loom)
{
    g_debug("loom_pick_up_ready\n");
    // walk queue to find ready threads
    GList* iter = loom->queued_threads->head;
    while (iter) {
        GList* next = iter->next;
        LoomThreadSpec* thread_spec = iter->data;

        // check weather all dependencies are done
        gboolean ready = TRUE;
        for (const char** dep = thread_spec->dependencies; dep && *dep; ++dep) {
            if (g_hash_table_contains(loom->running_threads, *dep)) {
                ready = FALSE;
                g_debug("thread '%s' not ready\n", thread_spec->tag);
                break;
            }
        }
        if (ready) {
            g_debug("weaving ready thread '%s'\n", thread_spec->tag);
            g_queue_delete_link(loom->queued_threads, iter);
            loom_weave(loom, thread_spec);
        }
        iter = next;
    }
}

/**
 * Ties off the thread with the knot and knot_data from the LoomThreadSpec.
 * Picks up the next ready thread and weaves it into the Loom pool.
 */
static void
loom_tie_off(GObject* source, GAsyncResult* result, gpointer tie_off_data)
{
    (void)source;
    LoomActiveThread* active_thread = tie_off_data;
    Loom* loom = active_thread->owning_loom;
    GTask* thread = G_TASK(result);
    GError* error = NULL;
    gpointer result_pointer = g_task_propagate_pointer(thread, &error);
    // gpointer shuttle_data = g_task_get_task_data(thread)->spec->shuttle_data;
    gpointer shuttle_data = active_thread->spec->shuttle_data;

    if (active_thread->spec->knot)
        active_thread->spec->knot(
          active_thread->spec->knot_data, shuttle_data, result_pointer, error);

    g_mutex_lock(&loom->lock);
    g_hash_table_remove(loom->running_threads,
                        (gpointer)active_thread->spec->tag);
    // TODO: implement completed_tags (or remove it)
    // g_hash_table_insert(loom->completed_tags,
    // g_strdup(active_thread->spec->tag),GINT_TO_POINTER(TRUE));

    loom_pick_up_ready(loom);
    g_mutex_unlock(&loom->lock);

    if (active_thread->spec->task_data_destroy)
        active_thread->spec->task_data_destroy(
          active_thread->spec->shuttle_data);

    g_clear_object(&active_thread->thread);
    g_clear_object(&active_thread->snippable);
    free_loom_active_thread(active_thread);
}

/**
 * Returns TRUE if spec has a higher priority than compare_spec.
 * If spec and compare_thread have the same priority, returns FALSE.
 */
static gboolean
thread_has_priority(const LoomThreadSpec* spec,
                    const LoomThreadSpec* compare_spec)
{
    g_debug("Comparing priority of thread '%s' with '%s'\n",
            spec->tag,
            compare_spec->tag);
    // if LIFO, return TRUE as soon as we have a thread with same priority
    if (spec->is_lifo && spec->priority == compare_spec->priority) {
        g_debug("LiFo thread '%s' has prio over older thread '%s'\n",
                spec->tag,
                compare_spec->tag);
        return TRUE;
    }
    // lower value is higher priority
    return spec->priority < compare_spec->priority;
}

/**
 * Deep-copies a LoomThreadSpec.
 * Returns a pointer to the copy.
 */
static LoomThreadSpec*
loom_thread_spec_dup(const LoomThreadSpec* spec)
{
    LoomThreadSpec* spec_copy = g_memdup2(spec, sizeof(LoomThreadSpec));
    spec_copy->tag = g_strdup(spec->tag);
    spec_copy->dependencies =
      (const gchar**)g_strdupv((gchar**)spec->dependencies);
    return spec_copy;
}

/* Public API */

gboolean
loom_is_busy(Loom* loom)
{
    return g_hash_table_size(loom->running_threads) >= loom->max_threads;
}

void
loom_queue_thread(Loom* loom,
                  const LoomThreadSpec* thread_spec,
                  GCancellable** out_cancellable)
{
    // TODO: implement cancellable
    (void)out_cancellable;
    g_mutex_lock(&loom->lock);

    // check if thread has to wait for dependencies
    gboolean has_dependencies_running = FALSE;
    if (thread_spec->dependencies) {
        for (const gchar** dep = thread_spec->dependencies; *dep; ++dep) {
            if (!thread_spec->tag) {
                g_warning("loom_queue_thread: thread_spec->tag is NULL!");
                g_mutex_unlock(&loom->lock);
                return;
            }
            if (!loom->running_threads) {
                g_warning("loom_pick_up_ready: loom->running_threads is NULL!");
                g_mutex_unlock(&loom->lock);
                return;
            }
            if (!*dep)
                g_warning("loom_queue_thread: dep is NULL!");
            g_debug("Checking dependency '%s'\n", *dep);
            if (g_hash_table_contains(loom->running_threads, *dep)) {
                has_dependencies_running = TRUE;
                g_debug("thread %s has dependencies\n", thread_spec->tag);
                break;
            }
        }
    }

    // if so, push to queue
    if (has_dependencies_running) {
        g_debug("pushing thread '%s' to queue\n", thread_spec->tag);
        LoomThreadSpec* thread_spec_copy = loom_thread_spec_dup(thread_spec);
        GQueue* queue = loom->queued_threads;
        guint i = 0;
        if (queue->length == 0)
            g_queue_push_tail(queue, thread_spec_copy);
        else {
            g_debug("queue length: %d\n", queue->length);
            for (; i < queue->length; ++i) {
                LoomThreadSpec* spec = g_queue_peek_nth(queue, i);
                g_debug("i: %d, spec: %s\n", i, spec->tag);
                if (thread_has_priority(thread_spec_copy, spec)) {
                    g_queue_insert_before(
                      queue, g_queue_peek_nth_link(queue, i), thread_spec_copy);
                    break;
                }
            }
        }
        g_mutex_unlock(&loom->lock);
        return;
    }

    g_debug("no dependencies, weaving thread '%s'\n", thread_spec->tag);
    // if not, weave hard copy (in case thread_spec lives on stack)
    loom_weave(loom, loom_thread_spec_dup(thread_spec));
    g_mutex_unlock(&loom->lock);
}

// TODO: test this
void
loom_snip(Loom* loom, const char* tag)
{
    g_mutex_lock(&loom->lock);
    LoomActiveThread* active_thread =
      g_hash_table_lookup(loom->running_threads, tag);
    if (active_thread && active_thread->snippable)
        g_cancellable_cancel(active_thread->snippable);
    g_mutex_unlock(&loom->lock);
}

void
loom_disassemble(Loom* loom)
{
    g_thread_pool_free(loom->pool, FALSE, TRUE);
    g_hash_table_destroy(loom->running_threads);
    g_hash_table_destroy(loom->completed_tags);
    // TODO: if quitting mid processing, the queued threads need to be freed
    while (!g_queue_is_empty(loom->queued_threads)) {
        LoomThreadSpec* spec = g_queue_pop_head(loom->queued_threads);
        free_loom_thread_spec(spec);
    }
    g_queue_free(loom->queued_threads);
    g_mutex_clear(&loom->lock);
}

Loom*
loom_new(gint max_threads)
{
    Loom* loom = g_new0(Loom, 1);
    if (!loom->pool) {
        if (max_threads <= 0)
            max_threads = MAX(2, g_get_num_processors() - 1);
        loom->max_threads = max_threads;
        loom->pool = g_thread_pool_new(
          (GFunc)g_task_run_in_thread, NULL, max_threads, FALSE, NULL);

        loom->running_threads =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        loom->completed_tags =
          g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        loom->queued_threads = g_queue_new();

        g_mutex_init(&loom->lock);
    }
    // TODO: error
    return loom;
}

LoomThreadSpec
loom_thread_spec_default(void)
{
    return (LoomThreadSpec){ .tag = "",
                             .dependencies = NULL,
                             .priority = 0,
                             .timeout_ms = 0,
                             .shuttle = NULL,
                             .shuttle_data = NULL,
                             .knot = NULL,
                             .knot_data = NULL,
                             .progress = NULL,
                             .task_data_destroy = NULL,
                             .is_lifo = FALSE };
}

Loom*
loom_get_default(void)
{
    if (!global_loom)
        global_loom = loom_new(0);
    return global_loom;
}
