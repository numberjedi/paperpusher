#define G_LOG_DOMAIN "search"

#include "search.h"
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/**
 * tokenize @query into lowercased keywords
 */
static gint
tokenize_query(const gchar* query,
               gchar keywords[MAX_KEYWORDS][MAX_KEYWORD_LEN])
{
    g_autofree gchar* lower_query =
      g_utf8_strdown(query, -1); // freed on function return
    gint count = 0;
    const gchar* pointer = lower_query;

    // contains some magic to make it utf8 compatible
    // breaks on '\0'
    while (*pointer && count < MAX_KEYWORDS) {
        while (*pointer && g_unichar_isspace(g_utf8_get_char(pointer)))
            pointer = g_utf8_next_char(pointer); // skip whitespace
        gint len = 0;
        gint char_len = g_utf8_validate(pointer, -1, NULL)
                          ? g_utf8_next_char(pointer) - pointer
                          : 1;
        // get new utf8 char (breaks on '\0')
        while (*pointer && !g_unichar_isspace(g_utf8_get_char(pointer))) {
            if (len + char_len < MAX_KEYWORD_LEN - 1) { // enough space?
                memcpy(&keywords[count][len],           // lives on stack
                       pointer,
                       char_len); // copy bytes of utf8 char into keyword
                len += char_len;
                pointer += char_len;
            } else
                break;
        }

        // Null-terminate if something was read
        if (len > 0) {
            keywords[count][len] = '\0';
            count++;
        }
    }
    return count;
}

/**
 * lowercase substring match
 */
static bool
contains_keyword(const gchar* field, const gchar* keyword)
{
    if (!field || !keyword)
        return FALSE;

    g_autofree gchar* lower_field =
      g_utf8_strdown(field, -1); // freed on function return
    bool found = strstr(lower_field, keyword) != NULL;
    return found;
}

/**
 * simple relevance score
 */
static gint
score_paper(const Paper* paper,
            gchar keywords[MAX_KEYWORDS][MAX_KEYWORD_LEN],
            gint kw_count)
{
    gint score = 0;
    // printf("Scoring paper: %s...", paper->title ? paper->title : "NULL
    // title");
    gchar yearbuf[12];
    g_snprintf(yearbuf, sizeof(yearbuf), "%d", paper->year);

    for (int i = 0; i < kw_count; ++i) {
        int preScore = score;
        char* kw = keywords[i];
        if (contains_keyword(paper->title, kw))
            score += 5 * strlen(kw);
        if (contains_keyword(paper->abstract, kw))
            score += 1 * strlen(kw);
        if (contains_keyword(paper->arxiv_id, kw) ||
            contains_keyword(paper->doi, kw))
            score += 10 * strlen(kw);
        // year int to string (maybe just store as string in the first place?
        if (contains_keyword(yearbuf, kw))
            score += 10 * strlen(kw);
        for (int j = 0; j < paper->authors_count; j++) {
            if (paper->authors[j] && contains_keyword(paper->authors[j], kw))
                score += 10 * strlen(kw);
        }
        for (int j = 0; j < paper->keyword_count; ++j)
            if (contains_keyword(paper->keywords[j], keywords[i]))
                score += 3 * strlen(kw);
        if (preScore == score) {
            score = 0;
            break;
        }
    }
    // printf(" score: %i\n", score);
    return score;
}

typedef struct
{
    const Paper* paper;
    gint score;
} ScoredResult;

/**
 * comparison for sort
 */
static gint
compare_results(gconstpointer a, gconstpointer b)
{
    const ScoredResult* ra = *(const ScoredResult* const*)a;
    const ScoredResult* rb = *(const ScoredResult* const*)b;
    return rb->score - ra->score;
}

/**
 * search & rank by relevance to @query
 */
gint
search_papers(PaperDatabase* db,
              gint paper_count,
              const gchar* query,
              const Paper** results,
              gint max_results)
{
    gchar keywords[MAX_KEYWORDS][MAX_KEYWORD_LEN];
    gint kw_count = tokenize_query(query, keywords);

    // growable ptr array to be auto cleaned up with g_free()
    GPtrArray* array = g_ptr_array_new_with_free_func(g_free);
    gint limit = 0;

    //WITH_DB_READ_LOCK(db, {
        Paper** papers = db->papers;

        for (int i = 0; i < paper_count; ++i) {
            gint score = score_paper(papers[i], keywords, kw_count);
            if (score > 0) {
                ScoredResult* sr = g_new(ScoredResult, 1); // freed before return
                sr->paper = papers[i];
                sr->score = score;
                g_ptr_array_add(array, sr); // christ, glib arrays are nice
            }
        }

        g_ptr_array_sort(array, compare_results); // (wraps qsort)
        limit =
          MIN(array->len, (guint)max_results); // only return `limit` results
        for (int i = 0; i < limit; ++i) {
            ScoredResult* sr = g_ptr_array_index(array, i);
            results[i] = sr->paper;
        }
    //});

    g_ptr_array_free(array, TRUE); // free the array and the ScoredResults

    return limit;
}
