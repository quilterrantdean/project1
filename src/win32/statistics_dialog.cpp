#include "statistics_dialog.h"
#include "biblioled/loan_statistics.h"
#include "biblioled/loan_rules.h"
#include "biblioled/loan_store.h"
#include "resource.h"
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct StatisticsContext {
    biblioled::LoanStore* store;
    std::vector<biblioled::LoanRecord> records;
    biblioled::LoanStatistics statistics;
    StatisticsContext() : store(NULL) {}
};

std::wstring Utf8ToWide(const std::string& value)
{
    if(value.empty()) return std::wstring();
    const int length=MultiByteToWideChar(CP_UTF8,0,value.c_str(),-1,NULL,0);
    if(length<=1) return std::wstring();
    std::wstring result(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.c_str(),-1,&result[0],length);
    result.resize(static_cast<size_t>(length-1));
    return result;
}
void RecordStatisticsDiagnostic(StatisticsContext* context,
                                const char* severity,
                                const char* operation,
                                const std::string& details)
{
    if (!context || !context->store || !context->store->IsOpen() ||
        context->store->MigrationRequired()) {
        return;
    }

    const std::string audit_details =
        std::string("context=") + operation + ";message=" + details;
    std::string ignored;
    context->store->RecordDiagnosticEvent(severity, audit_details, &ignored);
}
std::wstring Number(int value)
{
    wchar_t buffer[32]={0};
    _snwprintf(buffer,31,L"%d",value);
    return buffer;
}
std::wstring SignedNumber(int value)
{
    wchar_t buffer[32]={0};
    _snwprintf(buffer,31,L"%+d",value);
    return buffer;
}
std::wstring Decimal(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    std::string text=stream.str();
    for(size_t i=0;i<text.size();++i) if(text[i]=='.') text[i]=',';
    return std::wstring(text.begin(),text.end());
}
std::wstring DisplayDate(const std::string& value)
{
    if(value.size()>=10 && value[4]=='-' && value[7]=='-') {
        const std::string date=value.substr(8,2)+"/"+value.substr(5,2)+"/"+value.substr(0,4);
        return std::wstring(date.begin(),date.end());
    }
    return std::wstring(value.begin(),value.end());
}
void SetColumn(HWND list,int index,const wchar_t* title,int width)
{
    LVCOLUMNW column={};
    column.mask=LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
    column.pszText=const_cast<LPWSTR>(title);
    column.cx=width;
    column.iSubItem=index;
    ListView_InsertColumn(list,index,&column);
}
void InsertRow(HWND list,int index,const std::wstring& first,
               const std::wstring& second,const std::wstring& third=std::wstring(),
               const std::wstring& fourth=std::wstring())
{
    LVITEMW item={};
    item.mask=LVIF_TEXT;
    item.iItem=index;
    item.pszText=const_cast<LPWSTR>(first.c_str());
    const int inserted=ListView_InsertItem(list,&item);
    if(inserted<0) return;
    if(!second.empty()) ListView_SetItemText(list,inserted,1,const_cast<LPWSTR>(second.c_str()));
    if(!third.empty()) ListView_SetItemText(list,inserted,2,const_cast<LPWSTR>(third.c_str()));
    if(!fourth.empty()) ListView_SetItemText(list,inserted,3,const_cast<LPWSTR>(fourth.c_str()));
}
void InitList(HWND dialog,int id,const wchar_t* const* titles,const int* widths,int count)
{
    HWND list=GetDlgItem(dialog,id);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
    for(int i=0;i<count;++i) SetColumn(list,i,titles[i],widths[i]);
}
int CurrentYear()
{
    const std::string value=biblioled::FormatLocalDateTime(std::time(NULL),"%Y");
    return value.empty()?0:std::atoi(value.c_str());
}
int SelectedYear(HWND dialog)
{
    const LRESULT index=SendDlgItemMessageW(dialog,IDC_STATS_YEAR,CB_GETCURSEL,0,0);
    if(index==CB_ERR) return CurrentYear();
    const LRESULT year=SendDlgItemMessageW(dialog,IDC_STATS_YEAR,CB_GETITEMDATA,static_cast<WPARAM>(index),0);
    return year==CB_ERR?CurrentYear():static_cast<int>(year);
}
void PopulateYears(HWND dialog,const biblioled::LoanStatistics& stats)
{
    HWND combo=GetDlgItem(dialog,IDC_STATS_YEAR);
    SendMessageW(combo,CB_RESETCONTENT,0,0);
    int selected=CB_ERR;
    if(stats.annual.empty()) {
        const int year=CurrentYear();
        const std::wstring label=Number(year);
        const LRESULT index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if(index!=CB_ERR) {
            SendMessageW(combo,CB_SETITEMDATA,static_cast<WPARAM>(index),year);
            selected=static_cast<int>(index);
        }
    } else {
        for(std::vector<biblioled::AnnualLoanStatistics>::const_reverse_iterator it=stats.annual.rbegin();
            it!=stats.annual.rend();++it) {
            const std::wstring label=Number(it->year);
            const LRESULT index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(index==CB_ERR) continue;
            SendMessageW(combo,CB_SETITEMDATA,static_cast<WPARAM>(index),it->year);
            if(it->year==stats.selected_year) selected=static_cast<int>(index);
        }
    }
    if(selected!=CB_ERR) SendMessageW(combo,CB_SETCURSEL,static_cast<WPARAM>(selected),0);
}
void ShowPage(HWND dialog)
{
    const int tab=static_cast<int>(SendDlgItemMessageW(dialog,IDC_STATS_TABS,TCM_GETCURSEL,0,0));
    const bool general=tab==0;
    const int general_ids[]={IDC_STATS_TOTAL,IDC_STATS_TYPES,IDC_STATS_RANGE,
        IDC_STATS_COMPARISON,IDC_STATS_INVALID,IDC_STATS_ANNUAL};
    const int usage_ids[]={IDC_STATS_CALENDAR_LABEL,IDC_STATS_WEEKDAY_LABEL,
        IDC_STATS_HOUR_LABEL,IDC_STATS_DAILY,IDC_STATS_WEEKDAYS,
        IDC_STATS_HOURS,IDC_STATS_INTERVALS};
    for(size_t i=0;i<sizeof(general_ids)/sizeof(general_ids[0]);++i)
        ShowWindow(GetDlgItem(dialog,general_ids[i]),general?SW_SHOW:SW_HIDE);
    for(size_t i=0;i<sizeof(usage_ids)/sizeof(usage_ids[0]);++i)
        ShowWindow(GetDlgItem(dialog,usage_ids[i]),general?SW_HIDE:SW_SHOW);
}
void UpdateStatistics(HWND dialog,StatisticsContext* context,int year)
{
    if(!context) return;
    if(!biblioled::CalculateLoanStatistics(context->records,year,&context->statistics)) {
        RecordStatisticsDiagnostic(
            context, "error", "statistics_calculation",
            "CalculateLoanStatistics returned false.");
        MessageBoxW(dialog,L"Não foi possível calcular as estatísticas.",L"Estatísticas",MB_OK|MB_ICONERROR);
        return;
    }
    const biblioled::LoanStatistics& s=context->statistics;
    std::wstring text=L"Total no histórico: "+Number(static_cast<int>(s.total_records));
    SetDlgItemTextW(dialog,IDC_STATS_TOTAL,text.c_str());

    text=L"Livros: "+Number(s.book_count)+L"    Audiolivros: "+Number(s.audiobook_count);
    if(s.other_type_count>0) text+=L"    Outros: "+Number(s.other_type_count);
    SetDlgItemTextW(dialog,IDC_STATS_TYPES,text.c_str());

    text=L"Primeiro registo: "+(s.first_loan_date.empty()?L"—":DisplayDate(s.first_loan_date));
    text+=L"    Último: "+(s.last_loan_date.empty()?L"—":DisplayDate(s.last_loan_date));
    SetDlgItemTextW(dialog,IDC_STATS_RANGE,text.c_str());

    text=L"Ano "+Number(s.selected_year)+L": "+Number(s.selected_year_count);
    text+=L"    Ano anterior: "+Number(s.selected_year-1)+L" ("+Number(s.previous_year_count)+L")";
    text+=L"    Variação: "+SignedNumber(s.year_difference);
    if(s.previous_year_count>0) text+=L" ("+Decimal(static_cast<double>(s.year_difference)*100.0/s.previous_year_count)+L"%)";
    else text+=L" (sem base percentual)";
    SetDlgItemTextW(dialog,IDC_STATS_COMPARISON,text.c_str());

    text=L"Datas inválidas excluídas dos indicadores: "+Number(static_cast<int>(s.invalid_date_records));
    SetDlgItemTextW(dialog,IDC_STATS_INVALID,text.c_str());

    HWND annual=GetDlgItem(dialog,IDC_STATS_ANNUAL);
    ListView_DeleteAllItems(annual);
    for(size_t i=0;i<s.annual.size();++i) {
        const biblioled::AnnualLoanStatistics& item=s.annual[i];
        InsertRow(annual,static_cast<int>(i),Number(item.year),Number(item.loan_count),
                  Number(item.previous_year_count),SignedNumber(item.difference));
    }

    HWND daily=GetDlgItem(dialog,IDC_STATS_DAILY);
    ListView_DeleteAllItems(daily);
    for(size_t i=0;i<s.daily_activity.size();++i) {
        InsertRow(daily,static_cast<int>(i),DisplayDate(s.daily_activity[i].date),
                  Number(s.daily_activity[i].loan_count));
    }

    static const wchar_t* weekdays[]={L"Segunda-feira",L"Terça-feira",L"Quarta-feira",
        L"Quinta-feira",L"Sexta-feira",L"Sábado",L"Domingo"};
    HWND weekday_list=GetDlgItem(dialog,IDC_STATS_WEEKDAYS);
    ListView_DeleteAllItems(weekday_list);
    for(int i=0;i<7;++i) InsertRow(weekday_list,i,weekdays[i],Number(s.weekday_counts[i]));

    HWND hours=GetDlgItem(dialog,IDC_STATS_HOURS);
    ListView_DeleteAllItems(hours);
    for(int i=0;i<24;++i) {
        wchar_t label[32]={0};
        _snwprintf(label,31,L"%02d:00–%02d:59",i,i);
        InsertRow(hours,i,label,Number(s.hour_counts[i]));
    }

    if(s.interval_count==0) {
        text=L"Todo o histórico — intervalo entre dias com empréstimos: dados insuficientes.";
    } else {
        text=L"Todo o histórico — média entre dias com empréstimos: "+Decimal(s.mean_days_between);
        text+=L" dias; mediana: "+Decimal(s.median_days_between)+L" dias (";
        text+=Number(static_cast<int>(s.interval_count))+L" intervalos).";
    }
    SetDlgItemTextW(dialog,IDC_STATS_INTERVALS,text.c_str());
}
bool Reload(HWND dialog,StatisticsContext* context)
{
    if(!context || !context->store) return false;
    std::string error;
    if(!context->store->GetAll(&context->records,&error)) {
        RecordStatisticsDiagnostic(
            context, "failure", "statistics_load", error);
        std::wstring message=L"Não foi possível ler os empréstimos.\n\n";
        message+=Utf8ToWide(error);
        MessageBoxW(dialog,message.c_str(),L"Estatísticas",MB_OK|MB_ICONERROR);
        return false;
    }
    biblioled::LoanStatistics initial;
    if(!biblioled::CalculateLoanStatistics(context->records,0,&initial)) {
        RecordStatisticsDiagnostic(
            context, "error", "statistics_initial_calculation",
            "CalculateLoanStatistics returned false for the initial year.");
        return false;
    }
    PopulateYears(dialog,initial);
    UpdateStatistics(dialog,context,SelectedYear(dialog));
    return true;
}
INT_PTR CALLBACK StatisticsProc(HWND dialog,UINT message,WPARAM wParam,LPARAM lParam)
{
    StatisticsContext* context=reinterpret_cast<StatisticsContext*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(message==WM_INITDIALOG) {
        context=reinterpret_cast<StatisticsContext*>(lParam);
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(context));
        HWND tabs=GetDlgItem(dialog,IDC_STATS_TABS);
        TCITEMW item={};
        item.mask=TCIF_TEXT;
        item.pszText=const_cast<LPWSTR>(L"Geral e evolução");
        TabCtrl_InsertItem(tabs,0,&item);
        item.pszText=const_cast<LPWSTR>(L"Padrões de utilização");
        TabCtrl_InsertItem(tabs,1,&item);
        TabCtrl_SetCurSel(tabs,0);

        const wchar_t* annual_titles[]={L"Ano",L"Empréstimos",L"Ano anterior",L"Variação"};
        const int annual_widths[]={65,110,110,100};
        InitList(dialog,IDC_STATS_ANNUAL,annual_titles,annual_widths,4);
        const wchar_t* daily_titles[]={L"Data",L"Empréstimos"};
        const int daily_widths[]={104,100};
        InitList(dialog,IDC_STATS_DAILY,daily_titles,daily_widths,2);
        const wchar_t* weekday_titles[]={L"Dia",L"Total"};
        const int weekday_widths[]={104,54};
        InitList(dialog,IDC_STATS_WEEKDAYS,weekday_titles,weekday_widths,2);
        const wchar_t* hour_titles[]={L"Hora",L"Total"};
        const int hour_widths[]={88,54};
        InitList(dialog,IDC_STATS_HOURS,hour_titles,hour_widths,2);

        if(!Reload(dialog,context)) {
            EndDialog(dialog,IDCANCEL);
            return TRUE;
        }
        ShowPage(dialog);
        SetFocus(GetDlgItem(dialog,IDC_STATS_YEAR));
        return FALSE;
    }
    if(message==WM_COMMAND && context) {
        const int id=LOWORD(wParam),notification=HIWORD(wParam);
        if(id==IDC_STATS_YEAR && notification==CBN_SELCHANGE) {
            UpdateStatistics(dialog,context,SelectedYear(dialog));
            return TRUE;
        }
        if(id==IDOK || id==IDCANCEL) { EndDialog(dialog,id); return TRUE; }
    }
    if(message==WM_NOTIFY && context) {
        const NMHDR* h=reinterpret_cast<const NMHDR*>(lParam);
        if(h && h->idFrom==IDC_STATS_TABS && h->code==TCN_SELCHANGE) {
            ShowPage(dialog);
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace

namespace biblioled {
namespace win32 {
void ShowStatisticsDialog(HINSTANCE instance,HWND owner,LoanStore* store)
{
    if(!store) return;
    StatisticsContext context;
    context.store=store;
    DialogBoxParamW(instance,MAKEINTRESOURCEW(IDD_STATISTICS_DIALOG),owner,
                    StatisticsProc,reinterpret_cast<LPARAM>(&context));
}
}
}
