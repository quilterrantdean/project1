#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "biblioled/loan_store.h"
#include "biblioled/loan_rules.h"
#include "biblioled/csv_export.h"
#include "biblioled/audit_log.h"
#include "biblioled/application_paths.h"
#include "biblioled/settings_store.h"
#include "resource.h"
#include "history_dialog.h"
#include "statistics_dialog.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#ifndef BIBLIOLED_VERSION
#define BIBLIOLED_VERSION "0.1.0"
#endif

namespace {

const wchar_t* kWindowClass = L"BiblioLEDMainWindow";
const UINT kActivateMessage = WM_APP + 10;
const UINT kPersistColumnsMessage = WM_APP + 11;
using biblioled::AppSettings;
const int kColumnCount = biblioled::kSettingsColumnCount;

struct RecordDialogContext {
    biblioled::LoanRecord record;
    bool accepted;
    RecordDialogContext() : accepted(false) {}
};

struct VisibleLoan {
    biblioled::LoanRecord record;
    std::time_t loan_time;
    std::time_t expiry;
    long days_remaining;

    std::wstring title_sort;
    std::wstring type_sort;

    VisibleLoan()
        : loan_time(static_cast<std::time_t>(-1)),
          expiry(static_cast<std::time_t>(-1)),
          days_remaining(0)
    {
    }
};

HINSTANCE gInstance = NULL;
HWND gMainWindow = NULL;
HWND gLoanList = NULL;
HWND gQuotaLabel = NULL;
HWND gStatusLabel = NULL;
HANDLE gInstanceMutex = NULL;
HICON gApplicationIcon = NULL;
HACCEL gMainAccelerators = NULL;
biblioled::ApplicationPaths gApplicationPaths;
bool gSettingsFileValid = false;
bool gColumnSettingsWarningShown = false;
bool gGeneralSettingsPending = false;
AppSettings gPendingGeneralSettings;
biblioled::LoanStore gStore;
AppSettings gSettings;
std::vector<VisibleLoan> gVisibleLoans;
int gMainSortColumn = -1;
bool gMainSortAscending = true;

void CopyGeneralSettings(const AppSettings& source, AppSettings* destination)
{
    if (!destination) return;
    destination->daily_limit = source.daily_limit;
    destination->monthly_limit = source.monthly_limit;
    destination->monthly_window_days = source.monthly_window_days;
    destination->loan_duration_days = source.loan_duration_days;
    destination->diagnostics_enabled = source.diagnostics_enabled;
    destination->audit_log_max_size_bytes = source.audit_log_max_size_bytes;
}

void UpdateWriteAccess(HWND window);
void RecordApplicationDiagnostic(const char* severity,
                                 const std::string& context,
                                 const std::string& details);

enum {
    kToolbarHeight = 34,
    kQuotaHeight = 24,
    kStatusHeight = 23
};

bool ResolveApplicationPaths(biblioled::ApplicationPaths* paths,
                            std::wstring* error)
{
    wchar_t executable_path[MAX_PATH] = {0};
    SetLastError(ERROR_SUCCESS);
    const DWORD length =
        GetModuleFileNameW(NULL, executable_path, MAX_PATH);

    if (length == 0) {
        const DWORD code = GetLastError();
        std::wostringstream message;
        message << L"Não foi possível determinar o caminho do executável "
                << L"(erro Win32 " << code << L").";
        if (error) *error = message.str();
        return false;
    }

    if (length >= MAX_PATH) {
        if (error) {
            *error = L"O caminho completo do executável excede o limite "
                     L"suportado pelo Windows XP.";
        }
        return false;
    }

    return biblioled::BuildApplicationPaths(
        std::wstring(executable_path, length), paths, error);
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return std::string();
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, NULL, 0, NULL, NULL);
    if (length <= 1) return std::string();

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], length, NULL, NULL);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return std::wstring();
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, NULL, 0);
    if (length <= 1) return std::wstring();

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], length);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

void RecordApplicationDiagnostic(const char* severity,
                                 const std::string& context,
                                 const std::string& details)
{
    // Queue internally only. The regular audit lifecycle will synchronise the
    // event later; this helper must never recurse through error reporting.
    if (!gStore.IsOpen() || gStore.MigrationRequired()) return;

    const std::string audit_details =
        "context=" + context + ";message=" + details;
    std::string ignored;
    gStore.RecordDiagnosticEvent(severity, audit_details, &ignored);
}

bool EnsureDirectory(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), NULL)) return true;

    const DWORD create_error = GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) return false;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExistsW(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool CreateDatabaseMarker(const std::wstring& path, std::wstring* error)
{
    HANDLE marker = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS && FileExistsW(path)) return true;
        if (error) {
            *error = L"Não foi possível criar o marcador de segurança da base de dados. "
                     L"A aplicação não permitirá alterações nesta sessão.";
        }
        return false;
    }

    static const char contents[] = "BiblioLED database initialized\r\n";
    DWORD written = 0;
    const BOOL wrote = WriteFile(
        marker, contents, static_cast<DWORD>(sizeof(contents) - 1), &written, NULL);
    const BOOL flushed = wrote && written == sizeof(contents) - 1
        ? FlushFileBuffers(marker) : FALSE;
    CloseHandle(marker);

    if (!wrote || written != sizeof(contents) - 1 || !flushed) {
        DeleteFileW(path.c_str());
        if (error) {
            *error = L"Não foi possível gravar o marcador de segurança da base de dados. "
                     L"A aplicação não permitirá alterações nesta sessão.";
        }
        return false;
    }
    return true;
}

std::wstring GetControlText(HWND dialog, int control_id)
{
    const HWND control = GetDlgItem(dialog, control_id);
    if (!control) return std::wstring();

    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return std::wstring();

    std::wstring value(static_cast<std::wstring::size_type>(length) + 1, L'\\0');
    const int copied = GetWindowTextW(control, &value[0], length + 1);
    if (copied <= 0) return std::wstring();

    value.resize(static_cast<std::wstring::size_type>(copied));
    return value;
}

std::wstring TrimTitle(const std::wstring& value)
{
    const std::wstring::size_type first =
        value.find_first_not_of(L" \\t\\r\\n\\f\\v");
    if (first == std::wstring::npos) return std::wstring();

    const std::wstring::size_type last =
        value.find_last_not_of(L" \\t\\r\\n\\f\\v");
    return value.substr(first, last - first + 1);
}

bool ParsePositiveInteger(const std::string& text,
                          int minimum,
                          int maximum,
                          int* value)
{
    if (!value || text.empty()) return false;
    char* end = NULL;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (!end || end == text.c_str() || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

INT_PTR CALLBACK LoanDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    RecordDialogContext* context =
        reinterpret_cast<RecordDialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));

    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<RecordDialogContext*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));

        SendDlgItemMessageW(dialog, IDC_LOAN_TYPE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"Livro"));
        SendDlgItemMessageW(dialog, IDC_LOAN_TYPE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"Audiolivro"));

        if (context && context->record.id > 0) {
            SetDlgItemTextW(dialog, IDC_LOAN_TITLE,
                            Utf8ToWide(context->record.title).c_str());

            const int type_index =
                (context->record.type == "audiolivro" || context->record.type == "Audiolivro") ? 1 : 0;
            SendDlgItemMessageW(dialog, IDC_LOAN_TYPE, CB_SETCURSEL, type_index, 0);

            const std::string::size_type separator = context->record.loan_date.find(' ');
            const std::string date = context->record.loan_date.substr(0, separator);
            const std::string time = separator == std::string::npos
                ? "00:00" : context->record.loan_date.substr(separator + 1);

            SetDlgItemTextW(dialog, IDC_LOAN_DATE, Utf8ToWide(date).c_str());
            SetDlgItemTextW(dialog, IDC_LOAN_TIME, Utf8ToWide(time).c_str());
        } else {
            SetDlgItemTextW(dialog, IDC_LOAN_TYPE, L"Livro");
            SendDlgItemMessageW(dialog, IDC_LOAN_TYPE, CB_SETCURSEL, 0, 0);

            const std::string now =
                biblioled::FormatLocalDateTime(std::time(NULL), "%Y-%m-%d %H:%M");
            SetDlgItemTextW(dialog, IDC_LOAN_DATE,
                Utf8ToWide(now.substr(0, 10)).c_str());
            SetDlgItemTextW(dialog, IDC_LOAN_TIME,
                Utf8ToWide(now.substr(11, 5)).c_str());
        }

        SetFocus(GetDlgItem(dialog, IDC_LOAN_TITLE));
        return FALSE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
        if (!context) return TRUE;

        const std::wstring title = TrimTitle(GetControlText(dialog, IDC_LOAN_TITLE));
        const std::wstring date = GetControlText(dialog, IDC_LOAN_DATE);
        const std::wstring time = GetControlText(dialog, IDC_LOAN_TIME);
        const LRESULT type_index =
            SendDlgItemMessageW(dialog, IDC_LOAN_TYPE, CB_GETCURSEL, 0, 0);

        if (title.empty()) {
            RecordApplicationDiagnostic(
                "warning", "loan_dialog", "reason=missing_title");
            MessageBoxW(dialog, L"O título é obrigatório.", L"BiblioLED", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_LOAN_TITLE));
            return TRUE;
        }
        if (type_index == CB_ERR) {
            RecordApplicationDiagnostic(
                "warning", "loan_dialog", "reason=missing_loan_type");
            MessageBoxW(dialog, L"Seleccione o tipo de empréstimo.", L"BiblioLED",
                        MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_LOAN_TYPE));
            return TRUE;
        }

        const std::string loan_date = WideToUtf8(date) + " " + WideToUtf8(time);
        if (biblioled::ParseLoanDateTime(loan_date) == static_cast<std::time_t>(-1)) {
            RecordApplicationDiagnostic(
                "warning", "loan_dialog", "reason=invalid_loan_date_time");
            MessageBoxW(dialog,
                L"Data ou hora inválida. Utilize AAAA-MM-DD e HH:MM.",
                L"BiblioLED", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_LOAN_DATE));
            return TRUE;
        }

        context->record.title = WideToUtf8(title);
        context->record.loan_date = loan_date;
        context->record.type = type_index == 1 ? "audiolivro" : "livro";
        context->accepted = true;
        EndDialog(dialog, IDOK);
        return TRUE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }

    return FALSE;
}

bool ShowLoanDialog(HWND parent, biblioled::LoanRecord* record)
{
    if (!record) return false;

    RecordDialogContext context;
    context.record = *record;

    const INT_PTR result = DialogBoxParamW(
        gInstance, MAKEINTRESOURCEW(IDD_LOAN_DIALOG), parent,
        LoanDialogProc, reinterpret_cast<LPARAM>(&context));

    if (result != IDOK || !context.accepted) return false;
    *record = context.record;
    return true;
}

int SelectedRecordId()
{
    if (!gLoanList) return 0;
    const int selected = ListView_GetNextItem(gLoanList, -1, LVNI_SELECTED);
    if (selected < 0) return 0;

    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (!ListView_GetItem(gLoanList, &item)) return 0;
    return static_cast<int>(item.lParam);
}

void ShowStoreError(const wchar_t* operation, const std::string& error)
{
    RecordApplicationDiagnostic(
        "failure", WideToUtf8(std::wstring(operation)), error);
    std::wstring message = operation;
    message += L"\n\n";
    message += Utf8ToWide(error);
    MessageBoxW(gMainWindow, message.c_str(), L"BiblioLED", MB_OK | MB_ICONERROR);
}

bool SynchronizeExternalAuditLog(bool show_error)
{
    if (!gSettings.diagnostics_enabled) {
        gStore.SetExternalAuditLogStatus(false, true);
        UpdateWriteAccess(gMainWindow);
        return true;
    }

    // A legacy database has no audit table until the user confirms migration.
    // Its write operations are already blocked by the store.
    if (gStore.MigrationRequired()) {
        gStore.SetExternalAuditLogStatus(true, true);
        UpdateWriteAccess(gMainWindow);
        return true;
    }

    std::vector<biblioled::AuditEvent> events;
    std::string error;
    bool ok = gStore.GetAuditEvents(&events, &error);
    int written = 0;

    if (ok) {
        const std::wstring log_directory =
            gApplicationPaths.log_directory;
        ok = biblioled::SynchronizeAuditLog(
            log_directory, events, gSettings.audit_log_max_size_bytes,
            &written, &error);
    }

    gStore.SetExternalAuditLogStatus(true, ok);
    UpdateWriteAccess(gMainWindow);

    if (!ok && show_error) {
        ShowStoreError(
            L"Não foi possível sincronizar o registo de auditoria. "
            L"As alterações à base de dados ficam bloqueadas até o log estar disponível.",
            error);
    }
    return ok;
}

bool OfferLegacyMigration(HWND owner)
{
    if (!gStore.MigrationRequired()) return true;

    std::wstring question =
        L"A base de dados usa o esquema legado (versão 0).\n\n"
        L"A migração para a versão 1 acrescenta uma tabela de auditoria interna. "
        L"Os registos de empréstimos não serão convertidos nem eliminados.\n\n"
        L"Antes da migração será criada e validada uma cópia integral da base de dados. "
        L"A migração é transaccional e exige confirmação.\n\n"
        L"Pretende migrar agora?";

    if (MessageBoxW(owner, question.c_str(), L"Migração da base de dados",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
        MessageBoxW(owner,
            L"A base de dados será aberta em modo só de leitura. "
            L"Pode consultar os empréstimos e criar cópias de segurança, "
            L"mas não pode alterar registos até confirmar a migração.",
            L"Modo só de leitura", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    std::string backup_path;
    std::string error;
    if (!gStore.MigrateLegacySchema(&backup_path, &error)) {
        std::wstring message = L"Não foi possível migrar a base de dados.\n\n";
        message += Utf8ToWide(error);
        MessageBoxW(owner, message.c_str(), L"Migração da base de dados",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    std::wstring message = L"Base de dados migrada para a versão 1.\n\n";
    message += L"Cópia de segurança anterior à migração:\n";
    message += Utf8ToWide(backup_path);
    MessageBoxW(owner, message.c_str(), L"Migração concluída",
                MB_OK | MB_ICONINFORMATION);
    return true;
}


bool SaveColumnLayout()
{
    if (!gLoanList || !gSettingsFileValid) return false;

    int order[kColumnCount] = {0};
    if (!ListView_GetColumnOrderArray(gLoanList, kColumnCount, order)) return false;

    for (int i = 0; i < kColumnCount; ++i) {
        gSettings.column_order[i] = order[i];
        gSettings.column_widths[i] = ListView_GetColumnWidth(gLoanList, i);
    }

    AppSettings persisted = gSettings;
    if (gGeneralSettingsPending) {
        CopyGeneralSettings(gPendingGeneralSettings, &persisted);
    }

    std::string error;
    if (!biblioled::SaveSettingsFile(WideToUtf8(gApplicationPaths.settings_file), persisted, &error)) {
        if (!gColumnSettingsWarningShown) {
            RecordApplicationDiagnostic(
                "failure", "save_column_layout", error);
            MessageBoxW(gMainWindow, Utf8ToWide(error).c_str(), L"BiblioLED",
                        MB_OK | MB_ICONWARNING);
            gColumnSettingsWarningShown = true;
        }
        return false;
    }
    return true;
}

void ApplyColumnLayout()
{
    if (gLoanList) {
        ListView_SetColumnOrderArray(gLoanList, kColumnCount, gSettings.column_order);
    }
}

void InsertColumn(int index, const wchar_t* title, int width)
{
    LVCOLUMNW column = {};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = gSettings.column_widths[index] > 0
        ? gSettings.column_widths[index] : width;
    column.iSubItem = index;
    ListView_InsertColumn(gLoanList, index, &column);
}

int CompareTextInsensitive(const std::wstring& left,
                           const std::wstring& right)
{
    const int result = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
        left.c_str(), -1, right.c_str(), -1);
    if (result == 0) {
        if (left < right) return -1;
        if (right < left) return 1;
        return 0;
    }
    return result - CSTR_EQUAL;
}
bool MainLoanLess(const VisibleLoan& left, const VisibleLoan& right)
{
    int comparison = 0;
    switch (gMainSortColumn) {
    case 0: comparison = CompareTextInsensitive(left.title_sort, right.title_sort); break;
    case 1: comparison = CompareTextInsensitive(left.type_sort, right.type_sort); break;
    case 2: comparison = left.loan_time < right.loan_time ? -1 : (left.loan_time > right.loan_time ? 1 : 0); break;
    case 3: comparison = left.expiry < right.expiry ? -1 : (left.expiry > right.expiry ? 1 : 0); break;
    case 4: comparison = left.days_remaining < right.days_remaining ? -1 : (left.days_remaining > right.days_remaining ? 1 : 0); break;
    default: comparison = left.expiry < right.expiry ? -1 : (left.expiry > right.expiry ? 1 : 0); break;
    }
    if (comparison == 0) return left.record.id < right.record.id;
    return gMainSortAscending ? comparison < 0 : comparison > 0;
}
void SortMainLoans()
{
    std::sort(gVisibleLoans.begin(), gVisibleLoans.end(), MainLoanLess);
}
void PopulateMainLoanList()
{
    if (!gLoanList) return;
    ListView_DeleteAllItems(gLoanList);
    for (size_t i = 0; i < gVisibleLoans.size(); ++i) {
        const VisibleLoan& item = gVisibleLoans[i];
        const biblioled::LoanRecord& record = item.record;
        const std::wstring type = record.type == "audiolivro" ? L"Audiolivro" : L"Livro";
        const std::wstring loan_date = Utf8ToWide(biblioled::FormatLocalDateTime(item.loan_time, "%d/%m/%Y %H:%M"));
        const std::wstring expiry = Utf8ToWide(biblioled::FormatLocalDateTime(item.expiry, "%d/%m/%Y %H:%M"));
        wchar_t days[32] = {0};
        _snwprintf(days, 31, L"%ld", item.days_remaining);
        LVITEMW row = {};
        row.mask = LVIF_TEXT | LVIF_PARAM;
        row.iItem = static_cast<int>(i);
        row.iSubItem = 0;
        row.pszText = const_cast<LPWSTR>(item.title_sort.c_str());
        row.lParam = record.id;
        const int inserted = ListView_InsertItem(gLoanList, &row);
        if (inserted >= 0) {
            ListView_SetItemText(gLoanList, inserted, 1, const_cast<LPWSTR>(type.c_str()));
            ListView_SetItemText(gLoanList, inserted, 2, const_cast<LPWSTR>(loan_date.c_str()));
            ListView_SetItemText(gLoanList, inserted, 3, const_cast<LPWSTR>(expiry.c_str()));
            ListView_SetItemText(gLoanList, inserted, 4, days);
        }
    }
}
void SelectMainRecord(int record_id)
{
    if (!gLoanList || record_id <= 0) return;
    for (int i = 0; i < ListView_GetItemCount(gLoanList); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (ListView_GetItem(gLoanList, &item) && static_cast<int>(item.lParam) == record_id) {
            ListView_SetItemState(gLoanList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(gLoanList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(gLoanList, i, FALSE);
            return;
        }
    }
}
void RefreshList()
{
    if (!gLoanList) return;
    std::vector<biblioled::LoanRecord> records;
    std::string error;
    if (!gStore.GetAll(&records, &error)) {
        ShowStoreError(L"Não foi possível ler os empréstimos.", error);
        return;
    }
    const std::time_t now = std::time(NULL);
    std::vector<VisibleLoan> visible;
    int invalid_dates = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        biblioled::LoanEvaluation evaluation;
        if (!biblioled::EvaluateLoanDateTime(records[i].loan_date, gSettings.loan_duration_days, now, &evaluation)) {
            ++invalid_dates;
            continue;
        }
        // Future records remain visible and continue to reserve quota.
        // Expired records remain available in the separate history window.
        if (evaluation.visible_in_operational_list) {
            VisibleLoan item;
            item.record = records[i];
            item.loan_time = evaluation.loan_time;
            item.expiry = evaluation.expiry_time;
            item.days_remaining = evaluation.days_remaining;
            item.title_sort = Utf8ToWide(records[i].title);
            item.type_sort = Utf8ToWide(records[i].type);
            visible.push_back(item);
        }
    }
    gVisibleLoans.swap(visible);
    SortMainLoans();
    PopulateMainLoanList();

    int daily_count = 0;
    int monthly_count = 0;
    biblioled::QuotaWindowBounds quota_windows;
    if (!biblioled::BuildQuotaWindowBounds(
            now, gSettings.monthly_window_days, &quota_windows)) {
        ShowStoreError(L"Não foi possível calcular as janelas de disponibilidade.",
                       "Invalid local date/time or monthly window.");
        return;
    }

    // CountSince deliberately has no upper bound: future-dated records reserve quota.
    if (!gStore.CountSince(quota_windows.daily_since, &daily_count, &error) ||
        !gStore.CountSince(quota_windows.monthly_since, &monthly_count, &error)) {
        ShowStoreError(L"Não foi possível calcular a disponibilidade.", error);
        return;
    }

    biblioled::QuotaAvailability availability;
    if (!biblioled::CalculateQuotaAvailability(
            gSettings.daily_limit, daily_count,
            gSettings.monthly_limit, monthly_count, &availability)) {
        ShowStoreError(L"Não foi possível calcular a disponibilidade.",
                       "Invalid quota limit or loan count.");
        return;
    }

    wchar_t quota[256] = {0};
    _snwprintf(quota, 255,
        L"Disponibilidade: hoje %d/%d livres  |  últimos %d dias %d/%d livres",
        availability.daily_remaining, gSettings.daily_limit,
        gSettings.monthly_window_days, availability.monthly_remaining, gSettings.monthly_limit);
    SetWindowTextW(gQuotaLabel, quota);

    wchar_t status[256] = {0};
    _snwprintf(status, 255,
        L"%u empréstimo(s) apresentados%ls%ls%ls",
        static_cast<unsigned int>(gVisibleLoans.size()),
        invalid_dates ? L"  |  Existem registos com datas inválidas" : L"",
        gStore.MigrationRequired()
            ? L"  |  Só leitura: migração por confirmar" : L"",
        gStore.ExternalAuditLogBlocked()
            ? L"  |  Só leitura: log indisponível" : L"");
    SetWindowTextW(gStatusLabel, status);
}

void AddLoan()
{
    biblioled::LoanRecord record;
    if (!ShowLoanDialog(gMainWindow, &record)) return;

    std::string error;
    if (!gStore.Insert(&record, &error)) {
        ShowStoreError(L"Não foi possível criar o empréstimo.", error);
        return;
    }
    SynchronizeExternalAuditLog(true);
    RefreshList();
}

bool WriteExportFileAtomically(const std::wstring& destination,
                                const std::string& contents,
                                std::string* error)
{
    const std::wstring::size_type separator=destination.find_last_of(L"\\/");
    const std::wstring directory=separator==std::wstring::npos?L".":destination.substr(0,separator);
    wchar_t temporary[MAX_PATH]={0};
    if(!GetTempFileNameW(directory.c_str(),L"BLD",0,temporary)){
        std::ostringstream message; message<<"Unable to create temporary file (Win32 error "<<GetLastError()<<").";
        if(error)*error=message.str(); return false;
    }
    HANDLE file=CreateFileW(temporary,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE){
        const DWORD code=GetLastError(); DeleteFileW(temporary);
        std::ostringstream message; message<<"Unable to open temporary file (Win32 error "<<code<<").";
        if(error)*error=message.str(); return false;
    }
    bool ok=true; DWORD failure=ERROR_SUCCESS; size_t offset=0;
    while(offset<contents.size()){
        const size_t remaining=contents.size()-offset;
        const DWORD requested=remaining>0x7fffffffU?0x7fffffffU:static_cast<DWORD>(remaining);
        DWORD written=0;
        if(!WriteFile(file,contents.data()+offset,requested,&written,NULL)){failure=GetLastError();ok=false;break;}
        if(written==0){failure=ERROR_WRITE_FAULT;ok=false;break;}
        offset+=written;
    }
    if(ok&&!FlushFileBuffers(file)){failure=GetLastError();ok=false;}
    CloseHandle(file);
    if(!ok){
        DeleteFileW(temporary); std::ostringstream message;
        message<<"Unable to write export file (Win32 error "<<failure<<").";
        if(error)*error=message.str(); return false;
    }
    if(!MoveFileExW(temporary,destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        const DWORD code=GetLastError(); DeleteFileW(temporary);
        std::ostringstream message; message<<"Unable to replace destination (Win32 error "<<code<<").";
        if(error)*error=message.str(); return false;
    }
    return true;
}

void ExportLoansToCsv()
{
    std::vector<biblioled::LoanRecord> records;
    std::string error;
    if(!gStore.GetAll(&records,&error)){
        ShowStoreError(L"Não foi possível preparar a exportação.",error); return;
    }
    const std::wstring export_directory=gApplicationPaths.export_directory;
    if(!EnsureDirectory(export_directory)){
        ShowStoreError(L"Não foi possível preparar a pasta de exportação.","Unable to create exports directory."); return;
    }
    std::string timestamp=biblioled::FormatLocalDateTime(std::time(NULL),"%Y%m%d_%H%M");
    if(timestamp.empty()) timestamp="export";
    std::wstring suggested=L"bd_export_";
    suggested+=Utf8ToWide(timestamp); suggested+=L".csv";
    wchar_t filename[MAX_PATH]={0};
    const size_t count=suggested.size()<MAX_PATH-1?suggested.size():MAX_PATH-1;
    suggested.copy(filename,count); filename[count]=L'\0';
    static const wchar_t filter[]=L"CSV UTF-8 (*.csv)\0*.csv\0Todos os ficheiros (*.*)\0*.*\0\0";
    OPENFILENAMEW dialog={};
    dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=gMainWindow;
    dialog.lpstrFilter=filter; dialog.nFilterIndex=1; dialog.lpstrFile=filename;
    dialog.nMaxFile=MAX_PATH; dialog.lpstrInitialDir=export_directory.c_str();
    dialog.lpstrTitle=L"Exportar histórico de empréstimos"; dialog.lpstrDefExt=L"csv";
    dialog.Flags=OFN_EXPLORER|OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT|OFN_HIDEREADONLY|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&dialog)){
        const DWORD code=CommDlgExtendedError();
        if(code){std::ostringstream message;message<<"Common dialog error "<<code<<".";
            ShowStoreError(L"Não foi possível abrir a janela de exportação.",message.str());}
        return;
    }
    const std::string csv=biblioled::BuildLoanRecordsCsv(records);
    if(!WriteExportFileAtomically(filename,csv,&error)){
        ShowStoreError(L"Não foi possível gravar o ficheiro CSV.",error); return;
    }
    std::ostringstream details;
    details<<"path="<<WideToUtf8(filename)<<";records="<<records.size()<<";format=UTF-8;delimiter=semicolon";
    error.clear();
    if(gStore.MigrationRequired()||!gStore.RecordAuditEvent("records_exported",details.str(),&error)){
        std::wstring warning=L"O ficheiro foi exportado, mas a operação não pôde ser registada na auditoria.\n\n";
        warning+=filename;
        RecordApplicationDiagnostic(
            "failure", "csv_export_audit", error);
        MessageBoxW(gMainWindow,warning.c_str(),L"Exportação concluída com aviso",MB_OK|MB_ICONWARNING);
        return;
    }
    SynchronizeExternalAuditLog(true);
    std::wostringstream message;
    message<<L"Exportação concluída: "<<records.size()<<L" registo(s).\n\n"<<filename;
    MessageBoxW(gMainWindow,message.str().c_str(),L"Exportação concluída",MB_OK|MB_ICONINFORMATION);
}

void CreateManualDatabaseBackup()
{
    std::string backup_path;
    std::string error;
    if (!gStore.CreateManualBackup(&backup_path, &error)) {
        ShowStoreError(L"Não foi possível criar a cópia de segurança.", error);
        return;
    }

    error.clear();
    if (gStore.RecordAuditEvent("manual_backup", "path=" + backup_path, &error)) {
        SynchronizeExternalAuditLog(true);
    } else {
        ShowStoreError(L"A cópia foi criada, mas não foi possível registar a operação na auditoria.", error);
    }

    std::wstring message = L"Cópia de segurança criada e validada:\n\n";
    message += Utf8ToWide(backup_path);
    MessageBoxW(gMainWindow, message.c_str(), L"BiblioLED",
                MB_OK | MB_ICONINFORMATION);
}

bool PromptAndRecoverDatabase(const std::wstring& database_path,
                               const std::wstring& backup_directory,
                               const std::wstring& reason,
                               std::string* selected_backup_path,
                               std::string* preserved_database_path)
{
    std::wstring prompt =
        L"A base de dados não está disponível ou não pode ser aberta.\n\n";
    prompt += reason;
    prompt +=
        L"\n\nPode tentar recuperar a base de dados a partir de uma cópia "
        L"de segurança válida. A operação só avança depois de seleccionar "
        L"e confirmar uma cópia.\n\nPretende seleccionar uma cópia agora?";

    if (MessageBoxW(NULL, prompt.c_str(), L"Recuperação da base de dados",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
        return false;
    }

    wchar_t filename[MAX_PATH] = {0};
    static const wchar_t filter[] =
        L"Cópias SQLite (*.sqlite;*.sqlite.gz;*.sqlite.zlib)\0"
        L"*.sqlite;*.sqlite.gz;*.sqlite.zlib\0"
        L"Todos os ficheiros (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = NULL;
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = backup_directory.c_str();
    dialog.lpstrTitle = L"Seleccionar cópia de segurança para recuperação";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                   OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&dialog)) {
        const DWORD dialog_error = CommDlgExtendedError();
        if (dialog_error != 0) {
            std::wostringstream message;
            message << L"Não foi possível abrir o janela de selecção de cópias de segurança. "
                    << L"Código de erro do Windows: " << dialog_error << L".";
            MessageBoxW(NULL, message.str().c_str(), L"Recuperação da base de dados",
                        MB_OK | MB_ICONERROR);
        }
        return false;
    }

    const std::wstring selected(filename);
    std::wstring confirmation =
        L"A base de dados será reconstruída com os dados da cópia seleccionada. "
        L"Os registos posteriores à data dessa cópia poderão não estar presentes.\n\n"
        L"A cópia seleccionada não será alterada. Se existir uma base de dados "
        L"anterior, o ficheiro e os respectivos ficheiros auxiliares SQLite "
        L"serão preservados com outro nome antes da instalação.\n\n";
    confirmation += selected;
    confirmation += L"\n\nPretende continuar?";

    if (MessageBoxW(NULL, confirmation.c_str(), L"Confirmar recuperação",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
        return false;
    }

    std::string preserved;
    std::string error;
    if (!gStore.RecoverFromBackup(
            WideToUtf8(database_path), WideToUtf8(selected), &preserved, &error)) {
        std::wstring message = L"Não foi possível recuperar a base de dados.\n\n";
        message += Utf8ToWide(error);
        MessageBoxW(NULL, message.c_str(), L"Recuperação falhada",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    if (selected_backup_path) *selected_backup_path = WideToUtf8(selected);
    if (preserved_database_path) *preserved_database_path = preserved;
    return true;
}

void RestoreDatabaseFromBackup()
{
    wchar_t filename[MAX_PATH] = {0};
    const std::wstring backup_directory =
        gApplicationPaths.backup_directory;

    static const wchar_t filter[] =
        L"Cópias SQLite (*.sqlite;*.sqlite.gz;*.sqlite.zlib)\0*.sqlite;*.sqlite.gz;*.sqlite.zlib\0"
        L"Todos os ficheiros (*.*)\0*.*\0\0";

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = gMainWindow;
    dialog.lpstrFilter = filter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = backup_directory.c_str();
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                   OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameW(&dialog)) {
        const DWORD dialog_error = CommDlgExtendedError();
        if (dialog_error != 0) {
            std::ostringstream detail;
            detail << "Windows common file dialog error " << dialog_error << ".";
            ShowStoreError(
                L"Não foi possível abrir o janela de selecção de cópias de segurança.",
                detail.str());
        }
        return;
    }

    const std::wstring selected = filename;
    std::wstring confirmation =
        L"A reposição irá substituir todos os registos pelos dados da cópia seleccionada.\n\n";
    confirmation += L"Antes de continuar, será criada e validada uma cópia da base de dados actual.\n\n";
    confirmation += selected;
    confirmation += L"\n\nPretende continuar?";

    if (MessageBoxW(gMainWindow, confirmation.c_str(), L"Confirmar reposição",
                    MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
        return;
    }

    std::string safety_backup;
    std::string error;
    if (!gStore.RestoreFromBackup(WideToUtf8(selected), &safety_backup, &error)) {
        ShowStoreError(L"Não foi possível repor a cópia de segurança.", error);
        return;
    }

    OfferLegacyMigration(gMainWindow);
    error.clear();
    if (!gStore.MigrationRequired() &&
        !gStore.RecordAuditEvent(
            "restore",
            "source=" + WideToUtf8(selected) + "; safety_backup=" + safety_backup,
            &error)) {
        ShowStoreError(L"A base de dados foi reposta, mas não foi possível registar a operação na auditoria.", error);
    }

    std::wstring message = L"Base de dados reposta. A versão anterior foi salvaguardada em:\n\n";
    message += Utf8ToWide(safety_backup);
    MessageBoxW(gMainWindow, message.c_str(), L"BiblioLED",
                MB_OK | MB_ICONINFORMATION);

    SynchronizeExternalAuditLog(true);
    UpdateWriteAccess(gMainWindow);
    RefreshList();
}

bool EditLoanById(int id, HWND owner)
{
    if(id<=0 || gStore.MigrationRequired() || gStore.ExternalAuditLogBlocked()) return false;
    biblioled::LoanRecord record;
    std::string error;
    if(!gStore.GetById(id,&record,&error)) {
        ShowStoreError(L"Não foi possível abrir o empréstimo.",error);
        return false;
    }
    if(!ShowLoanDialog(owner,&record)) return false;
    if(!gStore.Update(record,&error)) {
        ShowStoreError(L"Não foi possível guardar as alterações.",error);
        return false;
    }
    SynchronizeExternalAuditLog(true);
    RefreshList();
    return true;
}
void EditLoan()
{
    const int id=SelectedRecordId();
    if(id<=0) {
        MessageBoxW(gMainWindow,L"Seleccione um empréstimo.",L"BiblioLED",
                    MB_OK|MB_ICONINFORMATION);
        return;
    }
    EditLoanById(id,gMainWindow);
}
bool DeleteLoanById(int id,HWND owner,bool confirm)
{
    if(id<=0 || gStore.MigrationRequired() || gStore.ExternalAuditLogBlocked()) return false;
    if(confirm && MessageBoxW(owner,L"Eliminar o empréstimo seleccionado?",
        L"Confirmar eliminação",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES) return false;
    std::string error;
    if(!gStore.Remove(id,&error)) {
        ShowStoreError(L"Não foi possível eliminar o empréstimo.",error);
        return false;
    }
    SynchronizeExternalAuditLog(true);
    RefreshList();
    return true;
}
void DeleteLoan()
{
    const int id=SelectedRecordId();
    if(id<=0) {
        MessageBoxW(gMainWindow,L"Seleccione um empréstimo.",L"BiblioLED",
                    MB_OK|MB_ICONINFORMATION);
        return;
    }
    DeleteLoanById(id,gMainWindow,true);
}

HMENU CreateApplicationMenu()
{
    HMENU menu = CreateMenu();
    HMENU file_menu = CreatePopupMenu();
    HMENU record_menu = CreatePopupMenu();
    HMENU view_menu = CreatePopupMenu();
    HMENU options_menu = CreatePopupMenu();
    HMENU help_menu = CreatePopupMenu();

    const UINT write_menu_flags = gStore.MigrationRequired() ? MF_GRAYED : MF_STRING;
    AppendMenuW(file_menu, write_menu_flags, IDM_NEW, L"Novo registo\tCtrl+N");
    AppendMenuW(file_menu, MF_STRING, IDM_BACKUP, L"Criar cópia de segurança...");
    AppendMenuW(file_menu, MF_STRING, IDM_RESTORE, L"Repor a partir de cópia...");
    AppendMenuW(file_menu, MF_STRING, IDM_EXPORT_CSV, L"Exportar histórico para CSV...");
    AppendMenuW(file_menu, MF_STRING, IDM_RETRY_AUDIT_SYNC,
                L"Tentar sincronizar registo de auditoria...");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file_menu, MF_STRING, IDM_EXIT, L"Sair");

    AppendMenuW(options_menu, MF_STRING, IDM_GENERAL_SETTINGS, L"Definições gerais...");
    AppendMenuW(options_menu, MF_STRING, IDM_BACKUP_SETTINGS, L"Definições de cópias...");

    AppendMenuW(record_menu, write_menu_flags, IDM_EDIT, L"Editar\tEnter");
    AppendMenuW(record_menu, write_menu_flags, IDM_DELETE, L"Eliminar\tDelete");

    AppendMenuW(view_menu, MF_STRING, IDM_HISTORY, L"Histórico\tCtrl+H");
    AppendMenuW(view_menu, MF_STRING, IDM_STATISTICS, L"Estatísticas...");
    AppendMenuW(help_menu, MF_STRING, IDM_ABOUT, L"Sobre");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"Ficheiro");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(record_menu), L"Registo");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"Ver");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(options_menu), L"Opções");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"Ajuda");
    return menu;
}

void CreateChildControls(HWND window)
{
    const DWORD write_button_style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON |
        (gStore.MigrationRequired() ? WS_DISABLED : 0);
    CreateWindowW(L"BUTTON", L"Novo", write_button_style,
        8, 6, 62, 24, window, reinterpret_cast<HMENU>(IDC_BUTTON_NEW), gInstance, NULL);
    CreateWindowW(L"BUTTON", L"Editar", write_button_style,
        74, 6, 62, 24, window, reinterpret_cast<HMENU>(IDC_BUTTON_EDIT), gInstance, NULL);
    CreateWindowW(L"BUTTON", L"Eliminar", write_button_style,
        140, 6, 66, 24, window, reinterpret_cast<HMENU>(IDC_BUTTON_DELETE), gInstance, NULL);

    gQuotaLabel = CreateWindowW(L"STATIC", L"Disponibilidade",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        9, 38, 600, 20, window,
        reinterpret_cast<HMENU>(IDC_QUOTA_LABEL), gInstance, NULL);

    gLoanList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        8, 62, 700, 400, window,
        reinterpret_cast<HMENU>(IDC_LOAN_LIST), gInstance, NULL);

    ListView_SetExtendedListViewStyle(gLoanList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
        LVS_EX_HEADERDRAGDROP);

    InsertColumn(0, L"Título", 320);
    InsertColumn(1, L"Tipo", 100);
    InsertColumn(2, L"Empréstimo", 130);
    InsertColumn(3, L"Expira em", 130);
    InsertColumn(4, L"Dias", 55);
    ApplyColumnLayout();

    gStatusLabel = CreateWindowW(L"STATIC", L"Pronto.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        8, 0, 700, 20, window,
        reinterpret_cast<HMENU>(IDC_STATUS_LABEL), gInstance, NULL);
}

void UpdateWriteAccess(HWND window)
{
    if (!window) return;
    const BOOL enabled =
        (gStore.MigrationRequired() || gStore.ExternalAuditLogBlocked()) ? FALSE : TRUE;

    EnableWindow(GetDlgItem(window, IDC_BUTTON_NEW), enabled);
    EnableWindow(GetDlgItem(window, IDC_BUTTON_EDIT), enabled);
    EnableWindow(GetDlgItem(window, IDC_BUTTON_DELETE), enabled);

    HMENU menu = GetMenu(window);
    if (menu) {
        HMENU file_menu = GetSubMenu(menu, 0);
        HMENU record_menu = GetSubMenu(menu, 1);
        const UINT flags = MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED);
        if (file_menu) EnableMenuItem(file_menu, IDM_NEW, flags);
        if (record_menu) {
            EnableMenuItem(record_menu, IDM_EDIT, flags);
            EnableMenuItem(record_menu, IDM_DELETE, flags);
        }
        DrawMenuBar(window);
    }
}

void LayoutControls(HWND window)
{
    RECT client = {};
    GetClientRect(window, &client);

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int list_height = height - kToolbarHeight - kQuotaHeight - kStatusHeight - 12;

    if (gQuotaLabel) {
        MoveWindow(gQuotaLabel, 9, kToolbarHeight + 3, width - 18, 18, TRUE);
    }
    if (gLoanList) {
        MoveWindow(gLoanList, 8, kToolbarHeight + kQuotaHeight,
                   width - 16, list_height, TRUE);

    }
    if (gStatusLabel) {
        MoveWindow(gStatusLabel, 8, height - kStatusHeight,
                   width - 16, kStatusHeight - 2, TRUE);
    }
}

INT_PTR CALLBACK AboutDialogProc(HWND dialog, UINT message,
                                            WPARAM wParam, LPARAM)
{
    if (message == WM_INITDIALOG) {
        HICON icon = gApplicationIcon
            ? gApplicationIcon
            : LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_BIBLIOLED));
        if (icon) {
            SendDlgItemMessageW(dialog, IDC_ABOUT_ICON, STM_SETICON,
                                reinterpret_cast<WPARAM>(icon), 0);
        }

        std::wstring version = L"Versão: ";
        version += Utf8ToWide(BIBLIOLED_VERSION);
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION, version.c_str());

        std::wstring build = L"Compilação: ";
        build += Utf8ToWide(std::string(__DATE__) + " " + __TIME__);
        SetDlgItemTextW(dialog, IDC_ABOUT_BUILD, build.c_str());
        return TRUE;
    }

    if (message == WM_COMMAND &&
        (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}

void ShowAbout()
{
    DialogBoxParamW(gInstance, MAKEINTRESOURCEW(IDD_ABOUT_DIALOG),
                    gMainWindow, AboutDialogProc, 0);
}


INT_PTR CALLBACK BackupSettingsDialogProc(HWND dialog, UINT message,
                                                WPARAM wParam, LPARAM lParam)
{
    AppSettings* settings =
        reinterpret_cast<AppSettings*>(GetWindowLongPtrW(dialog, DWLP_USER));

    if (message == WM_INITDIALOG) {
        settings = reinterpret_cast<AppSettings*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(settings));

        SendDlgItemMessageW(dialog, IDC_BACKUP_FORMAT, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"GZIP (.gz)"));
        SendDlgItemMessageW(dialog, IDC_BACKUP_FORMAT, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"zlib (.zlib)"));
        SendDlgItemMessageW(dialog, IDC_BACKUP_FORMAT, CB_SETCURSEL,
                            settings && settings->backup_compression_format == "zlib" ? 1 : 0, 0);

        for (int level = 0; level <= 9; ++level) {
            wchar_t item[8] = {0};
            _snwprintf(item, 7, L"%d", level);
            SendDlgItemMessageW(dialog, IDC_BACKUP_LEVEL, CB_ADDSTRING, 0,
                                reinterpret_cast<LPARAM>(item));
        }
        SendDlgItemMessageW(dialog, IDC_BACKUP_LEVEL, CB_SETCURSEL,
                            settings ? settings->backup_compression_level : 6, 0);
        return TRUE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
        if (!settings) return TRUE;
        const LRESULT format = SendDlgItemMessageW(
            dialog, IDC_BACKUP_FORMAT, CB_GETCURSEL, 0, 0);
        const LRESULT level = SendDlgItemMessageW(
            dialog, IDC_BACKUP_LEVEL, CB_GETCURSEL, 0, 0);
        if ((format != 0 && format != 1) || level < 0 || level > 9) {
            MessageBoxW(dialog, L"Seleccione um formato e um nível de compressão válidos.",
                        L"BiblioLED", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        settings->backup_compression_format = format == 1 ? "zlib" : "gzip";
        settings->backup_compression_level = static_cast<int>(level);
        EndDialog(dialog, IDOK);
        return TRUE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK GeneralSettingsDialogProc(HWND dialog, UINT message,
                                                   WPARAM wParam, LPARAM lParam)
{
    AppSettings* settings =
        reinterpret_cast<AppSettings*>(GetWindowLongPtrW(dialog, DWLP_USER));

    if (message == WM_INITDIALOG) {
        settings = reinterpret_cast<AppSettings*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(settings));
        if (settings) {
            wchar_t value[32] = {0};
            _snwprintf(value, 31, L"%d", settings->daily_limit);
            SetDlgItemTextW(dialog, IDC_GENERAL_DAILY_LIMIT, value);
            _snwprintf(value, 31, L"%d", settings->monthly_limit);
            SetDlgItemTextW(dialog, IDC_GENERAL_MONTHLY_LIMIT, value);
            _snwprintf(value, 31, L"%d", settings->monthly_window_days);
            SetDlgItemTextW(dialog, IDC_GENERAL_MONTHLY_WINDOW, value);
            _snwprintf(value, 31, L"%d", settings->loan_duration_days);
            SetDlgItemTextW(dialog, IDC_GENERAL_LOAN_DURATION, value);
            const unsigned long size_kb =
                (settings->audit_log_max_size_bytes + 1023UL) / 1024UL;
            _snwprintf(value, 31, L"%lu", size_kb);
            SetDlgItemTextW(dialog, IDC_GENERAL_LOG_SIZE_KB, value);
            SendDlgItemMessageW(dialog, IDC_GENERAL_DIAGNOSTICS, BM_SETCHECK,
                settings->diagnostics_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        SetFocus(GetDlgItem(dialog, IDC_GENERAL_DAILY_LIMIT));
        return FALSE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDOK) {
        if (!settings) return TRUE;

        int daily = 0;
        int monthly = 0;
        int window_days = 0;
        int duration = 0;
        int log_size_kb = 0;
        const bool valid =
            ParsePositiveInteger(WideToUtf8(GetControlText(dialog, IDC_GENERAL_DAILY_LIMIT)),
                                 1, 1000, &daily) &&
            ParsePositiveInteger(WideToUtf8(GetControlText(dialog, IDC_GENERAL_MONTHLY_LIMIT)),
                                 1, 10000, &monthly) &&
            ParsePositiveInteger(WideToUtf8(GetControlText(dialog, IDC_GENERAL_MONTHLY_WINDOW)),
                                 1, 3660, &window_days) &&
            ParsePositiveInteger(WideToUtf8(GetControlText(dialog, IDC_GENERAL_LOAN_DURATION)),
                                 1, 3660, &duration) &&
            ParsePositiveInteger(WideToUtf8(GetControlText(dialog, IDC_GENERAL_LOG_SIZE_KB)),
                                 64, 1048576, &log_size_kb);

        if (!valid) {
            RecordApplicationDiagnostic(
                "warning", "general_settings", "reason=invalid_input_values");
            MessageBoxW(dialog,
                L"Verifique os valores introduzidos.\n\n"
                L"Limite diário: 1–1000; limite mensal: 1–10000; "
                L"janela e duração: 1–3660 dias; log: 64–1048576 KB.",
                L"Definições inválidas", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dialog, IDC_GENERAL_DAILY_LIMIT));
            return TRUE;
        }

        settings->daily_limit = daily;
        settings->monthly_limit = monthly;
        settings->monthly_window_days = window_days;
        settings->loan_duration_days = duration;
        settings->audit_log_max_size_bytes =
            static_cast<unsigned long>(log_size_kb) * 1024UL;
        settings->diagnostics_enabled =
            SendDlgItemMessageW(dialog, IDC_GENERAL_DIAGNOSTICS,
                                BM_GETCHECK, 0, 0) == BST_CHECKED;
        EndDialog(dialog, IDOK);
        return TRUE;
    }

    if (message == WM_COMMAND && LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void ShowGeneralSettings()
{
    if (!gSettingsFileValid) {
        RecordApplicationDiagnostic(
            "warning", "settings", "reason=settings_file_invalid");
        MessageBoxW(gMainWindow,
            L"O ficheiro config/settings.ini é inválido e foi preservado. "
            L"As definições não podem ser gravadas até o ficheiro ser corrigido.",
            L"Definições", MB_OK | MB_ICONWARNING);
        return;
    }

    AppSettings before = gSettings;
    if (gGeneralSettingsPending) {
        CopyGeneralSettings(gPendingGeneralSettings, &before);
    }
    AppSettings edited = before;

    const INT_PTR result = DialogBoxParamW(
        gInstance, MAKEINTRESOURCEW(IDD_GENERAL_SETTINGS_DIALOG),
        gMainWindow, GeneralSettingsDialogProc,
        reinterpret_cast<LPARAM>(&edited));
    if (result != IDOK) return;

    const bool changed =
        edited.daily_limit != before.daily_limit ||
        edited.monthly_limit != before.monthly_limit ||
        edited.monthly_window_days != before.monthly_window_days ||
        edited.loan_duration_days != before.loan_duration_days ||
        edited.diagnostics_enabled != before.diagnostics_enabled ||
        edited.audit_log_max_size_bytes != before.audit_log_max_size_bytes;
    if (!changed) return;

    AppSettings persisted = gSettings;
    CopyGeneralSettings(edited, &persisted);

    std::string error;
    if (!biblioled::SaveSettingsFile(WideToUtf8(gApplicationPaths.settings_file), persisted, &error)) {
        ShowStoreError(L"Não foi possível guardar as definições gerais.", error);
        return;
    }

    gPendingGeneralSettings = edited;
    gGeneralSettingsPending = true;

    if (!gStore.MigrationRequired()) {
        std::ostringstream details;
        details << "daily_limit=" << before.daily_limit << "->" << edited.daily_limit
                << "; monthly_limit=" << before.monthly_limit << "->" << edited.monthly_limit
                << "; monthly_window_days=" << before.monthly_window_days
                << "->" << edited.monthly_window_days
                << "; loan_duration_days=" << before.loan_duration_days
                << "->" << edited.loan_duration_days
                << "; diagnostics_enabled=" << (before.diagnostics_enabled ? 1 : 0)
                << "->" << (edited.diagnostics_enabled ? 1 : 0)
                << "; audit_log_max_size_bytes=" << before.audit_log_max_size_bytes
                << "->" << edited.audit_log_max_size_bytes;

        error.clear();
        if (!gStore.RecordAuditEvent("settings_changed", details.str(), &error)) {
            ShowStoreError(
                L"As definições foram guardadas, mas não foi possível registar a alteração na auditoria.",
                error);
            return;
        }
        SynchronizeExternalAuditLog(true);
    }

    MessageBoxW(gMainWindow,
        L"As definições gerais foram guardadas em config/settings.ini. "
        L"Os novos limites e opções de registo serão aplicados na próxima execução.",
        L"Definições guardadas", MB_OK | MB_ICONINFORMATION);
}

void ShowBackupSettings()
{
    if (!gSettingsFileValid) {
        RecordApplicationDiagnostic(
            "warning", "settings", "reason=settings_file_invalid");
        MessageBoxW(gMainWindow,
            L"O ficheiro config/settings.ini é inválido e foi preservado. "
            L"As definições não podem ser gravadas até o ficheiro ser corrigido.",
            L"Definições", MB_OK | MB_ICONWARNING);
        return;
    }
    if (gStore.ExternalAuditLogBlocked()) {
        RecordApplicationDiagnostic(
            "warning", "backup_settings", "reason=external_audit_log_unavailable");
        MessageBoxW(gMainWindow,
            L"O registo de auditoria externo está indisponível. "
            L"Recupere primeiro a escrita do log antes de alterar definições.",
            L"Definições", MB_OK | MB_ICONWARNING);
        return;
    }

    AppSettings edited = gSettings;
    const INT_PTR result = DialogBoxParamW(
        gInstance, MAKEINTRESOURCEW(IDD_BACKUP_SETTINGS_DIALOG),
        gMainWindow, BackupSettingsDialogProc,
        reinterpret_cast<LPARAM>(&edited));
    if (result != IDOK) return;

    if (edited.backup_compression_format == gSettings.backup_compression_format &&
        edited.backup_compression_level == gSettings.backup_compression_level) {
        return;
    }

    const std::string old_format = gSettings.backup_compression_format;
    const int old_level = gSettings.backup_compression_level;
    AppSettings persisted = edited;
    if (gGeneralSettingsPending) {
        CopyGeneralSettings(gPendingGeneralSettings, &persisted);
    }

    std::string error;
    if (!biblioled::SaveSettingsFile(WideToUtf8(gApplicationPaths.settings_file), persisted, &error)) {
        ShowStoreError(L"Não foi possível guardar as definições de cópias de segurança.", error);
        return;
    }

    gSettings.backup_compression_format = edited.backup_compression_format;
    gSettings.backup_compression_level = edited.backup_compression_level;
    gStore.SetBackupCompression(
        gSettings.backup_compression_format,
        gSettings.backup_compression_level);

    if (!gStore.MigrationRequired()) {
        std::ostringstream details;
        details << "backup_compression_format=" << old_format << "->"
                << gSettings.backup_compression_format
                << "; backup_compression_level=" << old_level << "->"
                << gSettings.backup_compression_level;
        error.clear();
        if (!gStore.RecordAuditEvent("settings_changed", details.str(), &error)) {
            ShowStoreError(L"As definições foram guardadas, mas não foi possível registar a alteração na auditoria.", error);
            return;
        }
        SynchronizeExternalAuditLog(true);
    }

    MessageBoxW(gMainWindow,
        L"As definições foram guardadas e aplicadas imediatamente às próximas cópias "
        L"de segurança que forem criadas. Uma cópia automática válida já existente "
        L"para hoje será mantida.",
        L"Definições guardadas", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_GETMINMAXINFO:
        if (lParam) {
            MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = 700;
            limits->ptMinTrackSize.y = 450;
            return 0;
        }
        break;
    case WM_CREATE:
        CreateChildControls(window);
        UpdateWriteAccess(window);
        LayoutControls(window);
        RefreshList();
        return 0;

    case WM_SIZE:
        LayoutControls(window);
        return 0;

    case kPersistColumnsMessage:
        SaveColumnLayout();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_NEW:
        case IDC_BUTTON_NEW:
            AddLoan();
            return 0;
        case IDM_EDIT:
        case IDC_BUTTON_EDIT:
            EditLoan();
            return 0;
        case IDM_DELETE:
        case IDC_BUTTON_DELETE:
            DeleteLoan();
            return 0;
        case IDM_HISTORY:
            biblioled::win32::ShowHistoryDialog(gInstance,gMainWindow,&gStore);
            return 0;
        case IDM_STATISTICS:
            biblioled::win32::ShowStatisticsDialog(gInstance,gMainWindow,&gStore);
            return 0;
        case IDM_RETRY_AUDIT_SYNC:
            SynchronizeExternalAuditLog(true);
            return 0;
        case IDM_EXIT:
            DestroyWindow(window);
            return 0;
        case IDM_BACKUP:
            CreateManualDatabaseBackup();
            return 0;
        case IDM_RESTORE:
            RestoreDatabaseFromBackup();
            return 0;
        case IDM_GENERAL_SETTINGS:
            ShowGeneralSettings();
            return 0;
        case IDM_EXPORT_CSV:
            ExportLoansToCsv();
            return 0;
        case IDM_BACKUP_SETTINGS:
            ShowBackupSettings();
            return 0;
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        }
        break;

    case WM_NOTIFY:
        if (lParam) {
            const NMHDR* notification = reinterpret_cast<const NMHDR*>(lParam);
            if (notification->idFrom == IDC_LOAN_LIST && notification->code == LVN_COLUMNCLICK) {
                const NMLISTVIEW* click = reinterpret_cast<const NMLISTVIEW*>(lParam);
                if (click && click->iSubItem >= 0 && click->iSubItem < kColumnCount) {
                    const int selected_id = SelectedRecordId();
                    if (click->iSubItem == gMainSortColumn) gMainSortAscending = !gMainSortAscending;
                    else { gMainSortColumn = click->iSubItem; gMainSortAscending = true; }
                    SortMainLoans();
                    PopulateMainLoanList();
                    SelectMainRecord(selected_id);
                }
                return 0;
            }
        }
        if (gLoanList &&
            reinterpret_cast<NMHDR*>(lParam)->hwndFrom == ListView_GetHeader(gLoanList)) {
            const UINT code = reinterpret_cast<NMHDR*>(lParam)->code;
            if (code == HDN_ENDDRAG || code == HDN_ENDTRACKW) {
                PostMessageW(window, kPersistColumnsMessage, 0, 0);
                return FALSE;
            }
        }
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_LOAN_LIST &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_KEYDOWN) {
            const NMLVKEYDOWN* key = reinterpret_cast<const NMLVKEYDOWN*>(lParam);
            if (key->wVKey == VK_DELETE) {
                DeleteLoan();
                return 0;
            }
            if (key->wVKey == 'N' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                AddLoan();
                return 0;
            }
            if (key->wVKey == 'H' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                biblioled::win32::ShowHistoryDialog(gInstance,window,&gStore);
                return 0;
            }
        }
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == IDC_LOAN_LIST &&
            reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMACTIVATE) {
            EditLoan();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (wParam == 'N' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            AddLoan();
            return 0;
        }
        if (wParam == 'H' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            biblioled::win32::ShowHistoryDialog(gInstance,window,&gStore);
            return 0;
        }
        if (wParam == VK_DELETE && GetFocus() == gLoanList) {
            DeleteLoan();
            return 0;
        }
        break;

    case biblioled::win32::kHistoryEditMessage:
        return EditLoanById(static_cast<int>(wParam),
            lParam ? reinterpret_cast<HWND>(lParam) : window) ? 1 : 0;
    case biblioled::win32::kHistoryDeleteMessage:
        return DeleteLoanById(static_cast<int>(wParam),
            lParam ? reinterpret_cast<HWND>(lParam) : window,false) ? 1 : 0;
    case kActivateMessage:
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return 0;

    case WM_DESTROY:
        SaveColumnLayout();
        if (gStore.IsOpen() && !gStore.MigrationRequired()) {
            std::string shutdown_error;
            if (gStore.RecordAuditEvent(
                    "shutdown",
                    std::string("application_version=") + BIBLIOLED_VERSION,
                    &shutdown_error)) {
                SynchronizeExternalAuditLog(false);
            } else {
                RecordApplicationDiagnostic(
                    "failure", "application_shutdown", shutdown_error);
            }
        }
        gStore.Close();
        if (gInstanceMutex) {
            ReleaseMutex(gInstanceMutex);
            CloseHandle(gInstanceMutex);
            gInstanceMutex = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

unsigned long DirectoryHash(const std::wstring& value)
{
    unsigned long hash = 2166136261UL;
    for (size_t i = 0; i < value.size(); ++i) {
        hash ^= static_cast<unsigned long>(value[i]);
        hash *= 16777619UL;
    }
    return hash;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command)
{
    gInstance = instance;
    std::wstring path_error;
    if (!ResolveApplicationPaths(&gApplicationPaths, &path_error)) {
        MessageBoxW(NULL, path_error.c_str(), L"BiblioLED",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t mutex_name[80] = {0};
    _snwprintf(mutex_name, 79, L"Local\\BiblioLED_%08lX",
               DirectoryHash(gApplicationPaths.application_directory));

    SetLastError(ERROR_SUCCESS);
    gInstanceMutex = CreateMutexW(NULL, TRUE, mutex_name);
    const DWORD mutex_status = GetLastError();
    if (!gInstanceMutex) {
        std::wostringstream message;
        message << L"Não foi possível garantir a execução de uma única instância "
                << L"do BiblioLED nesta pasta. A aplicação não será iniciada.\n\n"
                << L"Código de erro do Windows: " << mutex_status << L".";
        MessageBoxW(NULL, message.str().c_str(), L"BiblioLED",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    if (mutex_status == ERROR_ALREADY_EXISTS) {
        HWND existing = NULL;
        for (int attempt = 0; attempt < 20 && !existing; ++attempt) {
            existing = FindWindowW(kWindowClass, NULL);
            if (!existing) Sleep(50);
        }
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(gInstanceMutex);
        gInstanceMutex = NULL;
        return 0;
    }

    const std::wstring required_directories[] = {
        gApplicationPaths.config_directory,
        gApplicationPaths.data_directory,
        gApplicationPaths.backup_directory,
        gApplicationPaths.export_directory,
        gApplicationPaths.log_directory
    };
    for (size_t i = 0;
         i < sizeof(required_directories) / sizeof(required_directories[0]); ++i) {
        if (!EnsureDirectory(required_directories[i])) {
            std::wstring message =
                L"Não foi possível criar ou utilizar a pasta necessária:\n\n";
            message += required_directories[i];
            message +=
                L"\n\nConfirme que o caminho não está ocupado por um ficheiro "
                L"e que a aplicação tem permissões de escrita.";
            MessageBoxW(NULL, message.c_str(), L"BiblioLED",
                        MB_OK | MB_ICONERROR);
            if (gInstanceMutex) {
                ReleaseMutex(gInstanceMutex);
                CloseHandle(gInstanceMutex);
                gInstanceMutex = NULL;
            }
            return 1;
        }
    }

    std::string settings_error;
    const std::string settings_path_utf8 =
        WideToUtf8(gApplicationPaths.settings_file);
    biblioled::SettingsLoadStatus settings_status = biblioled::kSettingsLoadError;
    gSettingsFileValid = biblioled::LoadSettingsFile(
        settings_path_utf8, &gSettings, &settings_error, &settings_status);

    bool settings_recreated = false;
    std::string archived_settings_path;
    if (settings_status == biblioled::kSettingsLoadInvalidFile) {
        std::wstring prompt =
            L"O ficheiro config/settings.ini é inválido e foi preservado.\n\n";
        prompt += Utf8ToWide(settings_error);
        prompt +=
            L"\n\nNesta sessão serão usados valores predefinidos. "
            L"Pretende arquivar o original com o sufixo .invalid e criar "
            L"um novo ficheiro com as definições predefinidas?";
        const int answer = MessageBoxW(
            NULL, prompt.c_str(), L"Recuperar definições",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

        if (answer == IDYES) {
            std::string archive_error;
            if (biblioled::ArchiveInvalidSettingsFile(
                    settings_path_utf8, &archived_settings_path, &archive_error)) {
                settings_error.clear();
                settings_status = biblioled::kSettingsLoadError;
                gSettingsFileValid = biblioled::LoadSettingsFile(
                    settings_path_utf8, &gSettings, &settings_error,
                    &settings_status);
                settings_recreated = gSettingsFileValid &&
                    settings_status == biblioled::kSettingsLoadCreatedDefaults;

                if (settings_recreated) {
                    std::wstring archived_message =
                        L"O ficheiro inválido foi preservado em:\n\n";
                    archived_message += Utf8ToWide(archived_settings_path);
                    archived_message +=
                        L"\n\nFoi criado um novo config/settings.ini com valores predefinidos.";
                    MessageBoxW(NULL, archived_message.c_str(), L"Definições recriadas",
                                MB_OK | MB_ICONINFORMATION);
                } else {
                    std::wstring recreate_error =
                        L"O ficheiro original foi arquivado, mas não foi possível criar "
                        L"o novo config/settings.ini:\n\n";
                    recreate_error += Utf8ToWide(settings_error);
                    MessageBoxW(NULL, recreate_error.c_str(), L"Recuperação de definições",
                                MB_OK | MB_ICONERROR);
                }
            } else {
                std::wstring archive_failure =
                    L"Não foi possível arquivar o ficheiro inválido. O original foi preservado.\n\n";
                archive_failure += Utf8ToWide(archive_error);
                MessageBoxW(NULL, archive_failure.c_str(), L"Recuperação de definições",
                            MB_OK | MB_ICONERROR);
                settings_error = archive_error;
            }
        }
    } else if (!gSettingsFileValid && !settings_error.empty()) {
        MessageBoxW(NULL, Utf8ToWide(settings_error).c_str(), L"BiblioLED",
                    MB_OK | MB_ICONWARNING);
    }

    const std::wstring& database_path = gApplicationPaths.database_file;
    const std::wstring& database_marker =
        gApplicationPaths.database_marker_file;
    const bool database_exists = FileExistsW(database_path);
    const bool database_marker_exists = FileExistsW(database_marker);

    const std::string database_utf8 = WideToUtf8(database_path);
    std::string database_error;
    const std::wstring backup_directory =
        gApplicationPaths.backup_directory;
    const std::string backup_directory_utf8 = WideToUtf8(backup_directory);
    std::string recovery_backup_path;
    std::string recovery_preserved_path;
    bool recovery_performed = false;

    if (!database_exists && database_marker_exists) {
        std::wstring reason = L"O ficheiro esperado não foi encontrado:\n\n";
        reason += database_path;
        if (!PromptAndRecoverDatabase(
                database_path, backup_directory, reason,
                &recovery_backup_path, &recovery_preserved_path)) {
            if (gInstanceMutex) CloseHandle(gInstanceMutex);
            return 1;
        }
        recovery_performed = true;
    }

    if (!recovery_performed &&
        !gStore.Open(database_utf8, backup_directory_utf8, &database_error)) {
        std::wstring reason =
            L"Não foi possível abrir a base de dados esperada:\n\n";
        reason += Utf8ToWide(database_error);
        if (!PromptAndRecoverDatabase(
                database_path, backup_directory, reason,
                &recovery_backup_path, &recovery_preserved_path)) {
            if (gInstanceMutex) CloseHandle(gInstanceMutex);
            return 1;
        }
        recovery_performed = true;
    }

    if (recovery_performed) {
        database_error.clear();
        if (!gStore.Open(database_utf8, backup_directory_utf8, &database_error)) {
            std::wstring message =
                L"A cópia foi instalada, mas a base de dados recuperada "
                L"não abriu correctamente:\n\n";
            message += Utf8ToWide(database_error);
            MessageBoxW(NULL, message.c_str(), L"BiblioLED",
                        MB_OK | MB_ICONERROR);
            if (gInstanceMutex) CloseHandle(gInstanceMutex);
            return 1;
        }
    }
    gStore.SetBackupCompression(
        gSettings.backup_compression_format,
        gSettings.backup_compression_level);

    if (!database_marker_exists) {
        std::wstring marker_error;
        if (!CreateDatabaseMarker(database_marker, &marker_error)) {
            RecordApplicationDiagnostic(
                "failure", "create_database_marker", WideToUtf8(marker_error));
            MessageBoxW(NULL, marker_error.c_str(), L"BiblioLED",
                        MB_OK | MB_ICONERROR);
            gStore.Close();
            if (gInstanceMutex) CloseHandle(gInstanceMutex);
            return 1;
        }
    }

    OfferLegacyMigration(NULL);
    if (!gSettingsFileValid && !settings_error.empty() &&
        !gStore.MigrationRequired()) {
        RecordApplicationDiagnostic(
            "warning", "startup_settings", settings_error);
    }
    if (settings_recreated && !gStore.MigrationRequired()) {
        std::string settings_audit_error;
        if (gStore.RecordAuditEvent(
                "settings_recreated",
                "archived=" + archived_settings_path,
                &settings_audit_error)) {
            SynchronizeExternalAuditLog(false);
        } else {
            RecordApplicationDiagnostic(
                "failure", "settings_recreated_audit", settings_audit_error);
        }
    }
    if (recovery_performed) {
        std::wstring recovered_message =
            L"A base de dados foi recuperada e validada a partir da cópia seleccionada.\n\n";
        recovered_message += L"Cópia utilizada:\n";
        recovered_message += Utf8ToWide(recovery_backup_path);
        if (!recovery_preserved_path.empty()) {
            recovered_message += L"\n\nOs ficheiros anteriores (base de dados e eventuais auxiliares) foram preservados com este prefixo:\n";
            recovered_message += Utf8ToWide(recovery_preserved_path);
        }
        MessageBoxW(NULL, recovered_message.c_str(), L"Recuperação concluída",
                    MB_OK | MB_ICONINFORMATION);
    }
    if (recovery_performed && !gStore.MigrationRequired()) {
        std::string recovery_error;
        std::string details = "source=" + recovery_backup_path +
            "; preserved=" + recovery_preserved_path;
        if (!gStore.RecordAuditEvent("database_recovery", details, &recovery_error)) {
            ShowStoreError(
                L"A base de dados foi recuperada, mas não foi possível registar a operação na auditoria.",
                recovery_error);
        }
    }
    if (!gStore.MigrationRequired()) {
        std::string startup_error;
        if (!gStore.RecordAuditEvent(
                "startup",
                std::string("application_version=") + BIBLIOLED_VERSION,
                &startup_error)) {
            ShowStoreError(L"Não foi possível registar o início da sessão na auditoria.", startup_error);
        }
    }
    SynchronizeExternalAuditLog(true);

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&controls);

    gApplicationIcon = LoadIconW(gInstance, MAKEINTRESOURCEW(IDI_BIBLIOLED));

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = MainWindowProc;
    window_class.hInstance = gInstance;
    window_class.hIcon = gApplicationIcon ? gApplicationIcon : LoadIconW(NULL, IDI_APPLICATION);
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = window_class.hIcon;

    if (!RegisterClassExW(&window_class)) {
        RecordApplicationDiagnostic(
            "failure", "register_main_window_class",
            "RegisterClassExW returned failure.");
        MessageBoxW(NULL, L"Não foi possível registar a janela principal.",
                    L"BiblioLED", MB_OK | MB_ICONERROR);
        gStore.Close();
        if (gInstanceMutex) CloseHandle(gInstanceMutex);
        return 1;
    }

    std::wstring title = L"BiblioLED v";
    title += Utf8ToWide(BIBLIOLED_VERSION);

    gMainWindow = CreateWindowExW(
        0, kWindowClass, title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 520,
        NULL, CreateApplicationMenu(), gInstance, NULL);

    if (!gMainWindow) {
        RecordApplicationDiagnostic(
            "failure", "create_main_window",
            "CreateWindowExW returned a null window handle.");
        MessageBoxW(NULL, L"Não foi possível criar a janela principal.",
                    L"BiblioLED", MB_OK | MB_ICONERROR);
        gStore.Close();
        if (gInstanceMutex) CloseHandle(gInstanceMutex);
        return 1;
    }

    if (gApplicationIcon) {
        SendMessageW(gMainWindow, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(gApplicationIcon));
        SendMessageW(gMainWindow, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(gApplicationIcon));
    }

    ACCEL accelerators[2] = {};
    accelerators[0].fVirt = FVIRTKEY | FCONTROL;
    accelerators[0].key = 'N';
    accelerators[0].cmd = IDM_NEW;
    accelerators[1].fVirt = FVIRTKEY | FCONTROL;
    accelerators[1].key = 'H';
    accelerators[1].cmd = IDM_HISTORY;
    gMainAccelerators = CreateAcceleratorTableW(accelerators, 2);

    ShowWindow(gMainWindow, show_command);
    UpdateWindow(gMainWindow);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (gMainAccelerators &&
            TranslateAcceleratorW(gMainWindow, gMainAccelerators, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (gMainAccelerators) {
        DestroyAcceleratorTable(gMainAccelerators);
        gMainAccelerators = NULL;
    }
    return static_cast<int>(message.wParam);
}
