#pragma once

// Interactive shell: the main user interface when the exe is
// double-clicked (no arguments). Shows a menu with options to
// mount, unmount, scan, see status, etc. Communicates with
// the background server via named pipe.
//
// This is the "friendly" mode for non-technical users who
// don't want to type CLI commands.
int interactive_main();
