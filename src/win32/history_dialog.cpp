#include "history_dialog.h"
#include "biblioled/loan_store.h"
#include "resource.h"
#include <commctrl.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {
struct HistoryRow {
    int id;
    std::wstring id_text, title, type, type_key, date, date_sort_key, search_key;
    bool date_sort_valid;
    HistoryRow() : id(0), date_sort_valid(false) {}
};
struct HistoryContext {
    HWND owner;
    biblioled::LoanStore* store;
    std::vector<HistoryRow> rows;
    unsigned int invalid_dates;
    int sort_column;
    bool sort_ascending;
    HistoryContext() : owner(NULL), store(NULL), invalid_dates(0),
        sort_column(-1), sort_ascending(true) {}
};

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
void RecordHistoryFailure(HistoryContext* context,
                          const char* operation,
                          const std::string& error)
{
    if (!context || !context->store || !context->store->IsOpen() ||
        context->store->MigrationRequired()) {
        return;
    }

    const std::string details =
        std::string("context=") + operation + ";message=" + error;
    std::string ignored;
    context->store->RecordDiagnosticEvent("failure", details, &ignored);
}
std::wstring Lowercase(std::wstring value)
{
    if (!value.empty()) CharLowerBuffW(&value[0], static_cast<DWORD>(value.size()));
    return value;
}
bool HasDateTimeShape(const std::string& value)
{
    if (value.size() < 16 || value[4] != '-' || value[7] != '-' ||
        value[10] != ' ' || value[13] != ':') return false;
    for (size_t i=0; i<16; ++i) {
        if (i==4 || i==7 || i==10 || i==13) continue;
        if (value[i]<'0' || value[i]>'9') return false;
    }
    return true;
}
std::wstring DisplayDate(const std::string& value)
{
    if (HasDateTimeShape(value)) {
        return Utf8ToWide(value.substr(8,2)+"/"+value.substr(5,2)+"/"+
                          value.substr(0,4)+value.substr(10));
    }
    return Utf8ToWide(value);
}
int CompareTextInsensitive(const std::wstring& left, const std::wstring& right)
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
bool HistoryRowLess(const HistoryRow& left, const HistoryRow& right, const HistoryContext* context)
{
    int comparison = 0;
    switch (context->sort_column) {
    case 0: comparison = left.id < right.id ? -1 : (left.id > right.id ? 1 : 0); break;
    case 1: comparison = CompareTextInsensitive(left.title, right.title); break;
    case 2: comparison = CompareTextInsensitive(left.type, right.type); break;
    case 3:
        if (left.date_sort_valid != right.date_sort_valid) return left.date_sort_valid;
        comparison = CompareTextInsensitive(left.date_sort_key, right.date_sort_key);
        break;
    default: comparison = 0; break;
    }
    if (comparison == 0) return left.id < right.id;
    return context->sort_ascending ? comparison < 0 : comparison > 0;
}
void SortHistoryRows(HistoryContext* context)
{
    if (!context) return;
    std::sort(context->rows.begin(), context->rows.end(),
        [context](const HistoryRow& left, const HistoryRow& right) {
            return HistoryRowLess(left, right, context);
        });
}
bool LoadRows(HistoryContext* context, std::string* error)
{
    if (!context || !context->store) {
        if (error) *error = "Invalid history context.";
        return false;
    }
    std::vector<biblioled::LoanRecord> records;
    if (!context->store->GetAll(&records,error)) return false;
    context->rows.clear();
    context->rows.reserve(records.size());
    context->invalid_dates=0;
    for (size_t i=0;i<records.size();++i) {
        const biblioled::LoanRecord& record=records[i];
        HistoryRow row;
        row.id=record.id;
        wchar_t id[24]={0};
        _snwprintf(id,23,L"%d",record.id);
        row.id_text=id;
        row.title=Utf8ToWide(record.title);
        row.type_key=Lowercase(Utf8ToWide(record.type));
        if (row.type_key==L"audiolivro") row.type=L"Audiolivro";
        else if (row.type_key==L"livro") row.type=L"Livro";
        else row.type=Utf8ToWide(record.type);
        row.date=DisplayDate(record.loan_date);
        row.date_sort_key=Utf8ToWide(record.loan_date);
        row.date_sort_valid=HasDateTimeShape(record.loan_date);
        if (!row.date_sort_valid) ++context->invalid_dates;
        row.search_key=Lowercase(row.title+L" "+row.type+L" "+row.date+L" "+row.id_text);
        context->rows.push_back(row);
    }
    return true;
}
std::wstring ControlText(HWND dialog,int id)
{
    HWND control=GetDlgItem(dialog,id);
    const int length=control?GetWindowTextLengthW(control):0;
    std::wstring value(static_cast<size_t>(length)+1,L'\0');
    if (control && length>0) GetWindowTextW(control,&value[0],length+1);
    value.resize(static_cast<size_t>(length));
    return value;
}
void UpdateActions(HWND dialog,const HistoryContext* context)
{
    const BOOL enabled=context && context->store &&
        !context->store->MigrationRequired() &&
        !context->store->ExternalAuditLogBlocked();
    EnableWindow(GetDlgItem(dialog,IDC_HISTORY_EDIT),enabled);
    EnableWindow(GetDlgItem(dialog,IDC_HISTORY_DELETE),enabled);
}
void RefreshList(HWND dialog,const HistoryContext* context)
{
    if (!dialog || !context) return;
    const std::wstring query=Lowercase(ControlText(dialog,IDC_HISTORY_SEARCH));
    const LRESULT kind=SendDlgItemMessageW(dialog,IDC_HISTORY_TYPE,CB_GETCURSEL,0,0);
    HWND list=GetDlgItem(dialog,IDC_HISTORY_LIST);
    ListView_DeleteAllItems(list);
    unsigned int shown=0;
    for(size_t i=0;i<context->rows.size();++i) {
        const HistoryRow& row=context->rows[i];
        if(kind==1 && row.type_key!=L"livro") continue;
        if(kind==2 && row.type_key!=L"audiolivro") continue;
        if(!query.empty() && row.search_key.find(query)==std::wstring::npos) continue;
        LVITEMW item={};
        item.mask=LVIF_TEXT|LVIF_PARAM;
        item.iItem=static_cast<int>(shown);
        item.pszText=const_cast<LPWSTR>(row.id_text.c_str());
        item.lParam=static_cast<LPARAM>(row.id);
        const int inserted=ListView_InsertItem(list,&item);
        if(inserted>=0) {
            ListView_SetItemText(list,inserted,1,const_cast<LPWSTR>(row.title.c_str()));
            ListView_SetItemText(list,inserted,2,const_cast<LPWSTR>(row.type.c_str()));
            ListView_SetItemText(list,inserted,3,const_cast<LPWSTR>(row.date.c_str()));
            ++shown;
        }
    }
    wchar_t status[256]={0};
    _snwprintf(status,255,L"%u de %u registo(s)",shown,
               static_cast<unsigned int>(context->rows.size()));
    if(context->invalid_dates>0) {
        const size_t used=wcslen(status);
        _snwprintf(status+used,255-used,L"  |  %u data(s) não reconhecida(s)",
                   context->invalid_dates);
    }
    SetDlgItemTextW(dialog,IDC_HISTORY_STATUS,status);
    UpdateActions(dialog,context);
}
int SelectedId(HWND dialog)
{
    HWND list=GetDlgItem(dialog,IDC_HISTORY_LIST);
    const int selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
    if(selected<0) return 0;
    LVITEMW item={};
    item.mask=LVIF_PARAM;
    item.iItem=selected;
    return ListView_GetItem(list,&item)?static_cast<int>(item.lParam):0;
}
void SelectHistoryId(HWND dialog, int record_id)
{
    if (record_id <= 0) return;
    HWND list=GetDlgItem(dialog,IDC_HISTORY_LIST);
    for (int i=0;i<ListView_GetItemCount(list);++i) {
        LVITEMW item={};
        item.mask=LVIF_PARAM;
        item.iItem=i;
        if(ListView_GetItem(list,&item) && static_cast<int>(item.lParam)==record_id) {
            ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
            ListView_SetItemState(list,i,LVIS_SELECTED|LVIS_FOCUSED,
                                  LVIS_SELECTED|LVIS_FOCUSED);
            ListView_EnsureVisible(list,i,FALSE);
            return;
        }
    }
}
void Reload(HWND dialog,HistoryContext* context)
{
    std::string error;
    if(!LoadRows(context,&error)) {
        RecordHistoryFailure(context, "history_refresh", error);
        MessageBoxW(dialog,(L"Não foi possível actualizar o histórico.\n\n"+
            Utf8ToWide(error)).c_str(),L"BiblioLED",MB_OK|MB_ICONERROR);
        return;
    }
    SortHistoryRows(context);
    RefreshList(dialog,context);
}
void EditSelected(HWND dialog,HistoryContext* context)
{
    const int id=SelectedId(dialog);
    if(id<=0) {
        MessageBoxW(dialog,L"Seleccione um registo do histórico.",
                    L"Histórico",MB_OK|MB_ICONINFORMATION);
        return;
    }
    if(SendMessageW(context->owner,biblioled::win32::kHistoryEditMessage,
                    static_cast<WPARAM>(id),reinterpret_cast<LPARAM>(dialog)))
        Reload(dialog,context);
    UpdateActions(dialog,context);
}
void DeleteSelected(HWND dialog,HistoryContext* context)
{
    const int id=SelectedId(dialog);
    if(id<=0) {
        MessageBoxW(dialog,L"Seleccione um registo do histórico.",
                    L"Histórico",MB_OK|MB_ICONINFORMATION);
        return;
    }
    if(MessageBoxW(dialog,L"Eliminar o registo seleccionado?",
        L"Confirmar eliminação",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES) return;
    if(SendMessageW(context->owner,biblioled::win32::kHistoryDeleteMessage,
                    static_cast<WPARAM>(id),reinterpret_cast<LPARAM>(dialog)))
        Reload(dialog,context);
    UpdateActions(dialog,context);
}

void MoveControlDlu(HWND dialog,int id,int x,int y,int width,int height)
{
    RECT bounds={x,y,x+width,y+height};
    if(!MapDialogRect(dialog,&bounds)) return;
    MoveWindow(GetDlgItem(dialog,id),bounds.left,bounds.top,
               bounds.right-bounds.left,bounds.bottom-bounds.top,TRUE);
}

void LayoutControls(HWND dialog)
{
    RECT client={};
    GetClientRect(dialog,&client);
    RECT base_units={0,0,4,8};
    if(!MapDialogRect(dialog,&base_units) ||
       base_units.right<=0 || base_units.bottom<=0) return;

    const int width_px=client.right-client.left;
    const int height_px=client.bottom-client.top;
    const int width_dlu=(width_px*4+base_units.right/2)/base_units.right;
    const int height_dlu=(height_px*8+base_units.bottom/2)/base_units.bottom;
    const int margin=8;
    const int type_width=112;
    const int search_x=50;
    const int search_width=width_dlu-search_x-margin-type_width-8;

    MoveControlDlu(dialog,IDC_HISTORY_SEARCH,search_x,7,
                   search_width>80?search_width:80,14);
    MoveControlDlu(dialog,IDC_HISTORY_TYPE,
                   width_dlu-margin-type_width,6,type_width,60);
    MoveControlDlu(dialog,IDC_HISTORY_LIST,margin,26,
                   width_dlu-2*margin,
                   height_dlu-26-30>0?height_dlu-26-30:1);

    const int button_y=height_dlu-27;
    const int close_width=36;
    const int delete_width=48;
    const int edit_width=42;
    const int close_x=width_dlu-margin-close_width;
    const int delete_x=close_x-6-delete_width;
    const int edit_x=delete_x-6-edit_width;
    MoveControlDlu(dialog,IDC_HISTORY_STATUS,margin,button_y+3,
                   edit_x-margin-10>0?edit_x-margin-10:1,12);
    MoveControlDlu(dialog,IDC_HISTORY_EDIT,edit_x,button_y,edit_width,18);
    MoveControlDlu(dialog,IDC_HISTORY_DELETE,delete_x,button_y,delete_width,18);
    MoveControlDlu(dialog,IDOK,close_x,button_y,close_width,18);
}

void ShowHistoryContextMenu(HWND dialog,HistoryContext* context,POINT screen_point)
{
    if(!context) return;
    HWND list=GetDlgItem(dialog,IDC_HISTORY_LIST);
    if(screen_point.x==-1 && screen_point.y==-1) {
        const int selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        if(selected>=0) {
            RECT item={};
            if(ListView_GetItemRect(list,selected,&item,LVIR_BOUNDS)) {
                screen_point.x=item.left+(item.right-item.left)/2;
                screen_point.y=item.top+(item.bottom-item.top)/2;
                ClientToScreen(list,&screen_point);
            } else {
                GetCursorPos(&screen_point);
            }
        } else {
            GetCursorPos(&screen_point);
        }
    } else {
        POINT client_point=screen_point;
        ScreenToClient(list,&client_point);
        LVHITTESTINFO hit={};
        hit.pt=client_point;
        const int index=ListView_HitTest(list,&hit);
        if(index>=0) {
            ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
            ListView_SetItemState(list,index,LVIS_SELECTED|LVIS_FOCUSED,
                                  LVIS_SELECTED|LVIS_FOCUSED);
        }
    }

    const BOOL can_edit=context->store &&
        !context->store->MigrationRequired() &&
        !context->store->ExternalAuditLogBlocked();
    const BOOL has_selection=SelectedId(dialog)>0;
    HMENU popup=CreatePopupMenu();
    if(!popup) return;
    AppendMenuW(popup,MF_STRING|((can_edit&&has_selection)?MF_ENABLED:MF_GRAYED),
                IDC_HISTORY_EDIT,L"Editar");
    AppendMenuW(popup,MF_STRING|((can_edit&&has_selection)?MF_ENABLED:MF_GRAYED),
                IDC_HISTORY_DELETE,L"Eliminar");
    SetForegroundWindow(dialog);
    const UINT command=TrackPopupMenu(popup,
        TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,
        screen_point.x,screen_point.y,0,dialog,NULL);
    DestroyMenu(popup);
    if(command==IDC_HISTORY_EDIT) EditSelected(dialog,context);
    else if(command==IDC_HISTORY_DELETE) DeleteSelected(dialog,context);
}

INT_PTR CALLBACK HistoryProc(HWND dialog,UINT message,WPARAM wParam,LPARAM lParam)
{
    HistoryContext* context=reinterpret_cast<HistoryContext*>(
        GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG) {
        context=reinterpret_cast<HistoryContext*>(lParam);
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(context));
        SendDlgItemMessageW(dialog,IDC_HISTORY_TYPE,CB_ADDSTRING,0,
            reinterpret_cast<LPARAM>(L"Todos os tipos"));
        SendDlgItemMessageW(dialog,IDC_HISTORY_TYPE,CB_ADDSTRING,0,
            reinterpret_cast<LPARAM>(L"Livro"));
        SendDlgItemMessageW(dialog,IDC_HISTORY_TYPE,CB_ADDSTRING,0,
            reinterpret_cast<LPARAM>(L"Audiolivro"));
        SendDlgItemMessageW(dialog,IDC_HISTORY_TYPE,CB_SETCURSEL,0,0);
        HWND list=GetDlgItem(dialog,IDC_HISTORY_LIST);
        ListView_SetExtendedListViewStyle(list,
            LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER|
            LVS_EX_HEADERDRAGDROP);
        const wchar_t* labels[]={L"ID",L"Título",L"Tipo",L"Data e hora"};
        const int widths[]={48,245,88,130};
        for(int i=0;i<4;++i) {
            LVCOLUMNW column={};
            column.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
            column.pszText=const_cast<LPWSTR>(labels[i]);
            column.cx=widths[i];
            column.iSubItem=i;
            ListView_InsertColumn(list,i,&column);
        }
        std::string error;
        if(!LoadRows(context,&error)) {
            RecordHistoryFailure(context, "history_initial_load", error);
            MessageBoxW(dialog,(L"Não foi possível ler o histórico.\n\n"+
                Utf8ToWide(error)).c_str(),L"BiblioLED",MB_OK|MB_ICONERROR);
            EndDialog(dialog,IDCANCEL);
            return TRUE;
        }
        SortHistoryRows(context);
        RefreshList(dialog,context);
        LayoutControls(dialog);
        SetFocus(GetDlgItem(dialog,IDC_HISTORY_SEARCH));
        return FALSE;
    }
    if(message==WM_GETMINMAXINFO) {
        MINMAXINFO* limits=reinterpret_cast<MINMAXINFO*>(lParam);
        if(limits) {
            limits->ptMinTrackSize.x=640;
            limits->ptMinTrackSize.y=360;
            return TRUE;
        }
    }
    if(message==WM_SIZE && context) {
        LayoutControls(dialog);
        return TRUE;
    }
    if(message==WM_COMMAND && context) {
        const int id=LOWORD(wParam), notification=HIWORD(wParam);
        if(id==IDC_HISTORY_SEARCH && notification==EN_CHANGE) {
            RefreshList(dialog,context); return TRUE;
        }
        if(id==IDC_HISTORY_TYPE && notification==CBN_SELCHANGE) {
            RefreshList(dialog,context); return TRUE;
        }
        if(id==IDC_HISTORY_EDIT) { EditSelected(dialog,context); return TRUE; }
        if(id==IDC_HISTORY_DELETE) { DeleteSelected(dialog,context); return TRUE; }
        if(id==IDOK || id==IDCANCEL) { EndDialog(dialog,id); return TRUE; }
    }
    if(message==WM_NOTIFY && context) {
        const NMHDR* header=reinterpret_cast<const NMHDR*>(lParam);
        if(header && header->idFrom==IDC_HISTORY_LIST) {
            if(header->code==LVN_COLUMNCLICK) {
                const NMLISTVIEW* click=reinterpret_cast<const NMLISTVIEW*>(lParam);
                if(click && click->iSubItem>=0 && click->iSubItem<4) {
                    const int selected_id=SelectedId(dialog);
                    if(click->iSubItem==context->sort_column)
                        context->sort_ascending=!context->sort_ascending;
                    else {
                        context->sort_column=click->iSubItem;
                        context->sort_ascending=true;
                    }
                    SortHistoryRows(context);
                    RefreshList(dialog,context);
                    SelectHistoryId(dialog,selected_id);
                }
                return TRUE;
            }
            if(header->code==NM_RCLICK) {
                const NMLISTVIEW* click=reinterpret_cast<const NMLISTVIEW*>(lParam);
                POINT point=click?click->ptAction:POINT();
                ClientToScreen(GetDlgItem(dialog,IDC_HISTORY_LIST),&point);
                ShowHistoryContextMenu(dialog,context,point);
                return TRUE;
            }
            if(header->code==LVN_ITEMACTIVATE) {
                EditSelected(dialog,context); return TRUE;
            }
            if(header->code==LVN_KEYDOWN) {
                const NMLVKEYDOWN* key=reinterpret_cast<const NMLVKEYDOWN*>(lParam);
                if(key->wVKey==VK_RETURN) { EditSelected(dialog,context); return TRUE; }
                if(key->wVKey==VK_DELETE) { DeleteSelected(dialog,context); return TRUE; }
                if(key->wVKey==VK_F5) { Reload(dialog,context); return TRUE; }
            }
        }
    }
    return FALSE;
}
} // namespace

namespace biblioled {
namespace win32 {
void ShowHistoryDialog(HINSTANCE instance,HWND owner,LoanStore* store)
{
    if(!store) return;
    HistoryContext context;
    context.owner=owner;
    context.store=store;
    DialogBoxParamW(instance,MAKEINTRESOURCEW(IDD_HISTORY_DIALOG),owner,
                    HistoryProc,reinterpret_cast<LPARAM>(&context));
}
}
}
