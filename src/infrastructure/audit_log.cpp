#include "biblioled/audit_log.h"
#include <windows.h>
#include <wchar.h>
#include <iomanip>
#include <set>
#include <sstream>
#include <vector>

namespace biblioled {
namespace {
const wchar_t* kActiveLog = L"audit.log";

void SetError(std::string* error, const char* message)
{
    if (error) *error = message;
}
std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    const wchar_t c = a[a.size()-1];
    return (c == L'\\' || c == L'/') ? a+b : a+L"\\"+b;
}
bool Exists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string Escape(const std::string& value)
{
    std::string out;
    for (size_t i=0;i<value.size();++i) {
        switch(value[i]) {
        case '\\': out += "\\\\"; break;
        case '|': out += "\\|"; break;
        case '\r': out += "\\r"; break;
        case '\n': out += "\\n"; break;
        case '"': out += "\\\""; break;
        default: out += value[i]; break;
        }
    }
    return out;
}
unsigned long long Fingerprint(const AuditEvent& e)
{
    unsigned long long h=1469598103934665603ULL;
    const unsigned long long p=1099511628211ULL;
    const std::string v[] = {
        e.occurred_at,e.action,e.details,
        e.has_before?e.before.title:std::string(),
        e.has_before?e.before.loan_date:std::string(),
        e.has_before?e.before.type:std::string(),
        e.has_after?e.after.title:std::string(),
        e.has_after?e.after.loan_date:std::string(),
        e.has_after?e.after.type:std::string()
    };
    for(size_t n=0;n<sizeof(v)/sizeof(v[0]);++n) {
        for(size_t i=0;i<v[n].size();++i) {
            h ^= static_cast<unsigned char>(v[n][i]); h *= p;
        }
        h ^= 0xff; h *= p;
    }
    const unsigned int nums[] = {
        static_cast<unsigned int>(e.id),
        static_cast<unsigned int>(e.id>>32),
        static_cast<unsigned int>(e.loan_id),
        e.has_before?1U:0U,e.has_after?1U:0U
    };
    for(size_t n=0;n<sizeof(nums)/sizeof(nums[0]);++n)
        for(int s=0;s<32;s+=8) { h ^= (nums[n]>>s)&0xff; h *= p; }
    return h;
}
std::string KeyText(const AuditEvent& e)
{
    std::ostringstream o; o<<std::hex<<std::setw(16)<<std::setfill('0')<<Fingerprint(e);
    return o.str();
}
std::string MakeLine(const AuditEvent& e,const std::string& key)
{
    std::ostringstream o;
    o<<e.occurred_at<<" | event_id="<<e.id<<" | event_key="<<key
     <<" | action="<<Escape(e.action)<<" | loan_id="<<e.loan_id;
    if(e.has_before)
        o<<" | before_title=\""<<Escape(e.before.title)<<"\""
         <<" | before_date=\""<<Escape(e.before.loan_date)<<"\""
         <<" | before_type=\""<<Escape(e.before.type)<<"\"";
    if(e.has_after)
        o<<" | after_title=\""<<Escape(e.after.title)<<"\""
         <<" | after_date=\""<<Escape(e.after.loan_date)<<"\""
         <<" | after_type=\""<<Escape(e.after.type)<<"\"";
    if(!e.details.empty()) o<<" | details=\""<<Escape(e.details)<<"\"";
    o<<"\r\n"; return o.str();
}
bool ReadLog(HANDLE file, bool active,
             std::set<std::string>* keys, std::string* error)
{
    static const char marker[] = "event_key=";
    const size_t marker_length = sizeof(marker) - 1;
    char buffer[64 * 1024];
    LONGLONG total_bytes = 0;
    LONGLONG complete_bytes = 0;
    std::string marker_window;
    std::string pending_key;
    bool key_found = false;
    bool key_started = false;
    bool key_valid = true;
    bool line_has_data = false;

    while (true) {
        DWORD received = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &received, NULL)) {
            SetError(error, "Não foi possível ler um ficheiro de auditoria.");
            return false;
        }
        if (received == 0) break;

        for (DWORD i = 0; i < received; ++i) {
            const char value = buffer[i];
            ++total_bytes;

            if (value == '\n') {
                if (key_found && key_valid && pending_key.size() == 16) {
                    keys->insert(pending_key);
                }
                marker_window.clear();
                pending_key.clear();
                key_found = false;
                key_started = false;
                key_valid = true;
                line_has_data = false;
                complete_bytes = total_bytes;
                continue;
            }

            line_has_data = true;
            if (key_started) {
                if (value == ' ' || value == '|' || value == '\r') {
                    key_started = false;
                } else {
                    const bool hexadecimal =
                        (value >= '0' && value <= '9') ||
                        (value >= 'a' && value <= 'f') ||
                        (value >= 'A' && value <= 'F');
                    if (!hexadecimal || pending_key.size() >= 16) {
                        key_valid = false;
                    } else {
                        pending_key.push_back(value);
                    }
                }
            } else if (!key_found) {
                if (marker_window.size() == marker_length) {
                    marker_window.erase(0, 1);
                }
                marker_window.push_back(value);
                if (marker_window == marker) {
                    key_found = true;
                    key_started = true;
                    marker_window.clear();
                }
            }
        }
    }

    if (line_has_data) {
        if (!active) {
            SetError(error, "Um log arquivado termina numa linha incompleta.");
            return false;
        }

        LARGE_INTEGER position;
        position.QuadPart = complete_bytes;
        if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) ||
            !SetEndOfFile(file) ||
            !FlushFileBuffers(file)) {
            SetError(error, "Não foi possível recuperar a última linha incompleta do log.");
            return false;
        }
    }
    return true;
}
bool ScanLogs(const std::wstring& dir,std::set<std::string>* keys,std::string* error)
{
    WIN32_FIND_DATAW data;
    HANDLE find=FindFirstFileW(JoinPath(dir,L"audit*.log").c_str(),&data);
    if(find==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND) return true;
        SetError(error,"Não foi possível enumerar os ficheiros de auditoria.");
        return false;
    }
    bool ok=true;
    do {
        if(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
        const bool active=_wcsicmp(data.cFileName,kActiveLog)==0;
        HANDLE f=CreateFileW(JoinPath(dir,data.cFileName).c_str(),
            active?(GENERIC_READ|GENERIC_WRITE):GENERIC_READ,
            FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(f==INVALID_HANDLE_VALUE) {
            SetError(error,"Não foi possível abrir um ficheiro de auditoria.");
            ok=false; break;
        }
        ok=ReadLog(f,active,keys,error);
        CloseHandle(f);
        if(!ok) break;
    } while(FindNextFileW(find,&data));
    DWORD last=GetLastError(); FindClose(find);
    if(ok && last!=ERROR_NO_MORE_FILES) {
        SetError(error,"A enumeração dos logs foi interrompida.");
        return false;
    }
    return ok;
}
bool FileSize(const std::wstring& path,unsigned long long* size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&d)) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND) {
            *size=0; return true;
        }
        return false;
    }
    *size=(static_cast<unsigned long long>(d.nFileSizeHigh)<<32)|d.nFileSizeLow;
    return true;
}
bool Rotate(const std::wstring& dir,unsigned long long next,unsigned long limit,std::string* error)
{
    const std::wstring active=JoinPath(dir,kActiveLog);
    unsigned long long size=0;
    if(!FileSize(active,&size)) {
        SetError(error,"Não foi possível consultar o tamanho do log.");
        return false;
    }
    if(!size || size+next<=limit) return true;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stem[64]={0};
    _snwprintf(stem,63,L"audit_%04u%02u%02u_%02u%02u%02u",
        t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    for(int n=0;n<1000;++n) {
        wchar_t name[96]={0};
        if(n) _snwprintf(name,95,L"%ls_%d.log",stem,n);
        else _snwprintf(name,95,L"%ls.log",stem);
        const std::wstring archive=JoinPath(dir,name);
        if(Exists(archive)) continue;
        if(!MoveFileExW(active.c_str(),archive.c_str(),MOVEFILE_WRITE_THROUGH)) {
            SetError(error,"Não foi possível arquivar o log actual.");
            return false;
        }
        return true;
    }
    SetError(error,"Não foi possível escolher um nome para o arquivo de log.");
    return false;
}
bool Append(const std::wstring& dir,const std::string& line,
            unsigned long limit,std::string* error)
{
    if(!Rotate(dir,line.size(),limit,error)) return false;
    HANDLE f=CreateFileW(JoinPath(dir,kActiveLog).c_str(),FILE_APPEND_DATA,
        FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) {
        SetError(error,"Não foi possível abrir o log para escrita.");
        return false;
    }
    size_t offset=0; bool ok=true;
    while(offset<line.size()) {
        DWORD requested=static_cast<DWORD>(
            (line.size()-offset)>0x7fffffffUL?0x7fffffffUL:(line.size()-offset));
        DWORD wrote=0;
        if(!WriteFile(f,line.data()+offset,requested,&wrote,NULL)||!wrote) {
            ok=false; break;
        }
        offset+=wrote;
    }
    if(ok && !FlushFileBuffers(f)) ok=false;
    CloseHandle(f);
    if(!ok) SetError(error,"Não foi possível gravar e sincronizar um evento no log.");
    return ok;
}
} // namespace

bool SynchronizeAuditLog(const std::wstring& directory,
                         const std::vector<AuditEvent>& events,
                         unsigned long maximum_file_size,
                         int* written_count,
                         std::string* error)
{
    if(written_count) *written_count=0;
    if(directory.empty()||maximum_file_size<65536) {
        SetError(error,"A pasta ou o tamanho máximo do log são inválidos.");
        return false;
    }
    std::set<std::string> known;
    if(!ScanLogs(directory,&known,error)) return false;
    int written=0;
    for(size_t i=0;i<events.size();++i) {
        const std::string key=KeyText(events[i]);
        if(known.find(key)!=known.end()) continue;
        if(!Append(directory,MakeLine(events[i],key),maximum_file_size,error)) return false;
        known.insert(key); ++written;
    }
    if(written_count) *written_count=written;
    return true;
}
} // namespace biblioled
