/* config.h */
#pragma once

/* Where we expect to find the paperparser binary, relative to the app dir */
#define PAPERPARSER_REL_PATH    "paperparser/paperparser"

/* Paperparser executable name */
#define PAPERPARSER_EXE_NAME	"paperparser"

/* Fallback environment variable name for parser override */
#define PAPERPARSER_ENVVAR      "PAPERPARSER_PATH"

/* Proc-fs link to our own executable (Linux only) */
#define SELF_EXE_PATH           "/proc/self/exe"

/* database files */
#define CACHE_PATH		"pp.cache"
#define JSON_PATH		"ppdb.json"
