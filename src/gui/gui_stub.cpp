// Console-only GUI fallback used by the portable cross-build.
// The normal MSVC/CMake build continues to compile gui_main.cpp.

#include "gui_main.hpp"
#include "../interactive.hpp"

int gui_main()
{
    return interactive_main();
}
