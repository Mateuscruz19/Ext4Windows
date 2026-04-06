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

#include <windows.h>
#include <string>

// Resource ID for the embedded HTML — must match ext4windows.rc
#define IDR_GUI_HTML 100

// Load the GUI HTML from the embedded Win32 resource.
//
// The HTML file (assets/gui.html) is compiled into the .exe by the
// resource compiler (rc.exe) via the RCDATA directive in ext4windows.rc.
// This function extracts it at runtime using FindResourceW/LoadResource.
//
// Why embed as a resource instead of a C++ string literal?
// MSVC has a limit of ~16KB per string literal (error C2026).
// Our HTML is ~30KB, so we use the resource system instead.
// It's the same mechanism used for the app icon (IDI_APPICON).
//
// In Python terms: this is like pkg_resources.resource_string() —
// it reads a file that was bundled into the package at build time.
//
// Docs:
//   RCDATA: https://learn.microsoft.com/en-us/windows/win32/menurc/rcdata-resource
//   FindResource: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-findresourcew
//   LoadResource: https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadresource

inline std::wstring load_gui_html()
{
    // GetModuleHandleW(nullptr) returns a handle to the current .exe.
    // In Python: like __file__ pointing to the current script.
    HMODULE hModule = GetModuleHandleW(nullptr);

    // FindResourceW locates the resource by type and ID.
    // MAKEINTRESOURCEW(IDR_GUI_HTML) converts the numeric ID (100) to
    // the format that FindResourceW expects.
    // RT_RCDATA means "raw data" — just a blob of bytes, not an icon
    // or dialog or other structured resource type.
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_GUI_HTML), RT_RCDATA);
    if (!hRes) return L"<html><body><h1>Error: GUI resource not found</h1></body></html>";

    // LoadResource gets a handle to the resource data in memory.
    // Despite the name, it doesn't actually "load" anything — the
    // data is already mapped into the process's address space as
    // part of the .exe file. It just returns a handle to find it.
    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return L"<html><body><h1>Error: could not load GUI resource</h1></body></html>";

    // LockResource returns a pointer to the actual bytes.
    // The name is misleading — it doesn't actually "lock" anything.
    // It's just a pointer to the data in the exe's memory.
    const char* data = static_cast<const char*>(LockResource(hData));
    DWORD size = SizeofResource(hModule, hRes);

    if (!data || size == 0)
        return L"<html><body><h1>Error: empty GUI resource</h1></body></html>";

    // Convert from UTF-8 (the .html file) to wide string (what
    // WebView2's NavigateToString expects).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, nullptr, 0);
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, (int)size, result.data(), wlen);

    return result;
}
