// Ext4Windows — Mount ext4 partitions as Windows drive letters
// Copyright (C) 2026 Mateus Cruz
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see <https://www.gnu.org/licenses/>.

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
