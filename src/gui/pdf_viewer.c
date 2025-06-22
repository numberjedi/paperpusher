#include "pdf_viewer.h"
#include "cairo.h"
#include "glib-object.h"
#include "glibconfig.h"
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
};

typedef struct
{
    int page;
    int dist;
} PageDist;

static PdfViewer* g_pdf_viewer = NULL;

PdfViewer*
pdf_viewer_get_global(void)
{
    return g_pdf_viewer;
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
    GList* keys = g_hash_table_get_keys(viewer->page_cache);
    int n_cached = g_list_length(keys);
    if (n_cached <= max_cache) {
        g_list_free(keys);
        return;
    }
    // Build a list of (page, distance)
    GArray* page_dists = g_array_new(FALSE, FALSE, sizeof(PageDist));
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

// Draw callback
static bool
on_pdf_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
    PdfViewer* viewer = (PdfViewer*)user_data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);

    if (!viewer || !viewer->doc || viewer->n_pages == 0) {
        const gchar* msg = "Select a PDF to preview it";
        PangoLayout* layout = gtk_widget_create_pango_layout(widget, msg);
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
        viewer->page_cache =
          g_hash_table_new_full(g_direct_hash,
                                g_direct_equal,
                                NULL,
                                (GDestroyNotify)cairo_surface_destroy);
    }

    // draw & cache all visible pages
    for (int i = first_visible_page; i <= last_visible_page; ++i) {
        cairo_surface_t* surface =
          g_hash_table_lookup(viewer->page_cache, GINT_TO_POINTER(i));
        if (!surface) {
            PopplerPage* page = poppler_document_get_page(viewer->doc, i);
            if (!page)
                continue;
            int w = (int)(viewer->page_width_pts * scale + 0.5);
            int h = (int)(viewer->page_height_pts * scale + 0.5);

            surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
            cairo_t* page_cr = cairo_create(surface);

            cairo_set_source_rgb(page_cr, 1.0, 1.0, 1.0);
            cairo_paint(page_cr);

            cairo_scale(page_cr, scale, scale);
            poppler_page_render(page, page_cr);
            cairo_destroy(page_cr);
            g_object_unref(page);

            g_hash_table_insert(
              viewer->page_cache, GINT_TO_POINTER(i), surface);
        }

        double y_offset = i * page_height_px - scroll_y;

        cairo_save(cr);
        cairo_set_source_surface(cr, surface, 0, y_offset);
        cairo_paint(cr);
        cairo_restore(cr);
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
    //double val = gtk_adjustment_get_value(vadj);
    double step = gtk_adjustment_get_step_increment(vadj);
    double page = gtk_adjustment_get_page_increment(vadj);

    if (event->direction == GDK_SCROLL_UP)
        //val -= step;
        pdf_viewer_scroll_by(-step);
    else if (event->direction == GDK_SCROLL_DOWN)
        //val += step;
        pdf_viewer_scroll_by(step);
    else if (event->direction == GDK_SCROLL_SMOOTH && event->delta_y != 0.0)
        //val += event->delta_y * step;
        pdf_viewer_scroll_by(event->delta_y * step);

    return TRUE;
}

void
pdf_viewer_setup(GtkBuilder* builder,
                 const gchar* scrollbar_id,
                 const gchar* drawing_area_id)
{
    GtkWidget* scrollbar =
      GTK_WIDGET(gtk_builder_get_object(builder, scrollbar_id));
    GtkWidget* area =
      GTK_WIDGET(gtk_builder_get_object(builder, drawing_area_id));
    g_return_if_fail(GTK_IS_DRAWING_AREA(area));
    g_return_if_fail(GTK_IS_SCROLLBAR(scrollbar));

    GtkAdjustment* vadj =
      gtk_adjustment_new(0.0, 0.0, 1000.0, 20.0, 200.0, 200.0);
    gtk_range_set_adjustment(GTK_RANGE(scrollbar), vadj);

    g_pdf_viewer = g_new0(PdfViewer, 1);
    g_pdf_viewer->doc = NULL;
    g_pdf_viewer->n_pages = 0;
    g_pdf_viewer->page_width_pts = 0.0;
    g_pdf_viewer->page_height_pts = 0.0;
    g_pdf_viewer->scale = 1.0;
    g_pdf_viewer->page_height_px = 0.0;
    g_pdf_viewer->drawing_area = area;
    g_pdf_viewer->vadjustment = vadj;
    // cache
    g_pdf_viewer->page_cache =
      g_hash_table_new_full(g_direct_hash,
                            g_direct_equal,
                            NULL,
                            (GDestroyNotify)cairo_surface_destroy);

    gtk_widget_set_can_focus(area, TRUE);
    gtk_widget_add_events(area, GDK_SCROLL_MASK);

    g_signal_connect(area, "draw", G_CALLBACK(on_pdf_draw), g_pdf_viewer);
    g_signal_connect(
      area, "size-allocate", G_CALLBACK(on_area_size_allocate), g_pdf_viewer);
    // mouse scroll
    g_signal_connect(
      area, "scroll-event", G_CALLBACK(on_pdf_scroll_event), g_pdf_viewer);
    // redraw when scrolled
    g_signal_connect(g_pdf_viewer->vadjustment,
                     "value-changed",
                     G_CALLBACK(on_scroll_value_changed),
                     g_pdf_viewer);
}

PdfViewer*
pdf_viewer_get(GtkWidget* drawing_area)
{
    return (PdfViewer*)g_object_get_data(G_OBJECT(drawing_area), "pdf_viewer");
}

void
pdf_viewer_load(const gchar* filepath)
{
    if (!g_pdf_viewer)
        return;

    // Clean up previous doc/page
    if (g_pdf_viewer->doc) {
        g_object_unref(g_pdf_viewer->doc);
        g_pdf_viewer->doc = NULL;
    }
    if (g_pdf_viewer->page_cache) {
        g_hash_table_destroy(g_pdf_viewer->page_cache);
        g_pdf_viewer->page_cache = NULL;
    }

    // clean up cache
    g_pdf_viewer->n_pages = 0;
    g_pdf_viewer->page_width_pts = 0.0;
    g_pdf_viewer->page_height_pts = 0.0;
    g_pdf_viewer->scale = 1.0;
    g_pdf_viewer->page_height_px = 0.0;

    if (!filepath) {
        gtk_widget_set_size_request(
          g_pdf_viewer->drawing_area, -1, 0); // no doc loaded
        gtk_widget_queue_draw(g_pdf_viewer->drawing_area);
        return;
    }

    GError* error = NULL;
    gchar* uri = g_filename_to_uri(filepath, NULL, &error);
    if (!uri) {
        g_warning("Invalid file path: %s", filepath);
        if (error)
            g_clear_error(&error);
        gtk_widget_set_size_request(g_pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(g_pdf_viewer->drawing_area);
        return;
    }

    g_pdf_viewer->doc = poppler_document_new_from_file(uri, NULL, &error);
    g_free(uri);

    if (!g_pdf_viewer->doc) {
        g_warning("Failed to open PDF '%s': %s",
                  filepath,
                  error ? error->message : "<unknown>");
        if (error)
            g_clear_error(&error);
        gtk_widget_set_size_request(g_pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(g_pdf_viewer->drawing_area);
        return;
    }

    g_pdf_viewer->n_pages =
      poppler_document_get_n_pages(g_pdf_viewer->doc); // page count
    PopplerPage* page0 = poppler_document_get_page(g_pdf_viewer->doc, 0);
    if (!page0) {
        g_warning("PDF '%s' has no pages", filepath);
        g_object_unref(g_pdf_viewer->doc);
        g_pdf_viewer->doc = NULL;
        g_pdf_viewer->n_pages = 0;
        gtk_widget_set_size_request(g_pdf_viewer->drawing_area, -1, 0);
        gtk_widget_queue_draw(g_pdf_viewer->drawing_area);
        return;
    }

    double pw_pts, ph_pts;
    poppler_page_get_size(page0, &pw_pts, &ph_pts);
    g_pdf_viewer->page_width_pts = pw_pts;
    g_pdf_viewer->page_height_pts = ph_pts;
    g_object_unref(page0);

    // determine scale for fit to width
    int widget_width =
      gtk_widget_get_allocated_width(g_pdf_viewer->drawing_area);
    // fallback to 800
    if (widget_width <= 1)
        widget_width = 800;
    g_pdf_viewer->scale = widget_width / pw_pts;
    g_pdf_viewer->page_height_px = ph_pts * g_pdf_viewer->scale;

    double doc_height = g_pdf_viewer->n_pages * g_pdf_viewer->page_height_px;

    // set drawing area height to viewport only
    int viewport_height =
      gtk_widget_get_allocated_height(g_pdf_viewer->drawing_area);
    if (viewport_height < 1)
        viewport_height = 800;

    // Reset scroll position to top
    GtkAdjustment* vadj = g_pdf_viewer->vadjustment;
    gtk_adjustment_set_upper(vadj, doc_height);
    gtk_adjustment_set_page_size(vadj, viewport_height);
    gtk_adjustment_set_value(vadj, 0.0); // jump to top (optional)
    gtk_adjustment_set_step_increment(vadj, g_pdf_viewer->page_height_px * 0.15);
    gtk_adjustment_set_page_increment(vadj, g_pdf_viewer->page_height_px * 0.25);
    // gtk_adjustment_changed(vadj);

    gtk_widget_queue_draw(g_pdf_viewer->drawing_area);
}

void pdf_viewer_scroll_by(double amount) 
{
    if (!g_pdf_viewer || !g_pdf_viewer->vadjustment)
        return;
    GtkAdjustment* vadj = g_pdf_viewer->vadjustment;
    if (amount == 1.0 || amount == -1.0) {
        double page = gtk_adjustment_get_page_increment(g_pdf_viewer->vadjustment);
        amount = amount * page;
    }
    double val = gtk_adjustment_get_value(vadj);
    val += amount;
    if (val < gtk_adjustment_get_lower(vadj))
        val = gtk_adjustment_get_lower(vadj);
    double max = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
    if (val > max)
        val = max;
    gtk_adjustment_set_value(vadj, val);
}
