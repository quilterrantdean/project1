#ifndef BIBLIOLED_HISTORY_DIALOG_H
#define BIBLIOLED_HISTORY_DIALOG_H
#include <windows.h>
namespace biblioled {
class LoanStore;
namespace win32 {
const UINT kHistoryEditMessage = WM_APP + 12;
const UINT kHistoryDeleteMessage = WM_APP + 13;
void ShowHistoryDialog(HINSTANCE instance, HWND owner, LoanStore* store);
}
}
#endif
