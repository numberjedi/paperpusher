#pragma once

#include <glib.h>

typedef struct
{
    gchar* paperparser_path;
    gchar* cache_path;
    gchar* json_path;
    gboolean list;
    gchar** import_paths;
} AppFlags;

typedef struct
{
    gboolean version;
    gboolean debug;
    gboolean mock_data;
} DebugFlags;

extern AppFlags app_flags;
extern DebugFlags debug_flags;
extern const GOptionEntry cmd_options[];
