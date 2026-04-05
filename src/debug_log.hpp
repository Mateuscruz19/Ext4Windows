#pragma once

#include <cstdio>
#include <cstdarg>

// Global debug flag — when true, debug messages are printed to stderr.
// Set via --debug CLI flag or interactive mode.
inline bool g_debug = false;

// Print a debug message to stderr (only when g_debug is true).
// Works exactly like printf but prefixes with [DEBUG] and outputs to stderr.
inline void dbg(const char* fmt, ...)
{
    if (!g_debug)
        return;

    fprintf(stderr, "[DEBUG] ");

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}
