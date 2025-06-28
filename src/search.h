/* search.h */
#pragma once

#include "paper.h"
#include <glib.h>

#define MAX_KEYWORDS 20
#define MAX_KEYWORD_LEN 50

G_BEGIN_DECLS

/**
 * Search and rank papers by relevance to a keyword query.
 *
 * @param papers       Array of Paper pointers to search.
 * @param paper_count  Number of papers in the array.
 * @param query        User query string.
 * @param results      Output array to store matching Paper pointers.
 * @param max_results  Maximum size of the results array.
 * @return Number of results stored in 'results'.
 */
gint
search_papers(PaperDatabase* db,
              gint paper_count,
              const gchar* query,
              const Paper** results,
              gint max_results);

G_END_DECLS
