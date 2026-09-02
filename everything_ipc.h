// facet · everything_ipc.h — the Everything 1.4 IPC contract facet speaks: a QUERY2 goes in by
// WM_COPYDATA, a LIST2 comes back by WM_COPYDATA. Constants and layouts follow voidtools'
// public SDK header (everything_ipc.h) and the ES command-line source, which uses the same
// interface; only the subset facet needs is here and no SDK DLL is ever loaded.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

namespace facet::ipc {

constexpr const wchar_t* kWndClass = L"EVERYTHING_TASKBAR_NOTIFICATION";
constexpr DWORD kCopyDataQuery2W = 18;          // COPYDATASTRUCT.dwData of a Unicode QUERY2

// SendMessage(everything, WM_USER, cmd, lparam) probes
constexpr UINT kWmIpc = WM_USER;
constexpr WPARAM kGetMajorVersion = 0, kGetMinorVersion = 1, kGetRevision = 2, kGetBuildNumber = 3;
constexpr WPARAM kIsDbLoaded = 401, kIsDbBusy = 402, kIsFastSort = 410, kIsFileInfoIndexed = 411;
constexpr LPARAM kFileInfoSize = 1, kFileInfoFolderSize = 2, kFileInfoDateCreated = 3,
                 kFileInfoDateModified = 4, kFileInfoDateAccessed = 5, kFileInfoAttributes = 6;

// Query2.search_flags
constexpr DWORD kMatchCase = 0x1, kMatchWholeWord = 0x2, kMatchPath = 0x4, kRegex = 0x8, kMatchAccents = 0x10;

// Query2.sort_type
enum Sort : DWORD {
    NameAsc = 1, NameDesc = 2, PathAsc = 3, PathDesc = 4, SizeAsc = 5, SizeDesc = 6,
    ExtAsc = 7, ExtDesc = 8, TypeAsc = 9, TypeDesc = 10, CreatedAsc = 11, CreatedDesc = 12,
    ModifiedAsc = 13, ModifiedDesc = 14, AttrAsc = 15, AttrDesc = 16,
    FileListNameAsc = 17, FileListNameDesc = 18, RunCountAsc = 19, RunCountDesc = 20,
    RecentAsc = 21, RecentDesc = 22, AccessedAsc = 23, AccessedDesc = 24,
    DateRunAsc = 25, DateRunDesc = 26,
};

// Query2.request_flags — an item's data is laid out in exactly this bit order:
// strings as DWORD length + UTF-16 chars + NUL; sizes as LARGE_INTEGER; dates as FILETIME;
// attributes / run count as DWORD.
constexpr DWORD kReqName = 0x1, kReqPath = 0x2, kReqFullPath = 0x4, kReqExt = 0x8, kReqSize = 0x10,
                kReqCreated = 0x20, kReqModified = 0x40, kReqAccessed = 0x80, kReqAttr = 0x100,
                kReqFileListName = 0x200, kReqRunCount = 0x400, kReqDateRun = 0x800,
                kReqRecent = 0x1000, kReqHlName = 0x2000, kReqHlPath = 0x4000, kReqHlFull = 0x8000;

// Item2.flags
constexpr DWORD kItemFolder = 0x1, kItemDrive = 0x2;

struct Query2 {                     // followed by the NUL-terminated UTF-16 search string
    DWORD reply_hwnd;               // HWND truncated to 32 bits (Win32 HWNDs are 32-bit significant)
    DWORD reply_copydata_message;   // dwData Everything puts on the reply
    DWORD search_flags;
    DWORD offset;                   // first result index
    DWORD max_results;              // 0xFFFFFFFF = all
    DWORD request_flags;
    DWORD sort_type;
};
struct List2 {                      // followed by numitems Item2, then the item data
    DWORD totitems;                 // matches for the whole query
    DWORD numitems;                 // items in this reply
    DWORD offset;
    DWORD request_flags;            // what the reply actually carries (may be a subset)
    DWORD sort_type;
};
struct Item2 {
    DWORD flags;
    DWORD data_offset;              // from the start of List2
};
static_assert(sizeof(Query2) == 28 && sizeof(List2) == 20 && sizeof(Item2) == 8, "IPC layout");

}  // namespace facet::ipc
