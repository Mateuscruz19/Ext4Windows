#pragma once

#include <cstdio>
#include <cstdarg>

// Global debug flag — when true, debug messages are printed.
// Set via --debug CLI flag or interactive mode.
inline bool g_debug = false;

// Optional debug log file — used by the server (which has no console).
// When set, debug output goes to this file instead of stderr.
inline FILE* g_debug_file = nullptr;

// Print a debug message (only when g_debug is true).
// Outputs to g_debug_file if set, otherwise stderr.
inline void dbg(const char* fmt, ...)
{
    if (!g_debug)
        return;

    FILE* out = g_debug_file ? g_debug_file : stderr;
    fprintf(out, "[DEBUG] ");

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
}
