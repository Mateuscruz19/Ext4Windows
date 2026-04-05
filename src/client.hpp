#pragma once

// Client entry point: parse subcommands and communicate with the server
// via named pipe. Auto-starts the server if it's not running.
//
// Subcommands:
//   ext4windows mount <path> <drive:> [--rw]
//   ext4windows unmount <drive:>
//   ext4windows status
//   ext4windows quit
int client_main(int argc, wchar_t* argv[]);
