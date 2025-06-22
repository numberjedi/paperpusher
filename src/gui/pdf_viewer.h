#pragma once

#include <glib.h>
#include <gtk/gtk.h>
#include <poppler.h>

#define PDF_CACHE_MAX_PAGES 50

// Opaque PDF viewer object associated with a GtkDrawingArea
typedef struct _PdfViewer PdfViewer;

/**
 * Initialize a PdfViewer on the drawing area with the given widget ID from the builder.
 * The viewer object is stored as data on the widget and cleaned up automatically.
 */
void pdf_viewer_setup(GtkBuilder *builder, const gchar* scrolled_window_id, const gchar *drawing_area_id);

/**
 * Load the first page of the PDF at filepath into the viewer.
 * Call this whenever the selected paper changes.
 */
void pdf_viewer_load(const gchar *filepath);

/**
 * return g_pdf_viewer
 */
PdfViewer* pdf_viewer_get_global(void);

/**
 * scroll the pdf by @amount
 * if @amount==1.0, "page down"
 * if @amount==-1.0, "page up"
 */
void pdf_viewer_scroll_by(double amount);
