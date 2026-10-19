#pragma once

#include <windows.h>

#define WM_APP_TRAY         (WM_APP + 1)
#define WM_APP_LAYOUT_DIRTY (WM_APP + 2)
#define WM_APP_BOX_DELETE   (WM_APP + 3)
#define WM_APP_CONTENT_DIRTY (WM_APP + 4)

HWND TheTrayWnd();
#define WM_APP_UNDO_NOW     (WM_APP + 5)
#define WM_APP_RENAME_BOX   (WM_APP + 6)
