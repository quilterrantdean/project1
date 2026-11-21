#ifndef BIBLIOLED_STATISTICS_DIALOG_H
#define BIBLIOLED_STATISTICS_DIALOG_H
#include <windows.h>
namespace biblioled {
class LoanStore;
namespace win32 {
void ShowStatisticsDialog(HINSTANCE instance, HWND owner, LoanStore* store);
}
}
#endif
