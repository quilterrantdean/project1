#include "biblioled/csv_export.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
namespace {
int failures=0;
void Check(bool ok,const char* message){if(!ok){++failures;std::cerr<<"FAIL: "<<message<<std::endl;}}
biblioled::LoanRecord Record(const std::string& title,const std::string& date,const std::string& type)
{
    biblioled::LoanRecord value; value.title=title; value.loan_date=date; value.type=type; return value;
}
void TestEmpty()
{
    const std::vector<biblioled::LoanRecord> records;
    Check(biblioled::BuildLoanRecordsCsv(records)==std::string("\xEF\xBB\xBF",3)+"Title;LoanDate;Type\r\n",
          "empty export should contain the UTF-8 BOM and legacy header");
}
void TestRows()
{
    std::vector<biblioled::LoanRecord> records;
    records.push_back(Record("O Principe","2026-11-14 12:30","livro"));
    records.push_back(Record("Audiobook","2030-01-02 09:15","audiolivro"));
    const std::string expected=std::string("\xEF\xBB\xBF",3)+
        "Title;LoanDate;Type\r\nO Principe;2026-11-14 12:30;livro\r\n"
        "Audiobook;2030-01-02 09:15;audiolivro\r\n";
    Check(biblioled::BuildLoanRecordsCsv(records)==expected,"records should retain order, dates and column order");
}
void TestEscaping()
{
    std::vector<biblioled::LoanRecord> records;
    records.push_back(Record("Título; \"especial\"\nsegunda linha","2026-11-14 12:30","livro"));
    records.push_back(Record("Linha\r\nquebrada","","audiolivro"));
    const std::string csv=biblioled::BuildLoanRecordsCsv(records);
    Check(csv.find("\"Título; \"\"especial\"\"\nsegunda linha\";2026-11-14 12:30;livro\r\n")!=std::string::npos,
          "special characters should be quoted and embedded quotes doubled");
    Check(csv.find("\"Linha\r\nquebrada\";;audiolivro\r\n")!=std::string::npos,
          "embedded CRLF and empty fields should be retained");
}
void TestRawDates()
{
    std::vector<biblioled::LoanRecord> records;
    records.push_back(Record("Future","2035-12-31 23:59","livro"));
    records.push_back(Record("Invalid","not-a-date","unknown"));
    const std::string csv=biblioled::BuildLoanRecordsCsv(records);
    Check(csv.find("Future;2035-12-31 23:59;livro\r\n")!=std::string::npos,"future dates should be retained");
    Check(csv.find("Invalid;not-a-date;unknown\r\n")!=std::string::npos,"invalid dates should be preserved");
}
}
int main()
{
    TestEmpty(); TestRows(); TestEscaping(); TestRawDates();
    if(failures){std::cerr<<failures<<" CSV test(s) failed."<<std::endl;return EXIT_FAILURE;}
    std::cout<<"All CSV export tests passed."<<std::endl;return EXIT_SUCCESS;
}
