#define G_LOG_DOMAIN "pdf"

#include "pdf_viewer.h"
#include "cairo.h"
#include "glib-object.h"
#include "glibconfig.h"
#include "loom.h"
#include "poppler-document.h"
#include "poppler-page.h"
#include <glib.h>
#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <poppler.h>
#include <stdbool.h>

struct _PdfViewer
{
    PopplerDocument* doc;
    GtkWidget* drawing_area;
    int n_pages;

    // geometry & scaling
    double page_width_pts;  // Unscaled Poppler page width (points)
    double page_height_pts; // Unscaled Poppler page height (points)
    double scale;           // Current scale factor for rendering
    double page_height_px;  // Page height in pixels (scaled)

    // scrolling
    GtkAdjustment* vadjustment;

    // cache
    GHashTable* page_cache; // keys: page numbers (gpointer), values: GdkPixbuf*
    GHashTable* rendering_pages; // keys: page numbers (gpointer), values: dummy
};

typedef struct
{
    int page;
    int dist;
} PageDist;

static PdfViewer* pdf_viewer = NULL;
static Loom* gui_loom = NULL;
static GMutex poppler_render_mutex;

PdfViewer*
pdf_viewer_get_global(void)
{
    return pdf_viewer;
}

static int
page_dist_compare_descending(const void* a, const void* b)
{
    const PageDist* pa = a;
    const PageDist* pb = b;
    return pb->dist - pa->dist;
}

// trim cache when it gets too large
static void
pdf_cache_trim(PdfViewer* viewer, int first, int last, int max_cache)
{
    if (!viewer->page_cache)
        return;
    GList* keys =
      g_hash_table_get_keys(viewer->page_cache); // freed before return
    int n_cached = g_list_length(keys);
    if (n_cached <= max_cache) {
        g_list_free(keys);
        return;
    }
    // Build a list of (page, distance)
    GArray* page_dists =
      g_array_new(FALSE, FALSE, sizeof(PageDist)); // freed before return
    for (GList* l = keys; l; l = l->next) {
        int page = GPOINTER_TO_INT(l->data);
        int dist = 0;
        if (page < first)
            dist = first - page;
        else if (page > last)
            dist = page - last;
        g_array_append_val(page_dists, ((PageDist){ page, dist }));
    }
    // Sort by descending distance (furthest first)
    g_array_sort(page_dists, page_dist_compare_descending);

    // Remove furthest until at most max_cache remain
    int to_remove = n_cached - max_cache;
    for (int i = 0; i < to_remove; ++i) {
        int page = g_array_index(page_dists, PageDist, i).page;
        g_hash_table_remove(viewer->page_cache, GINT_TO_POINTER(page));
    }
    g_array_free(page_dists, TRUE);
    g_list_free(keys);
}

typedef struct
{
    PopplerDocument* doc;
    int page_num;
    double scale;
    double width_pts;
    double height_pts;
} RenderTaskData;

/**
 * Renders a page to a cairo surface.
 * Returns a pointer to the surface on success, NULL on error.
 * Caller owns the returned surface.
 */
static gpointer
render_page_shuttle(gpointer shuttle_data, GError** error)
{
    RenderTaskData* data = shuttle_data;

    if (!data->doc) {
        *error = g_error_new_literal(
          G_IO_ERROR, G_IO_ERROR_FAILED, "Document is NULL");
        return NULL;
    }

    PopplerPage* page =
      poppler_document_get_page(data->doc,
                                data->page_num); // unref'd before return
    if (!page) {
        *error =
          g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Page not found");
        return NULL;
    }

    int w = (int)(data->width_pts * data->scale + 0.5);
    int h = (int)(data->height_pts * data->scale + 0.5);

    cairo_surface_t* surface = // freed by g_hash_table_destroy()
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = cairo_create(surface); // freed before return

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_scale(cr, data->scale, data->scale);
    g_mutex_lock(&poppler_render_mutex); // sadly has to be serialized
    g_debug("poppler rendering page %d \n", data->page_num);
    poppler_page_render(page, cr);
    g_mutex_unlock(&poppler_render_mutex);

    cairo_destroy(cr);
    g_object_unref(page);

    return surface;
}

/**
 * Called when a page has been rendered.
 * Inserts the surface into the page cache and redraws the widget.
 */
static void
render_page_knot(gpointer knot_data,
                 gpointer shuttle_data,
                 gpointer result,
                 GError* error)
{
    PdfViewer* viewer = knot_data;
    RenderTaskData* data = shuttle_data;
    cairo_surface_t* surface = result; // we own this
    // only insert into cache if the document hasn't changed
    if (viewer->doc == data->doc && (!error || result)) {
        g_hash_table_insert(viewer->page_cache, // takes ownership
                            GINT_TO_POINTER(data->page_num),
                            surface); // freed by g_hash_table_destroy()
        // redraw
        gtk_widget_queue_draw(viewer->drawing_area);
        // mark as done
        g_debug("marking page %d as done\n", data->page_num);
        g_hash_table_remove(viewer->rendering_pages,
                            GINT_TO_POINTER(data->page_num));
    } else { // discard surface
        cairo_surface_destroy(surface);
    }

    g_object_unref(
      data->doc); // done with the doc, so unref it (should free it)
    g_free(data);
}

/**
 * Pushes a page to the render queue.
 */
static void
pdf_render_enqueue(PdfViewer* viewer, int page_num)
{
    // set rendering flag
    g_hash_table_add(viewer->rendering_pages, GINT_TO_POINTER(page_num));
    // retain document -- use mutex if needed
    PopplerDocument* doc = viewer->doc;
    if (!doc)
        return;
    g_object_ref(doc);

    RenderTaskData* data = g_new0(RenderTaskData, 1); // freed by knot
    data->doc = doc;
    data->page_num = page_num;
    data->scale = viewer->scale;
    data->width_pts = viewer->page_width_pts;
    data->height_pts = viewer->page_height_pts;

    LoomThreadSpec spec = loom_thread_spec_default(); // on stack
    spec.tag = "pdf-page-render";
    spec.priority = -1; // could vary this by distance to viewport
    spec.shuttle = render_page_shuttle;
    spec.shuttle_data = data;
    spec.knot = render_page_knot;
    spec.knot_data = viewer;
    spec.is_lifo = TRUE;
    static const gchar* deps[] = { "pdf-page-render", NULL };
    spec.dependencies = deps;
    // use gui_loom if default loom is busy
    loom_queue_thread(gui_loom, &spec, NULL);
}

// Draw callback
static bool
on_pdf_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
    PdfViewer* viewer = (PdfViewer*)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    // if nothing to draw, show a message
    if (!viewer || !viewer->doc || viewer->n_pages == 0) {
        const gchar* msg = "Select a PDF to preview it";
        PangoLayout* layout =
          gtk_widget_create_pango_layout(widget, msg); // freed before return

        // style the message according to the current theme
        // color
        GtkStyleContext* context = gtk_widget_get_style_context(widget);
        GdkRGBA foreground;
        gtk_style_context_get_color(
          context, GTK_STATE_FLAG_NORMAL, &foreground);
        cairo_set_source_rgba(cr,
                              foreground.red,
                              foreground.green,
                              foreground.blue,
                              foreground.alpha);
        // font
        PangoFontDescription* desc = NULL;
          gtk_style_context_get(context, GTK_STATE_FLAG_NORMAL, "font", &desc, NULL);
        if (desc) {
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);
        }

        int lw, lh;
        pango_layout_get_size(layout, &lw, &lh);
        lw /= PANGO_SCALE;
        lh /= PANGO_SCALE;
        cairo_move_to(cr, (alloc.width - lw) / 2, (alloc.height - lh) / 2);
        pango_cairo_show_layout(cr, layout);
        g_object_unref(layout);
        return FALSE;
    }

    // Update scale and height in case window resized
    double scale = (double)alloc.width / viewer->page_width_pts;
    double page_height_px = viewer->page_height_pts * scale;
    viewer->scale = scale; // necessary?
    viewer->page_height_px = page_height_px;

    // fill background
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);

    // get scroll offset
    double scroll_y = gtk_adjustment_get_value(viewer->vadjustment);
    double viewport_height = alloc.height;

    // compute visible pages (w/ prefetch)
    int first_visible_page = MAX(0, (int)(scroll_y / page_height_px) - 1);
    int last_visible_page =
      MIN(viewer->n_pages - 1,
          (int)((scroll_y + viewport_height) / page_height_px) + 1);

    // invalidate cache if scale changes
    static double last_scale = 0.0;
    if (viewer->page_cache && last_scale != scale) {
        g_hash_table_destroy(viewer->page_cache);
        viewer->page_cache = NULL;
    }
    last_scale = scale;

    // ensure page_cache is allocated
    if (!viewer->page_cache) {
        viewer->page_cache = // freed by pdf_viewer_destroy()
          g_hash_table_new_full(g_direct_hash,
                                g_direct_equal,
                                NULL,
                                (GDestroyNotify)cairo_surface_destroy);
    }

    // draw & cache all visible pages asynchronously
    for (int i = first_visible_page; i <= last_visible_page; ++i) {
        cairo_surface_t* surface =
          g_hash_table_lookup(viewer->page_cache, GINT_TO_POINTER(i));
        // render page if not in cache or rendering
        if (!surface) {
            if (!g_hash_table_contains(viewer->rendering_pages,
                                       GINT_TO_POINTER(i))) {
                // push page to render queue
                g_debug("pushing page %d to render queue\n", i);
                pdf_render_enqueue(viewer, i);
            }
        } else { // if in cache, paint
            double y_offset = i * page_height_px - scroll_y;

            cairo_save(cr);
            cairo_set_source_surface(cr, surface, 0, y_offset);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }

    // trim cache if too big
    pdf_cache_trim(
      viewer, first_visible_page, last_visible_page, PDF_CACHE_MAX_PAGES);

    return FALSE;
}

static void
on_scroll_value_changed(GtkAdjustment* adj, gpointer data)
{
    (void)adj;
    PdfViewer* viewer = (PdfViewer*)data;
    gtk_widget_queue_draw(viewer->drawing_area);
}

static void
on_area_size_allocate(GtkWidget* widget,
                      GdkRectangle* alloc,
                      gpointer user_data)
{
    PdfViewer* viewer = (PdfViewer*)user_data;
    if (!viewer->vadjustment)
        return;

    // Resize drawing area to always match viewport
    gtk_widget_set_size_request(widget, -1, alloc->height);

    // Update the adjustment page size
    gtk_adjustment_set_page_size(viewer->vadjustment, alloc->height);

    // Optionally, clamp scroll position if at end
    double max = gtk_adjustment_get_upper(viewer->vadjustment) - alloc->height;
    if (gtk_adjustment_get_value(viewer->vadjustment) > max)
        gtk_adjustment_set_value(viewer->vadjustment, max);

    // Force redraw
    gtk_widget_queue_draw(widget);
}

static bool
on_pdf_scroll_event(GtkWidget* widget,
                    GdkEventScroll* event,
                    gpointer user_data)
{
    (void)widget;
    PdfViewer* viewer = (PdfViewer*)user_data;
    GtkAdjustment* vadj = viewer->vadjustment;
    double step = gtk_adjustment_get_step_increment(vadj);
    // double page = gtk_adjustment_get_page_increment(vadj);

    if (event->direction == GDK_SCROLL_UP)
        pdf_viewer_scroll_by(-step);
    else if (event->direction == GDK_SCROLL_DOWN)
        pdf_viewer_scroll_by(step);
    else if (event->direction == GDK_SCROLL_SMOOTH && event->delta_y != 0.0)
        pdf_viewer_scroll_by(event->delta_y * step);

    return TRUE;
}

/**
 * Frees the page_cache and rendering_pages hash tables.
 */
static void
free_hash_tables(PdfViewer* viewer)
{
    g_debug("freeing hash tables\n");
    if (viewer->page_cache) {
        g_hash_table_destroy(viewer->page_cache);
        viewer->page_cache = NULL;
    }
    if (viewer->rendering_pages) {
        g_hash_table_destroy(viewer->rendering_pages);
        viewer->rendering_pages = NULL;
    }
}

/* Public API */

/**
 * Sets up the PDF viewer, i.e. the drawing area and scrollbar.
 *
 * @param builder The GtkBuilder object containing the UI elements.
 * @param scrollbar_id The ID of the scrollbar widget.
 * @param drawing_area_id The ID of the drawing area widget.
 */
void
pdf_viewer_setup(Loom* loom,
                 GtkApplication* app,
                 GtkBuilder* builder,
                 const gchar* scrollbar_id,
                 const gchar* drawing_area_id)
{
    gui_loom = loom;
    GtkWidget* scrollbar = // freed when builder is destroyed
      GTK_WIDGET(gtk_builder_get_object(builder, scrollbar_id));
    GtkWidget* area = // freed when builder is destroyed
      GTK_WIDGET(gtk_builder_get_object(builder, drawing_area_id));
    g_return_if_fail(GTK_IS_DRAWING_AREA(area));
    g_return_if_fail(GTK_IS_SCROLLBAR(scrollbar));

    GtkAdjustment* vadj =
      gtk_adjustment_new(0.0, 0.0, 1000.0, 20.0, 200.0, 200.0);
    gtk_range_set_adjustment(GTK_RANGE(scrollbar), vadj);

    pdf_viewer = g_new0(PdfViewer, 1); // destroyed on shutdown
    pdf_viewer->doc = NULL;
    pdf_viewer->n_pages = 0;
    pdf_viewer->page_width_pts = 0.0;
    pdf_viewer->page_height_pts = 0.0;
    pdf_viewer->scale = 1.0;
    pdf_viewer->page_height_px = 0.0;
    pdf_viewer->drawing_area = area;
    pdf_viewer->vadjustment = vadj;
    // cache
    pdf_viewer->page_cache = // freed by pdf_viewer_destroy()
      g_hash_table_new_full(g_direct_hash,
                            g_direct_equal,
                            NULL,
                            (GDestroyNotify)cairo_surface_destroy);
    pdf_viewer->rendering_pages = // freed by pdf_viewer_destroy()
      g_hash_table_new(g_direct_hash, g_direct_equal);

    gtk_widget_set_can_focus(area, TRUE);
    gtk_widget_add_events(area, GDK_SCROLL_MASK);

    g_signal_connect(area, "draw", G_CALLBACK(on_pdf_draw), pdf_viewer);
    g_signal_connect(
      area, "size-allocate", G_CALLBACK(on_area_size_allocate), pdf_viewer);
    // mouse scroll
    g_signal_connect(
      area, "scroll-event", G_CALLBACK(on_pdf_scroll_event), pdf_viewer);
    // redraw when scrolled
    g_signal_connect(pdf_viewer->vadjustment,
                     "value-changed",
                     G_CALLBACK(on_scroll_value_changed),
                     pdf_viewer);
    g_signal_connect(
      app, "shutdown", G_CALLBACK(pdf_viewer_destroy), pdf_viewer);
}

void
pdf_viewer_destroy(GApplication* app, gpointer user_data)
{
    (void)app;
    g_debug("freeing PdfViewer\n");
    PdfViewer* viewer = (PdfViewer*)user_data;
    free_hash_tables(viewer);
    // g_object_unref(viewer->doc);
    g_free(viewer);
}

// PdfViewer*
// pdf_viewer_get(GtkWidget* drawing_area)
// {
//     return (PdfViewer*)g_object_get_data(G_OBJECT(drawing_area),
//     "pdf_viewer");
// }

void
pdf_viewer_load(const gchar* filepath)
{
    if (!pdf_viewer)
        return;

    // Clean up previous doc/page
    if (pdf_viewer->doc) {
        g_object_unref(pdf_viewer->doc);
        pdf_viewer->doc = NULL;
    }
    if (pdf_viewer->page_cache) {
        g_hash_table_destroy(pdf_viewer->page_cache);
        pdf_viewer->page_cache = NULL;
    }
    if (pdf_viewer->rendering_pages) {
        g_hash_table_destroy(pdf_viewer->rendering_pages);
        pdf_viewer->rendering_pages = NULL;
    }

    // clean up cache
    pdf_viewer->n_pages = 0;
    pdf_viewer->page_width_pts = 0.0;
    pdf_viewer->page_height_pts = 0.0;
    pdf_viewer->scale = 1.0;
    pdf_viewer->page_height_px = 0.0;

    if (!filepath) {
        gtk_widget_set_size_request(
          pdf_viewer->drawing_area, -1, 0); // no doc loaded
        gtk_widget_queue_draw(pdf_viewer->drawing_area);
        return;
    }

    GError* error = NULL;
    gchar* uri =
      g_filename_to_uri(filepath, NULL, &error); // freed before return
    if (!uri) {
        g_warning("Invalid file path: %s", filepath);
        if (error)
            g_clear_error(&error);
        gtk_widget_set_size_request(pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(pdf_viewer->drawing_area);
        return;
    }

    pdf_viewer->doc = poppler_document_new_from_file(
      uri, NULL, &error); // freed by pdf_viewer_destroy()
    g_free(uri);

    if (!pdf_viewer->doc) {
        g_warning("Failed to open PDF '%s': %s",
                  filepath,
                  error ? error->message : "<unknown>");
        if (error)
            g_clear_error(&error);
        gtk_widget_set_size_request(pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(pdf_viewer->drawing_area);
        return;
    }

    pdf_viewer->n_pages =
      poppler_document_get_n_pages(pdf_viewer->doc); // page count
    PopplerPage* page0 =
      poppler_document_get_page(pdf_viewer->doc, 0); // unref'd before return
    if (!page0) {
        g_warning("PDF '%s' has no pages", filepath);
        g_object_unref(pdf_viewer->doc);
        pdf_viewer->doc = NULL;
        pdf_viewer->n_pages = 0;
        gtk_widget_set_size_request(pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(pdf_viewer->drawing_area);
        return;
    }

    double pw_pts, ph_pts;
    poppler_page_get_size(page0, &pw_pts, &ph_pts);
    pdf_viewer->page_width_pts = pw_pts;
    pdf_viewer->page_height_pts = ph_pts;
    g_object_unref(page0);

    // determine scale for fit to width
    int widget_width = gtk_widget_get_allocated_width(pdf_viewer->drawing_area);
    // fallback to 800
    if (widget_width <= 1)
        widget_width = 800;
    pdf_viewer->scale = widget_width / pw_pts;
    pdf_viewer->page_height_px = ph_pts * pdf_viewer->scale;

    double doc_height = pdf_viewer->n_pages * pdf_viewer->page_height_px;

    // set drawing area height to viewport only
    int viewport_height =
      gtk_widget_get_allocated_height(pdf_viewer->drawing_area);
    if (viewport_height < 1)
        viewport_height = 800;

    // Reset scroll position to top
    GtkAdjustment* vadj = pdf_viewer->vadjustment;
    gtk_adjustment_set_upper(vadj, doc_height);
    gtk_adjustment_set_page_size(vadj, viewport_height);
    gtk_adjustment_set_value(vadj, 0.0); // jump to top (optional)
    gtk_adjustment_set_step_increment(vadj, pdf_viewer->page_height_px * 0.15);
    gtk_adjustment_set_page_increment(vadj, pdf_viewer->page_height_px * 0.25);

    // re-init caches
    free_hash_tables(pdf_viewer);
    pdf_viewer->page_cache = // freed by pdf_viewer_destroy()
      g_hash_table_new_full(g_direct_hash,
                            g_direct_equal,
                            NULL,
                            (GDestroyNotify)cairo_surface_destroy);
    pdf_viewer->rendering_pages = // freed by pdf_viewer_destroy()
      g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    gtk_widget_queue_draw(pdf_viewer->drawing_area);
}

void
pdf_viewer_scroll_by(double amount)
{
    if (!pdf_viewer || !pdf_viewer->vadjustment)
        return;
    GtkAdjustment* vadj = pdf_viewer->vadjustment;
    if (amount == 1.0 || amount == -1.0) {
        double page =
          gtk_adjustment_get_page_increment(pdf_viewer->vadjustment);
        amount = amount * page;
    }
    double val = gtk_adjustment_get_value(vadj);
    val += amount;
    if (val < gtk_adjustment_get_lower(vadj))
        val = gtk_adjustment_get_lower(vadj);
    double max =
      gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
    if (val > max)
        val = max;
    gtk_adjustment_set_value(vadj, val);
}
