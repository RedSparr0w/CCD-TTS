#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <sapi.h>
#include <winhttp.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <climits>
#include <cwctype>
#include <fstream>
#include <regex>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

int ClampInt(int value, int low, int high) { return value < low ? low : (value > high ? high : value); }

HMODULE g_self = nullptr;
using SetTextFn = void (__thiscall*)(void*, const void*);
using GetNameFn = const void* (__thiscall*)(void*);
using GetParentFn = void* (__thiscall*)(void*);
using StringCStrFn = const char* (__thiscall*)(const void*);
using SetPropertyFn = void (__thiscall*)(void*, const void*, const void*);
SetTextFn g_originalSetText = nullptr;
SetPropertyFn g_originalSetProperty = nullptr;
GetNameFn g_getName = nullptr;
GetParentFn g_getParent = nullptr;
StringCStrFn g_stringCStr = nullptr;

CRITICAL_SECTION g_queueLock;
HANDLE g_queueEvent = nullptr;
volatile LONG g_stopping = 0;
std::deque<std::wstring> g_queue;
std::wstring g_lastSpoken;
DWORD g_lastSpokenTick = 0;

bool g_enabled = true;
bool g_logAllText = false;
bool g_logSpoken = false;
bool g_logStartup = true;
bool g_interruptPrevious = true;
int g_volume = 100;
int g_rate = 0;
DWORD g_duplicateMs = 8000;
std::vector<std::string> g_speakTokens;
std::vector<std::string> g_ignoreTokens;
std::wstring g_engine = L"SAPI";
std::wstring g_sapiVoice;
std::wstring g_supertonicVoice = L"F1";
std::wstring g_supertonicLang = L"en";
int g_supertonicSteps = 8;
int g_supertonicSpeedPct = 105;
PROCESS_INFORMATION g_supertonicProcess{};
volatile LONG g_supertonicStarting = 0;

std::wstring g_piperVoice = L"en_US-lessac-medium";
int g_piperSpeedPct = 100;
PROCESS_INFORMATION g_piperProcess{};
volatile LONG g_piperStarting = 0;

std::wstring g_rvcModel;
int g_rvcPitch = 0;
PROCESS_INFORMATION g_rvcProcess{};
HANDLE g_childJob = nullptr;
volatile LONG g_rvcStarting = 0;
std::wstring g_rvcLoadedModel;
int g_rvcAppliedPitch = 999;
bool g_rvcDeviceConfigured = false;
std::wstring g_rvcActiveDevice;

HWND g_settingsWnd = nullptr;
HWND g_engineCombo = nullptr, g_voiceCombo = nullptr, g_voiceLabel = nullptr, g_enabledCheck = nullptr;
HWND g_baseVoiceCombo = nullptr, g_baseVoiceLabel = nullptr;
HWND g_rateEdit = nullptr, g_volumeEdit = nullptr, g_statusLabel = nullptr, g_rateLabel = nullptr;
HWND g_rateSlider = nullptr, g_volumeSlider = nullptr, g_rateValueLabel = nullptr, g_volumeValueLabel = nullptr;
HWND g_subtitleLabel = nullptr;
HWND g_installButton = nullptr, g_testButton = nullptr, g_importButton = nullptr, g_logButton = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_sectionFont = nullptr;
HBRUSH g_darkBrush = nullptr;
HBRUSH g_panelBrush = nullptr;

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s(path); size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

// Keep CCD_TTS portable: the INI stays beside the DLL and all larger runtime
// data lives in one subfolder beside it. This avoids per-user state and makes
// the whole mod movable with the game install.
std::wstring DataDirectory() {
    std::wstring dir = ModuleDirectory() + L"\\CCD_TTS";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
std::wstring IniPath() { return ModuleDirectory() + L"\\CCD_TTS.ini"; }
std::wstring LogPath() { return DataDirectory() + L"\\CCD_TTS.log"; }
std::wstring SupertonicRoot() { std::wstring p = DataDirectory() + L"\\supertonic"; CreateDirectoryW(p.c_str(), nullptr); return p; }
std::wstring VoicesDirectory() { std::wstring p = DataDirectory() + L"\\voices"; CreateDirectoryW(p.c_str(), nullptr); return p; }
std::wstring PiperRoot() { std::wstring p = DataDirectory() + L"\\piper"; CreateDirectoryW(p.c_str(), nullptr); return p; }
std::wstring PiperVoicesDirectory() { std::wstring p = PiperRoot() + L"\\voices"; CreateDirectoryW(p.c_str(), nullptr); return p; }
std::wstring RvcRoot() { std::wstring p = DataDirectory() + L"\\rvc"; CreateDirectoryW(p.c_str(), nullptr); return p; }
std::wstring RvcDeviceHintPath() { return RvcRoot() + L"\\device_hint.txt"; }
std::wstring RvcModelsDirectory() { std::wstring p = RvcRoot() + L"\\models"; CreateDirectoryW(p.c_str(), nullptr); return p; }

void ResetLog() {
    HANDLE h = CreateFileW(LogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
void LogLine(const std::string& line) {
    HANDLE h = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{}; GetLocalTime(&st); char prefix[64] = {};
    int pn = std::snprintf(prefix, sizeof(prefix), "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    DWORD w = 0; if (pn > 0) WriteFile(h, prefix, (DWORD)pn, &w, nullptr);
    WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr); WriteFile(h, "\r\n", 2, &w, nullptr); CloseHandle(h);
}
std::wstring Utf8ToWide(const char* s) {
    if (!s || !*s) return {}; int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0); UINT cp = CP_UTF8; DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) { cp = CP_ACP; flags = 0; n = MultiByteToWideChar(cp, flags, s, -1, nullptr, 0); }
    if (n <= 0) return {}; std::wstring out((size_t)n, L'\0'); MultiByteToWideChar(cp, flags, s, -1, out.data(), n); if (!out.empty() && out.back() == 0) out.pop_back(); return out;
}
std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr); if (n <= 0) return {};
    std::string out((size_t)n, '\0'); WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr); return out;
}

std::wstring ReadFirstLine(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return L"";
    std::string line;
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return Utf8ToWide(line.c_str());
}
std::string JsonEscape(const std::wstring& s) {
    std::string u = WideToUtf8(s), out; out.reserve(u.size()+16);
    for (unsigned char c : u) { if (c=='\\' || c=='\"') { out.push_back('\\'); out.push_back((char)c); } else if (c=='\n') out += "\\n"; else if (c=='\r') {} else if (c < 0x20) {} else out.push_back((char)c); }
    return out;
}
std::string LowerAscii(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); }); return s; }
std::vector<std::string> SplitTokens(const std::string& value) {
    std::vector<std::string> out; size_t start=0; while (start<=value.size()) { size_t end=value.find(';',start); if(end==std::string::npos) end=value.size(); std::string t=value.substr(start,end-start);
        while(!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin()); while(!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back(); if(!t.empty()) out.push_back(LowerAscii(t)); if(end==value.size()) break; start=end+1; } return out;
}
std::wstring ReadIniString(const wchar_t* sec, const wchar_t* key, const wchar_t* def) { wchar_t b[512] = {}; GetPrivateProfileStringW(sec,key,def,b,512,IniPath().c_str()); return b; }
void LoadConfig() {
    std::wstring ini=IniPath(); g_enabled=GetPrivateProfileIntW(L"Speech",L"Enabled",1,ini.c_str())!=0; g_volume=ClampInt(GetPrivateProfileIntW(L"Speech",L"Volume",100,ini.c_str()),0,100);
    g_rate=ClampInt(GetPrivateProfileIntW(L"Speech",L"Rate",0,ini.c_str()),-10,10); g_interruptPrevious=GetPrivateProfileIntW(L"Speech",L"InterruptPrevious",1,ini.c_str())!=0;
    g_duplicateMs=(DWORD)ClampInt(GetPrivateProfileIntW(L"Speech",L"DuplicateTimeoutMs",8000,ini.c_str()),0,86400000); g_logAllText=GetPrivateProfileIntW(L"Diagnostics",L"LogAllText",0,ini.c_str())!=0;
    g_logSpoken=GetPrivateProfileIntW(L"Diagnostics",L"LogSpoken",0,ini.c_str())!=0; g_logStartup=GetPrivateProfileIntW(L"Diagnostics",L"LogStartup",1,ini.c_str())!=0;
    g_engine=ReadIniString(L"Speech",L"Engine",L"SAPI"); g_sapiVoice=ReadIniString(L"Speech",L"SapiVoice",L""); g_supertonicVoice=ReadIniString(L"Supertonic",L"Voice",L"F1"); g_supertonicLang=ReadIniString(L"Supertonic",L"Language",L"en");
    g_supertonicSteps=ClampInt(GetPrivateProfileIntW(L"Supertonic",L"Steps",8,ini.c_str()),5,12); g_supertonicSpeedPct=ClampInt(GetPrivateProfileIntW(L"Supertonic",L"SpeedPercent",105,ini.c_str()),70,200);
    g_piperVoice=ReadIniString(L"Piper",L"Voice",L"en_US-lessac-medium"); g_piperSpeedPct=ClampInt(GetPrivateProfileIntW(L"Piper",L"SpeedPercent",100,ini.c_str()),50,200);
    g_rvcModel=ReadIniString(L"RVC",L"Model",L""); g_rvcPitch=ClampInt(GetPrivateProfileIntW(L"RVC",L"Pitch",0,ini.c_str()),-24,24);
    char b[1024]={}; GetPrivateProfileStringA("Filter","SpeakWindowContains","MainMenu/PagerText;WindowMessage/Text",b,sizeof(b),WideToUtf8(ini).c_str()); g_speakTokens=SplitTokens(b);
    char ib[1024]={}; GetPrivateProfileStringA("Filter","IgnoreWindowContains","WindowLoading;WindowSettingsEx;WindowCareer;WindowMessageHMD;PagerTextHMD",ib,sizeof(ib),WideToUtf8(ini).c_str()); g_ignoreTokens=SplitTokens(ib);
}
void SaveConfig() {
    std::wstring ini=IniPath(); WritePrivateProfileStringW(L"Speech",L"Enabled",g_enabled?L"1":L"0",ini.c_str()); WritePrivateProfileStringW(L"Speech",L"Engine",g_engine.c_str(),ini.c_str()); WritePrivateProfileStringW(L"Speech",L"SapiVoice",g_sapiVoice.c_str(),ini.c_str());
    wchar_t b[32]; _snwprintf_s(b,_TRUNCATE,L"%d",g_volume); WritePrivateProfileStringW(L"Speech",L"Volume",b,ini.c_str()); _snwprintf_s(b,_TRUNCATE,L"%d",g_rate); WritePrivateProfileStringW(L"Speech",L"Rate",b,ini.c_str());
    WritePrivateProfileStringW(L"Supertonic",L"Voice",g_supertonicVoice.c_str(),ini.c_str()); WritePrivateProfileStringW(L"Supertonic",L"Language",g_supertonicLang.c_str(),ini.c_str());
    _snwprintf_s(b,_TRUNCATE,L"%d",g_supertonicSteps); WritePrivateProfileStringW(L"Supertonic",L"Steps",b,ini.c_str()); _snwprintf_s(b,_TRUNCATE,L"%d",g_supertonicSpeedPct); WritePrivateProfileStringW(L"Supertonic",L"SpeedPercent",b,ini.c_str());
    WritePrivateProfileStringW(L"Piper",L"Voice",g_piperVoice.c_str(),ini.c_str()); _snwprintf_s(b,_TRUNCATE,L"%d",g_piperSpeedPct); WritePrivateProfileStringW(L"Piper",L"SpeedPercent",b,ini.c_str());
    WritePrivateProfileStringW(L"RVC",L"Model",g_rvcModel.c_str(),ini.c_str()); _snwprintf_s(b,_TRUNCATE,L"%d",g_rvcPitch); WritePrivateProfileStringW(L"RVC",L"Pitch",b,ini.c_str());
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,ini.c_str());
    if (g_logStartup) LogLine("Settings saved to "+WideToUtf8(ini));
}
bool ContainsAnyToken(const std::string& n0,const std::string&p0,const std::vector<std::string>&t){std::string n=LowerAscii(n0),p=LowerAscii(p0);for(auto&s:t)if(n.find(s)!=std::string::npos||p.find(s)!=std::string::npos)return true;return false;}
bool ContainsSpeakToken(const std::string& n,const std::string&p){if(ContainsAnyToken(n,p,g_ignoreTokens))return false;return ContainsAnyToken(n,p,g_speakTokens);}
std::wstring RegexReplaceSpeech(const std::wstring& input,const std::wstring& pattern,const std::wstring& replacement){
    try{
        return std::regex_replace(input,std::wregex(pattern,std::regex_constants::icase),replacement);
    }catch(...){
        return input;
    }
}

// User rules are intentionally applied once, before the built-in speech cleanup.
// Syntax: source = replacement (UTF-8; # / ; comments). Longer sources win.
std::wstring ReplacementsPath() { return DataDirectory() + L"\\replacements.txt"; }
struct SpeechRule { std::wstring from, to; };
std::vector<SpeechRule> g_speechRules;
FILETIME g_rulesWriteTime{};
DWORD g_rulesSize = 0;
bool g_rulesLoaded = false;
std::wstring TrimSpeechRule(const std::wstring& s) {
    size_t a=s.find_first_not_of(L" \t\r\n"), b=s.find_last_not_of(L" \t\r\n");
    return a==std::wstring::npos ? L"" : s.substr(a,b-a+1);
}
void RefreshSpeechRules() {
    const std::wstring path=ReplacementsPath();
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attrs)) {
        if(!g_rulesLoaded || !g_speechRules.empty()) g_speechRules.clear();
        g_rulesLoaded=false;return;
    }
    if(g_rulesLoaded && CompareFileTime(&g_rulesWriteTime,&attrs.ftLastWriteTime)==0 && g_rulesSize==attrs.nFileSizeLow)return;
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return;
    LARGE_INTEGER len{};GetFileSizeEx(f,&len);
    if(len.QuadPart>1024*1024){CloseHandle(f);return;}
    std::string bytes((size_t)len.QuadPart,'\0');DWORD got=0;
    BOOL ok=ReadFile(f,bytes.data(),(DWORD)bytes.size(),&got,nullptr);CloseHandle(f);
    if(!ok)return;
    bytes.resize(got);
    if(bytes.size()>=3 && (unsigned char)bytes[0]==0xef && (unsigned char)bytes[1]==0xbb && (unsigned char)bytes[2]==0xbf)bytes.erase(0,3);
    std::vector<SpeechRule> next;
    size_t from=0;
    while(from<bytes.size()){
        size_t end=bytes.find('\n',from);if(end==std::string::npos)end=bytes.size();
        std::string row=bytes.substr(from,end-from);from=end+1;
        std::wstring line=TrimSpeechRule(Utf8ToWide(row.c_str()));
        if(line.empty()||line[0]==L'#'||line[0]==L';')continue;
        size_t eq=line.find(L'=');if(eq==std::wstring::npos)continue;
        std::wstring lhs=TrimSpeechRule(line.substr(0,eq)),rhs=TrimSpeechRule(line.substr(eq+1));
        if(!lhs.empty())next.push_back({lhs,rhs});
    }
    std::stable_sort(next.begin(),next.end(),[](const SpeechRule&a,const SpeechRule&b){return a.from.size()>b.from.size();});
    g_speechRules.swap(next);g_rulesWriteTime=attrs.ftLastWriteTime;g_rulesSize=attrs.nFileSizeLow;g_rulesLoaded=true;
}
std::wstring ApplySpeechRules(const std::wstring& s) {
    RefreshSpeechRules();
    if(g_speechRules.empty())return s;
    std::wstring out;out.reserve(s.size());
    for(size_t i=0;i<s.size();){
        const SpeechRule* found=nullptr;
        for(const auto& rule:g_speechRules){
            if(rule.from.size()>s.size()-i)continue;
            if(_wcsnicmp(s.c_str()+i,rule.from.c_str(),rule.from.size())!=0)continue;
            const size_t j=i+rule.from.size();
            // Avoid matching Button:2 as the prefix of Button:20.
            if(i && iswalnum(s[i-1]) && iswalnum(rule.from[0]))continue;
            if(j<s.size() && iswalnum(s[j]) && iswalnum(rule.from.back()))continue;
            found=&rule;break;
        }
        if(found){out+=found->to;i+=found->from.size();}
        else out+=s[i++];
    }
    return out;
}
void OpenSpeechRules(HWND owner){
    const std::wstring path=ReplacementsPath();
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE){MessageBoxW(owner,L"Unable to create replacements.txt. Check write permissions in the game folder.",L"CCD TTS",MB_OK|MB_ICONWARNING);return;}
    LARGE_INTEGER size{};GetFileSizeEx(f,&size);
    if(size.QuadPart==0){
        const char* sample="# Custom speech replacements (UTF-8)\r\n# Format: original text = spoken text\r\n# Rules ignore case, match longer phrases first, and run before built-in cleanup.\r\n# Changes are applied automatically to the next speech message.\r\n# To remove speech, leave the right-hand side empty.\r\n# Example: Button:2 = B Button\r\n";
        DWORD written=0;WriteFile(f,sample,(DWORD)strlen(sample),&written,nullptr);
    }
    CloseHandle(f);
    ShellExecuteW(owner,L"open",L"notepad.exe",(L"\""+path+L"\"").c_str(),nullptr,SW_SHOWNORMAL);
}

std::wstring ExpandSpeechUnits(std::wstring s){
    // Expand common compact units only when they follow a number. Keeping the
    // number in the pattern avoids changing normal words such as the article
    // "a" or the letter "m" when they are not measurements.
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:km\s*/\s*h|kph|kmph)\b)",L"$1 kilometers per hour");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*mph\b)",L"$1 miles per hour");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:m\s*/\s*s|mps)\b)",L"$1 meters per second");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*km\b)",L"$1 kilometers");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*cm\b)",L"$1 centimeters");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*mm\b)",L"$1 millimeters");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*m\b)",L"$1 meters");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:mi|mile|miles)\b)",L"$1 miles");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:ft|feet)\b)",L"$1 feet");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:sec|secs|second|seconds)\.?\b)",L"$1 seconds");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:min|mins|minute|minutes)\.?\b)",L"$1 minutes");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*(?:hr|hrs|hour|hours)\.?\b)",L"$1 hours");
    s=RegexReplaceSpeech(s,LR"((\d+(?:[\.,]\d+)?)\s*%)",L"$1 percent");

    // Acronyms that most speech engines otherwise try to pronounce as words.
    s=RegexReplaceSpeech(s,LR"(\bRPM\b)",L"R P M");
    return s;
}

std::wstring CleanForSpeech(std::wstring s){
    s=ApplySpeechRules(s);
    // The CEGUI hook normally receives already-resolved game text. Keep a
    // lightweight markup guard anyway, then normalize display-oriented text
    // into something that all speech engines can read consistently.
    std::wstring stripped; bool angle=false,square=false;
    for(wchar_t c:s){
        if(!angle&&!square&&c==L'<'){angle=true;continue;}
        if(angle){if(c==L'>')angle=false;continue;}
        if(!angle&&!square&&c==L'['){square=true;continue;}
        if(square){if(c==L']')square=false;continue;}
        if(c==L'\r'||c==L'\n'||c==L'\t')c=L' ';
        stripped.push_back(c);
    }

    std::wstring out; out.reserve(stripped.size()+32);
    for(size_t i=0;i<stripped.size();++i){
        wchar_t c=stripped[i];

        // Binding labels such as Button:5 should be read as "Button 5".
        // A colon is only removed when it joins otherwise useful text; ordinary
        // sentence punctuation can still be represented by the resulting pause.
        if(c==L':'){
            bool left=i>0&&!iswspace(stripped[i-1]);
            bool right=i+1<stripped.size()&&!iswspace(stripped[i+1]);
            if(left||right){out.push_back(L' ');continue;}
        }

        // Numeric ranges: 1-2, 40 - 60, and Unicode dash variants become
        // "1 to 2" / "40 to 60". Word hyphens remain untouched.
        if(c==L'-'||c==0x2013||c==0x2014){
            size_t left=i;
            while(left>0&&iswspace(stripped[left-1]))--left;
            size_t right=i+1;
            while(right<stripped.size()&&iswspace(stripped[right]))++right;
            if(left>0&&right<stripped.size()&&iswdigit(stripped[left-1])&&iswdigit(stripped[right])){
                while(!out.empty()&&iswspace(out.back()))out.pop_back();
                out+=L" to ";
                i=right-1;
                continue;
            }
        }

        out.push_back(c);
    }

    // Expand generic measurements after range normalization, so examples such
    // as "40-60 km/h", "100 m", "20%", and "1-2 sec." all sound natural.
    out=ExpandSpeechUnits(out);

    // Collapse whitespace introduced by normalization.
    std::wstring compact; compact.reserve(out.size()); bool lastSpace=true;
    for(wchar_t c:out){
        if(iswspace(c)){
            if(!lastSpace)compact.push_back(L' ');
            lastSpace=true;
        }else{compact.push_back(c);lastSpace=false;}
    }
    while(!compact.empty()&&iswspace(compact.back()))compact.pop_back();
    return compact;
}

bool FileExists(const std::wstring& path){ return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES; }
std::string Base64Encode(const std::vector<unsigned char>& data){
    static const char* tbl="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string out; out.reserve(((data.size()+2)/3)*4);
    for(size_t i=0;i<data.size();i+=3){unsigned v=(unsigned)data[i]<<16;if(i+1<data.size())v|=(unsigned)data[i+1]<<8;if(i+2<data.size())v|=data[i+2];out.push_back(tbl[(v>>18)&63]);out.push_back(tbl[(v>>12)&63]);out.push_back(i+1<data.size()?tbl[(v>>6)&63]:'=');out.push_back(i+2<data.size()?tbl[v&63]:'=');}return out;
}
std::wstring UrlEncode(const std::wstring& in){std::string u=WideToUtf8(in);std::string o;char b[4];for(unsigned char c:u){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')o.push_back((char)c);else{std::snprintf(b,sizeof(b),"%%%02X",c);o+=b;}}return Utf8ToWide(o.c_str());}
bool HttpGetOk(const wchar_t* host,INTERNET_PORT port,const wchar_t* path,DWORD timeout=1500){HINTERNET s=WinHttpOpen(L"CCD_TTS/25",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!s)return false;WinHttpSetTimeouts(s,timeout,timeout,timeout,timeout);HINTERNET c=WinHttpConnect(s,host,port,0);HINTERNET r=c?WinHttpOpenRequest(c,L"GET",path,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;BOOL ok=r&&WinHttpSendRequest(r,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(r,nullptr);DWORD st=0,sz=sizeof(st);if(ok)WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&st,&sz,nullptr);if(r)WinHttpCloseHandle(r);if(c)WinHttpCloseHandle(c);WinHttpCloseHandle(s);return ok&&st>=200&&st<300;}
bool HttpPostBytes(const wchar_t* host,INTERNET_PORT port,const wchar_t* path,const char* body,size_t bodyLen,std::vector<unsigned char>& out,DWORD timeout=60000,DWORD* statusOut=nullptr){out.clear();if(statusOut)*statusOut=0;HINTERNET s=WinHttpOpen(L"CCD_TTS/25",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!s)return false;WinHttpSetTimeouts(s,3000,3000,timeout,timeout);HINTERNET c=WinHttpConnect(s,host,port,0);HINTERNET r=c?WinHttpOpenRequest(c,L"POST",path,nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0):nullptr;const wchar_t* hdr=L"Content-Type: application/json\r\n";BOOL ok=r&&WinHttpSendRequest(r,hdr,(DWORD)-1L,(LPVOID)body,(DWORD)bodyLen,(DWORD)bodyLen,0)&&WinHttpReceiveResponse(r,nullptr);DWORD st=0,sz=sizeof(st);if(ok)WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&st,&sz,nullptr);if(statusOut)*statusOut=st;if(ok){for(;;){DWORD a=0;if(!WinHttpQueryDataAvailable(r,&a)||!a)break;size_t old=out.size();out.resize(old+a);DWORD got=0;if(!WinHttpReadData(r,out.data()+old,a,&got)){out.clear();break;}out.resize(old+got);}}if(r)WinHttpCloseHandle(r);if(c)WinHttpCloseHandle(c);WinHttpCloseHandle(s);return ok&&st>=200&&st<300;}
bool WriteTempWav(const std::vector<unsigned char>& wav,const std::wstring& name,std::wstring& path){path=DataDirectory()+L"\\"+name;HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD w=0;BOOL ok=WriteFile(h,wav.data(),(DWORD)wav.size(),&w,nullptr);CloseHandle(h);return ok&&w==wav.size();}
static unsigned short ReadU16(const unsigned char* p){return (unsigned short)(p[0]|(p[1]<<8));}
static unsigned int ReadU32(const unsigned char* p){return (unsigned int)p[0]|((unsigned int)p[1]<<8)|((unsigned int)p[2]<<16)|((unsigned int)p[3]<<24);}
bool ApplyWavVolume(std::vector<unsigned char>& wav,int volume){
    if(volume>=100)return true;
    if(volume<=0){volume=0;}
    if(wav.size()<44||memcmp(wav.data(),"RIFF",4)!=0||memcmp(wav.data()+8,"WAVE",4)!=0)return false;
    unsigned short format=0,bits=0; size_t dataPos=0,dataSize=0;
    for(size_t pos=12;pos+8<=wav.size();){unsigned int sz=ReadU32(wav.data()+pos+4);size_t next=pos+8+(size_t)sz+(sz&1);if(next>wav.size()+1)break;if(memcmp(wav.data()+pos,"fmt ",4)==0&&sz>=16&&pos+24<=wav.size()){format=ReadU16(wav.data()+pos+8);bits=ReadU16(wav.data()+pos+22);}else if(memcmp(wav.data()+pos,"data",4)==0){dataPos=pos+8;dataSize=std::min<size_t>(sz,wav.size()-dataPos);break;}pos=next;}
    if(!dataPos||!dataSize)return false;
    const double gain=ClampInt(volume,0,100)/100.0; unsigned char* d=wav.data()+dataPos;
    if(format==1&&bits==8){for(size_t i=0;i<dataSize;++i){int v=(int)d[i]-128;v=(int)(v*gain);d[i]=(unsigned char)ClampInt(v+128,0,255);}return true;}
    if(format==1&&bits==16){for(size_t i=0;i+1<dataSize;i+=2){short v=(short)(d[i]|(d[i+1]<<8));int nv=(int)(v*gain);nv=ClampInt(nv,-32768,32767);d[i]=(unsigned char)(nv&255);d[i+1]=(unsigned char)((nv>>8)&255);}return true;}
    if(format==1&&bits==24){for(size_t i=0;i+2<dataSize;i+=3){int v=(int)d[i]|((int)d[i+1]<<8)|((int)d[i+2]<<16);if(v&0x800000)v|=~0xFFFFFF;long long nv=(long long)(v*gain);if(nv<-8388608)nv=-8388608;if(nv>8388607)nv=8388607;d[i]=(unsigned char)(nv&255);d[i+1]=(unsigned char)((nv>>8)&255);d[i+2]=(unsigned char)((nv>>16)&255);}return true;}
    if(format==1&&bits==32){for(size_t i=0;i+3<dataSize;i+=4){int v=(int)ReadU32(d+i);long long nv=(long long)(v*gain);if(nv<INT_MIN)nv=INT_MIN;if(nv>INT_MAX)nv=INT_MAX;unsigned int u=(unsigned int)(int)nv;d[i]=(unsigned char)(u&255);d[i+1]=(unsigned char)((u>>8)&255);d[i+2]=(unsigned char)((u>>16)&255);d[i+3]=(unsigned char)((u>>24)&255);}return true;}
    if(format==3&&bits==32){for(size_t i=0;i+3<dataSize;i+=4){float f;memcpy(&f,d+i,4);f=(float)(f*gain);memcpy(d+i,&f,4);}return true;}
    return false;
}
bool PlayWavBytes(const std::vector<unsigned char>& source,const std::wstring& name){std::vector<unsigned char>wav=source;if(g_volume<100)ApplyWavVolume(wav,g_volume);std::wstring path;if(!WriteTempWav(wav,name,path))return false;if(g_interruptPrevious)PlaySoundW(nullptr,nullptr,0);return PlaySoundW(path.c_str(),nullptr,SND_FILENAME|SND_ASYNC|SND_NODEFAULT)!=FALSE;}
std::string BytesPreview(const std::vector<unsigned char>& data,size_t maxLen=400){size_t n=data.size()<maxLen?data.size():maxLen;std::string s((const char*)data.data(),(const char*)data.data()+n);for(char&c:s)if((unsigned char)c<32&&c!='\r'&&c!='\n'&&c!='\t')c=' ';return s;}
bool ProcessAlive(const PROCESS_INFORMATION& pi){if(!pi.hProcess)return false;DWORD code=0;return GetExitCodeProcess(pi.hProcess,&code)&&code==STILL_ACTIVE;}

bool SupertonicInstalling(){ return FileExists(SupertonicRoot()+L"\\installing.flag"); }
bool SupertonicInstallFailed(){ return FileExists(SupertonicRoot()+L"\\install_failed.txt"); }
bool SupertonicInstalled(){ return FileExists(SupertonicRoot()+L"\\installed.ok") && FileExists(SupertonicRoot()+L"\\python\\python.exe"); }
void EnsureChildJob(){
    if(g_childJob)return;
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(!job)return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof(info))){CloseHandle(job);return;}
    g_childJob=job;
}
void TrackChildProcess(HANDLE process){
    if(!process)return;
    EnsureChildJob();
    if(g_childJob&&!AssignProcessToJobObject(g_childJob,process)){
        if(g_logStartup)LogLine("Warning: could not attach child process to cleanup job, Win32="+std::to_string(GetLastError()));
    }
}
bool SupertonicHealthy() {
    HINTERNET s=WinHttpOpen(L"CCD_TTS/12",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0); if(!s)return false;
    HINTERNET c=WinHttpConnect(s,L"127.0.0.1",7788,0); if(!c){WinHttpCloseHandle(s);return false;} HINTERNET r=WinHttpOpenRequest(c,L"GET",L"/v1/health",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0);
    BOOL ok=r&&WinHttpSendRequest(r,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(r,nullptr); DWORD status=0,sz=sizeof(status); if(ok)WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&sz,nullptr);
    if(r)WinHttpCloseHandle(r);WinHttpCloseHandle(c);WinHttpCloseHandle(s);return ok&&status==200;
}
void StartSupertonicServer(){
    if(!SupertonicInstalled()||SupertonicHealthy()||InterlockedExchange(&g_supertonicStarting,1))return;
    std::wstring py=SupertonicRoot()+L"\\python\\python.exe";
    std::wstring cli=SupertonicRoot()+L"\\python\\Scripts\\supertonic.exe";
    std::wstring exe=FileExists(cli)?cli:py;
    std::wstring cmd=FileExists(cli)?(L"\""+cli+L"\" serve --host 127.0.0.1 --port 7788"):(L"\""+py+L"\" -m supertonic serve --host 127.0.0.1 --port 7788");
    std::vector<wchar_t> buf(cmd.begin(),cmd.end());buf.push_back(0);
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};
    if(CreateProcessW(exe.c_str(),buf.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,SupertonicRoot().c_str(),&si,&pi)){g_supertonicProcess=pi; TrackChildProcess(pi.hProcess); if(g_logStartup)LogLine("Supertonic server starting (first start may download the model).");} else LogLine("ERROR: Failed to start Supertonic server.");
    InterlockedExchange(&g_supertonicStarting,0);
}
bool HttpPostWav(const std::wstring& text,std::vector<unsigned char>& out){
    HINTERNET s=WinHttpOpen(L"CCD_TTS/12",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);if(!s)return false; WinHttpSetTimeouts(s,2000,2000,30000,30000);
    HINTERNET c=WinHttpConnect(s,L"127.0.0.1",7788,0); if(!c){WinHttpCloseHandle(s);return false;} HINTERNET r=WinHttpOpenRequest(c,L"POST",L"/v1/tts",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0);if(!r){WinHttpCloseHandle(c);WinHttpCloseHandle(s);return false;}
    char json[8192]; double speed=g_supertonicSpeedPct/100.0; std::snprintf(json,sizeof(json),"{\"text\":\"%s\",\"voice\":\"%s\",\"lang\":\"%s\",\"steps\":%d,\"speed\":%.2f,\"response_format\":\"wav\"}",JsonEscape(text).c_str(),WideToUtf8(g_supertonicVoice).c_str(),WideToUtf8(g_supertonicLang).c_str(),g_supertonicSteps,speed);
    const wchar_t* hdr=L"Content-Type: application/json\r\n"; BOOL ok=WinHttpSendRequest(r,hdr,(DWORD)-1L,(LPVOID)json,(DWORD)strlen(json),(DWORD)strlen(json),0)&&WinHttpReceiveResponse(r,nullptr); DWORD status=0,ss=sizeof(status); if(ok)WinHttpQueryHeaders(r,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&ss,nullptr);
    if(ok&&status==200){for(;;){DWORD avail=0;if(!WinHttpQueryDataAvailable(r,&avail)||!avail)break;size_t old=out.size();out.resize(old+avail);DWORD got=0;if(!WinHttpReadData(r,out.data()+old,avail,&got)){out.clear();break;}out.resize(old+got);}}
    WinHttpCloseHandle(r);WinHttpCloseHandle(c);WinHttpCloseHandle(s);return ok&&status==200&&!out.empty();
}
bool SpeakSupertonic(const std::wstring& text){
    if(!SupertonicHealthy()){StartSupertonicServer(); for(int i=0;i<50&&!SupertonicHealthy();++i)Sleep(200);}
    std::vector<unsigned char>wav;if(!HttpPostWav(text,wav))return false;return PlayWavBytes(wav,L"supertonic_speech.wav");
}

bool PiperInstalling(){return FileExists(PiperRoot()+L"\\installing.flag");}
bool PiperInstallFailed(){return FileExists(PiperRoot()+L"\\install_failed.txt");}
bool PiperInstalled(){return FileExists(PiperRoot()+L"\\installed.ok")&&FileExists(PiperRoot()+L"\\python\\python.exe");}
bool PiperHealthy(){return HttpGetOk(L"127.0.0.1",5001,L"/info",1000);}
void StartPiperServer(){if(!PiperInstalled()||PiperHealthy()||InterlockedExchange(&g_piperStarting,1))return;std::wstring py=PiperRoot()+L"\\python\\python.exe";std::wstring cmd=L"\""+py+L"\" -m piper.http_server --host 127.0.0.1 --port 5001 --data-dir \""+PiperVoicesDirectory()+L"\" -m \""+g_piperVoice+L"\"";std::vector<wchar_t>b(cmd.begin(),cmd.end());b.push_back(0);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};if(CreateProcessW(py.c_str(),b.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,PiperRoot().c_str(),&si,&pi)){g_piperProcess=pi;TrackChildProcess(pi.hProcess);if(g_logStartup)LogLine("Piper server starting.");}else LogLine("ERROR: Failed to start Piper server.");InterlockedExchange(&g_piperStarting,0);}
bool SynthesizePiper(const std::wstring& text,std::vector<unsigned char>& wav){if(!PiperHealthy()){StartPiperServer();for(int i=0;i<60&&!PiperHealthy();++i)Sleep(200);}if(!PiperHealthy())return false;double length=100.0/(double)ClampInt(g_piperSpeedPct,50,200);char json[12288];std::snprintf(json,sizeof(json),"{\"text\":\"%s\",\"voice\":\"%s\",\"length_scale\":%.3f}",JsonEscape(text).c_str(),WideToUtf8(g_piperVoice).c_str(),length);return HttpPostBytes(L"127.0.0.1",5001,L"/synthesize",json,strlen(json),wav,60000)&&!wav.empty();}
bool SpeakPiper(const std::wstring& text){std::vector<unsigned char>wav;if(!SynthesizePiper(text,wav))return false;return PlayWavBytes(wav,L"piper_speech.wav");}

bool RvcInstalling(){return FileExists(RvcRoot()+L"\\installing.flag");}
bool RvcInstallFailed(){return FileExists(RvcRoot()+L"\\install_failed.txt");}
bool RvcInstalled(){return FileExists(RvcRoot()+L"\\installed.ok")&&FileExists(RvcRoot()+L"\\python\\python.exe");}
bool RvcHealthy(){return HttpGetOk(L"127.0.0.1",5055,L"/models",500);}
void StartRvcServer(){if(!RvcInstalled()||RvcHealthy()||InterlockedExchange(&g_rvcStarting,1))return;std::wstring py=RvcRoot()+L"\\python\\python.exe";std::wstring cmd=L"\""+py+L"\" -m rvc_python api -p 5055 -md \""+RvcModelsDirectory()+L"\"";std::vector<wchar_t>b(cmd.begin(),cmd.end());b.push_back(0);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};if(CreateProcessW(py.c_str(),b.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,RvcRoot().c_str(),&si,&pi)){if(g_rvcProcess.hProcess){CloseHandle(g_rvcProcess.hProcess);CloseHandle(g_rvcProcess.hThread);}g_rvcProcess=pi;TrackChildProcess(pi.hProcess);g_rvcDeviceConfigured=false;g_rvcActiveDevice.clear();g_rvcLoadedModel.clear();g_rvcAppliedPitch=999;if(g_logStartup)LogLine("RVC server starting on 127.0.0.1:5055.");}else LogLine("ERROR: Failed to start RVC server, Win32="+std::to_string(GetLastError()));InterlockedExchange(&g_rvcStarting,0);}
bool ConfigureRvcDevice(){if(g_rvcDeviceConfigured)return true;std::wstring hint=ReadFirstLine(RvcDeviceHintPath());if(hint.empty())hint=L"cpu";std::vector<unsigned char>resp;DWORD status=0;auto setDevice=[&](const wchar_t*dev)->bool{std::string json="{\"device\":\""+WideToUtf8(dev)+"\"}";resp.clear();status=0;return HttpPostBytes(L"127.0.0.1",5055,L"/set_device",json.data(),json.size(),resp,30000,&status);};if(_wcsicmp(hint.c_str(),L"cuda:0")==0){if(setDevice(L"cuda:0")){g_rvcActiveDevice=L"CUDA";g_rvcDeviceConfigured=true;LogLine("RVC acceleration: CUDA GPU enabled.");return true;}LogLine("RVC CUDA device unavailable; falling back to CPU. HTTP "+std::to_string(status)+" response: "+BytesPreview(resp));}if(!setDevice(L"cpu")){LogLine("ERROR: RVC CPU device setup failed HTTP "+std::to_string(status)+" response: "+BytesPreview(resp));return false;}g_rvcActiveDevice=L"CPU";g_rvcDeviceConfigured=true;LogLine("RVC acceleration: CPU mode.");return true;}
bool EnsureRvcModel(){if(g_rvcModel.empty()){LogLine("ERROR: Piper+RVC has no RVC model selected.");return false;}if(!RvcHealthy()){StartRvcServer();for(int i=0;i<120&&!RvcHealthy();++i)Sleep(250);}if(!RvcHealthy()){LogLine("ERROR: RVC API did not become ready on port 5055.");return false;}if(!ConfigureRvcDevice())return false;bool modelChanged=_wcsicmp(g_rvcLoadedModel.c_str(),g_rvcModel.c_str())!=0;std::vector<unsigned char>resp;DWORD status=0;if(modelChanged){std::string modelsDirJson="{\"models_dir\":\""+JsonEscape(RvcModelsDirectory())+"\"}";if(!HttpPostBytes(L"127.0.0.1",5055,L"/set_models_dir",modelsDirJson.data(),modelsDirJson.size(),resp,30000,&status)){LogLine("ERROR: RVC model directory refresh failed HTTP "+std::to_string(status)+" response: "+BytesPreview(resp));return false;}resp.clear();status=0;std::wstring path=L"/models/"+UrlEncode(g_rvcModel);if(!HttpPostBytes(L"127.0.0.1",5055,path.c_str(),"{}",2,resp,120000,&status)){LogLine("ERROR: RVC model load failed for '"+WideToUtf8(g_rvcModel)+"' HTTP "+std::to_string(status)+" response: "+BytesPreview(resp));return false;}g_rvcLoadedModel=g_rvcModel;g_rvcAppliedPitch=999;if(g_logStartup)LogLine("RVC model loaded: "+WideToUtf8(g_rvcModel));}if(g_rvcAppliedPitch!=g_rvcPitch){const char*method=_wcsicmp(g_rvcActiveDevice.c_str(),L"CUDA")==0?"rmvpe":"pm";char json[512];std::snprintf(json,sizeof(json),"{\"params\":{\"f0method\":\"%s\",\"f0up_key\":%d,\"index_rate\":0.75,\"filter_radius\":3,\"resample_sr\":0,\"rms_mix_rate\":0.25,\"protect\":0.33}}",method,g_rvcPitch);resp.clear();status=0;if(!HttpPostBytes(L"127.0.0.1",5055,L"/params",json,strlen(json),resp,30000,&status)){LogLine("ERROR: RVC parameter update failed HTTP "+std::to_string(status)+" response: "+BytesPreview(resp));return false;}g_rvcAppliedPitch=g_rvcPitch;if(g_logStartup)LogLine(std::string("RVC pitch extractor: ")+method+" ("+WideToUtf8(g_rvcActiveDevice)+")");}return true;}
bool ConvertRvc(const std::vector<unsigned char>& input,std::vector<unsigned char>& output){if(!EnsureRvcModel())return false;std::string b64=Base64Encode(input);std::string json="{\"audio_data\":\""+b64+"\"}";auto convertOnce=[&](DWORD&status)->bool{output.clear();status=0;return HttpPostBytes(L"127.0.0.1",5055,L"/convert",json.data(),json.size(),output,180000,&status)&&!output.empty();};DWORD status=0;if(convertOnce(status))return true;LogLine("ERROR: RVC conversion failed HTTP "+std::to_string(status)+" response: "+BytesPreview(output));if(_wcsicmp(g_rvcActiveDevice.c_str(),L"CUDA")==0){LogLine("RVC GPU conversion failed; retrying this utterance on CPU for compatibility.");std::vector<unsigned char>resp;DWORD ds=0;const char cpuJson[]="{\"device\":\"cpu\"}";if(HttpPostBytes(L"127.0.0.1",5055,L"/set_device",cpuJson,strlen(cpuJson),resp,30000,&ds)){g_rvcActiveDevice=L"CPU";g_rvcDeviceConfigured=true;g_rvcLoadedModel.clear();g_rvcAppliedPitch=999;if(EnsureRvcModel()&&convertOnce(status)){LogLine("RVC switched to CPU after GPU failure.");return true;}LogLine("ERROR: RVC CPU retry failed HTTP "+std::to_string(status)+" response: "+BytesPreview(output));}else LogLine("ERROR: RVC CPU fallback device setup failed HTTP "+std::to_string(ds)+" response: "+BytesPreview(resp));}output.clear();return false;}
bool SpeakPiperRvc(const std::wstring& text){std::vector<unsigned char>base,out;if(!SynthesizePiper(text,base)){LogLine("ERROR: Piper synthesis failed before RVC conversion.");return false;}if(!ConvertRvc(base,out)){LogLine("ERROR: RVC conversion failed; falling back to base Piper voice for this utterance.");return PlayWavBytes(base,L"piper_rvc_fallback.wav");}return PlayWavBytes(out,L"rvc_speech.wav");}

void QueueSpeech(const std::wstring& text){if(!g_enabled||text.empty())return;DWORD now=GetTickCount();EnterCriticalSection(&g_queueLock);if(text==g_lastSpoken&&(now-g_lastSpokenTick)<g_duplicateMs){LeaveCriticalSection(&g_queueLock);return;}g_lastSpoken=text;g_lastSpokenTick=now;if(g_interruptPrevious)g_queue.clear();g_queue.push_back(text);LeaveCriticalSection(&g_queueLock);SetEvent(g_queueEvent);}
bool EnumSapiVoiceTokens(IEnumSpObjectTokens** outEnum){
    if(!outEnum)return false; *outEnum=nullptr;
    ISpObjectTokenCategory* category=nullptr;
    HRESULT hr=CoCreateInstance(CLSID_SpObjectTokenCategory,nullptr,CLSCTX_INPROC_SERVER,IID_ISpObjectTokenCategory,(void**)&category);
    if(FAILED(hr)||!category)return false;
    hr=category->SetId(SPCAT_VOICES,FALSE);
    if(SUCCEEDED(hr))hr=category->EnumTokens(nullptr,nullptr,outEnum);
    category->Release();
    return SUCCEEDED(hr)&&*outEnum;
}
bool SapiTokenDisplayName(ISpObjectToken* token,std::wstring& out){
    out.clear(); if(!token)return false;
    WCHAR* value=nullptr;
    HRESULT hr=token->GetStringValue(nullptr,&value);
    if(SUCCEEDED(hr)&&value&&*value)out=value;
    if(value)CoTaskMemFree(value);
    return !out.empty();
}
std::vector<std::wstring> EnumerateSapiVoices(){
    std::vector<std::wstring> out; IEnumSpObjectTokens* e=nullptr;
    if(!EnumSapiVoiceTokens(&e))return out;
    ISpObjectToken* tok=nullptr; ULONG fetched=0;
    while(e->Next(1,&tok,&fetched)==S_OK){std::wstring d;if(SapiTokenDisplayName(tok,d))out.push_back(d);tok->Release();tok=nullptr;}
    e->Release(); return out;
}
bool ApplySapiVoice(ISpVoice* voice,const std::wstring& wanted){
    if(!voice||wanted.empty())return false; IEnumSpObjectTokens* e=nullptr;
    if(!EnumSapiVoiceTokens(&e))return false; bool found=false; ISpObjectToken* tok=nullptr; ULONG fetched=0;
    while(e->Next(1,&tok,&fetched)==S_OK){std::wstring d;if(SapiTokenDisplayName(tok,d)&&_wcsicmp(d.c_str(),wanted.c_str())==0){voice->SetVoice(tok);found=true;}tok->Release();tok=nullptr;if(found)break;}
    e->Release(); return found;
}
DWORD WINAPI SpeechThread(LPVOID){HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ISpVoice*voice=nullptr;if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_SpVoice,nullptr,CLSCTX_ALL,IID_ISpVoice,(void**)&voice);if(voice){voice->SetVolume((USHORT)g_volume);voice->SetRate(g_rate);ApplySapiVoice(voice,g_sapiVoice);if(g_logStartup)LogLine("SAPI fallback ready.");}
    while(InterlockedCompareExchange(&g_stopping,0,0)==0){WaitForSingleObject(g_queueEvent,250);for(;;){std::wstring t;EnterCriticalSection(&g_queueLock);if(!g_queue.empty()){t=std::move(g_queue.front());g_queue.pop_front();}LeaveCriticalSection(&g_queueLock);if(t.empty())break;bool spoken=false;if(_wcsicmp(g_engine.c_str(),L"Supertonic")==0&&SupertonicInstalled())spoken=SpeakSupertonic(t);else if(_wcsicmp(g_engine.c_str(),L"Piper")==0&&PiperInstalled())spoken=SpeakPiper(t);else if(_wcsicmp(g_engine.c_str(),L"PiperRVC")==0&&PiperInstalled()&&RvcInstalled())spoken=SpeakPiperRvc(t);if(!spoken&&voice){voice->SetVolume((USHORT)g_volume);voice->SetRate(g_rate);ApplySapiVoice(voice,g_sapiVoice);voice->Speak(t.c_str(),SPF_ASYNC|(g_interruptPrevious?SPF_PURGEBEFORESPEAK:0),nullptr);}if(g_logSpoken)LogLine("SPEAK("+WideToUtf8(g_engine)+"): "+WideToUtf8(t));}}
    if(voice)voice->Release();if(SUCCEEDED(hr))CoUninitialize();return 0;}

std::string WindowName(void*w){if(!w||!g_getName||!g_stringCStr)return{};const void*s=g_getName(w);const char*p=s?g_stringCStr(s):nullptr;return p?std::string(p):std::string();}
std::string ParentName(void*w){if(!w||!g_getParent)return{};void*p=g_getParent(w);return p?WindowName(p):std::string();}
void HandleSetText(void*w,const void*t){if(!t||!g_stringCStr)return;const char*raw=g_stringCStr(t);if(!raw||!*raw)return;std::string n=WindowName(w),p=ParentName(w);std::wstring v=CleanForSpeech(Utf8ToWide(raw));if(v.empty())return;if(g_logAllText)LogLine("TEXT window=\""+n+"\" parent=\""+p+"\" text=\""+WideToUtf8(v)+"\"");if(ContainsSpeakToken(n,p))QueueSpeech(v);}
void __fastcall HookSetText(void*self,void*,const void*t){HandleSetText(self,t);if(g_originalSetText)g_originalSetText(self,t);} void __fastcall HookSetProperty(void*self,void*,const void*n,const void*v){if(g_originalSetProperty)g_originalSetProperty(self,n,v);}
bool InstallInlineHook(void*target,void*hook,void**trampOut,size_t len,const char*label){if(!target||!hook||!trampOut||len<5)return false;auto*src=(unsigned char*)target;if(len==7&&!(src[0]==0x6A&&src[1]==0xFF&&src[2]==0x68)){LogLine(std::string("ERROR: Unexpected prologue for ")+label);return false;}auto*tr=(unsigned char*)VirtualAlloc(nullptr,len+5,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);if(!tr)return false;memcpy(tr,src,len);tr[len]=0xE9;*(int32_t*)(tr+len+1)=(int32_t)((src+len)-(tr+len+5));DWORD old=0;if(!VirtualProtect(src,len,PAGE_EXECUTE_READWRITE,&old)){VirtualFree(tr,0,MEM_RELEASE);return false;}src[0]=0xE9;*(int32_t*)(src+1)=(int32_t)((unsigned char*)hook-(src+5));for(size_t i=5;i<len;++i)src[i]=0x90;DWORD ign;VirtualProtect(src,len,old,&ign);FlushInstructionCache(GetCurrentProcess(),src,len);*trampOut=tr;if(g_logStartup)LogLine(std::string("INLINE HOOK installed: ")+label);return true;}

BOOL CALLBACK FindGameWindowProc(HWND h,LPARAM lp){DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId()&&IsWindowVisible(h)&&GetWindow(h,GW_OWNER)==nullptr){*(HWND*)lp=h;return FALSE;}return TRUE;}
HWND GameWindow(){HWND h=nullptr;EnumWindows(FindGameWindowProc,(LPARAM)&h);return h;}
void SetCtlFont(HWND h){if(h&&g_uiFont)SendMessageW(h,WM_SETFONT,(WPARAM)g_uiFont,TRUE);} 
int EngineIndexFromName(const std::wstring&e){if(_wcsicmp(e.c_str(),L"Supertonic")==0)return 1;if(_wcsicmp(e.c_str(),L"Piper")==0)return 2;if(_wcsicmp(e.c_str(),L"PiperRVC")==0)return 3;return 0;}
int SelectedEngineIndex(){return g_engineCombo?(int)SendMessageW(g_engineCombo,CB_GETCURSEL,0,0):EngineIndexFromName(g_engine);}
void SelectComboString(HWND combo,const std::wstring& wanted){int count=(int)SendMessageW(combo,CB_GETCOUNT,0,0),sel=count?0:-1;for(int i=0;i<count;++i){wchar_t b[512]={};SendMessageW(combo,CB_GETLBTEXT,i,(LPARAM)b);if(_wcsicmp(b,wanted.c_str())==0){sel=i;break;}}if(sel>=0)SendMessageW(combo,CB_SETCURSEL,sel,0);}
void AddSupertonicVoiceItems(){SendMessageW(g_voiceCombo,CB_RESETCONTENT,0,0);const wchar_t* v[]={L"M1",L"M2",L"M3",L"M4",L"M5",L"F1",L"F2",L"F3",L"F4",L"F5"};for(auto*s:v)SendMessageW(g_voiceCombo,CB_ADDSTRING,0,(LPARAM)s);WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileW((VoicesDirectory()+L"\\*.json").c_str(),&fd);if(f!=INVALID_HANDLE_VALUE){do{std::wstring n=fd.cFileName;size_t p=n.rfind(L'.');if(p!=std::wstring::npos)n.resize(p);SendMessageW(g_voiceCombo,CB_ADDSTRING,0,(LPARAM)n.c_str());}while(FindNextFileW(f,&fd));FindClose(f);}SelectComboString(g_voiceCombo,g_supertonicVoice);}
void AddSapiVoiceItems(){SendMessageW(g_voiceCombo,CB_RESETCONTENT,0,0);HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);auto voices=EnumerateSapiVoices();for(auto&v:voices)SendMessageW(g_voiceCombo,CB_ADDSTRING,0,(LPARAM)v.c_str());SelectComboString(g_voiceCombo,g_sapiVoice);if(g_sapiVoice.empty()&&!voices.empty())g_sapiVoice=voices.front();if(SUCCEEDED(hr))CoUninitialize();}
void AddPiperVoiceItemsTo(HWND combo){if(!combo)return;SendMessageW(combo,CB_RESETCONTENT,0,0);WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileW((PiperVoicesDirectory()+L"\\*.onnx").c_str(),&fd);if(f!=INVALID_HANDLE_VALUE){do{std::wstring n=fd.cFileName;size_t p=n.rfind(L'.');if(p!=std::wstring::npos)n.resize(p);SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)n.c_str());}while(FindNextFileW(f,&fd));FindClose(f);}SelectComboString(combo,g_piperVoice);}
void AddPiperVoiceItems(){AddPiperVoiceItemsTo(g_voiceCombo);}
void AddRvcModelItems(){SendMessageW(g_voiceCombo,CB_RESETCONTENT,0,0);WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileW((RvcModelsDirectory()+L"\\*").c_str(),&fd);if(f!=INVALID_HANDLE_VALUE){do{if((fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&wcscmp(fd.cFileName,L".")&&wcscmp(fd.cFileName,L".."))SendMessageW(g_voiceCombo,CB_ADDSTRING,0,(LPARAM)fd.cFileName);}while(FindNextFileW(f,&fd));FindClose(f);}SelectComboString(g_voiceCombo,g_rvcModel);}
void LayoutEngineUI(int ei){bool rvc=ei==3;if(g_baseVoiceLabel)ShowWindow(g_baseVoiceLabel,rvc?SW_SHOW:SW_HIDE);if(g_baseVoiceCombo)ShowWindow(g_baseVoiceCombo,rvc?SW_SHOW:SW_HIDE);}
void UpdateSliderValueLabels(){
    if(g_volumeSlider&&g_volumeValueLabel){int v=(int)SendMessageW(g_volumeSlider,TBM_GETPOS,0,0);wchar_t b[32];_snwprintf_s(b,_TRUNCATE,L"%d%%",v);SetWindowTextW(g_volumeValueLabel,b);}
    if(g_rateSlider&&g_rateValueLabel){int v=(int)SendMessageW(g_rateSlider,TBM_GETPOS,0,0);int ei=SelectedEngineIndex();wchar_t b[32];if(ei==1||ei==2)_snwprintf_s(b,_TRUNCATE,L"%d%%",v);else if(ei==3)_snwprintf_s(b,_TRUNCATE,L"%+d",v);else _snwprintf_s(b,_TRUNCATE,L"%+d",v);SetWindowTextW(g_rateValueLabel,b);}
}
void ConfigureRateSlider(int ei){
    if(!g_rateSlider)return;int lo=-10,hi=10,val=g_rate;
    if(ei==1){lo=70;hi=200;val=g_supertonicSpeedPct;}else if(ei==2){lo=50;hi=200;val=g_piperSpeedPct;}else if(ei==3){lo=-24;hi=24;val=g_rvcPitch;}
    SendMessageW(g_rateSlider,TBM_SETRANGEMIN,FALSE,lo);SendMessageW(g_rateSlider,TBM_SETRANGEMAX,FALSE,hi);SendMessageW(g_rateSlider,TBM_SETPOS,TRUE,val);SendMessageW(g_rateSlider,TBM_SETTICFREQ,ei==0?5:(ei==3?6:25),0);UpdateSliderValueLabels();
}
void RefreshVoiceUI(){int ei=SelectedEngineIndex();if(g_voiceLabel)SetWindowTextW(g_voiceLabel,ei==3?L"RVC model":L"Voice");if(ei==1)AddSupertonicVoiceItems();else if(ei==2)AddPiperVoiceItems();else if(ei==3){AddRvcModelItems();AddPiperVoiceItemsTo(g_baseVoiceCombo);}else AddSapiVoiceItems();LayoutEngineUI(ei);if(g_rateLabel)SetWindowTextW(g_rateLabel,ei==0?L"SAPI rate (-10..10)":(ei==1?L"Speed (70..200%)":(ei==2?L"Speed (50..200%)":L"RVC pitch (-24..24)")));if(g_installButton)EnableWindow(g_installButton,ei!=0);if(g_importButton)EnableWindow(g_importButton,ei!=0);ConfigureRateSlider(ei);}

void RefreshStatus(){if(!g_statusLabel)return;int ei=SelectedEngineIndex();std::wstring s;if(ei==0)s=L"Windows SAPI: ready";else if(ei==1){if(SupertonicInstalling())s=L"Supertonic: installing...";else if(SupertonicInstallFailed())s=L"Supertonic install failed: "+ReadFirstLine(SupertonicRoot()+L"\\install_failed.txt");else if(SupertonicInstalled())s=L"Supertonic: installed";else s=L"Supertonic: not installed";SetWindowTextW(g_installButton,SupertonicInstalled()?L"Update / Repair Supertonic":L"Download & Install Supertonic");SetWindowTextW(g_importButton,L"Import Voice JSON");}
else if(ei==2){if(PiperInstalling())s=L"Piper: installing...";else if(PiperInstallFailed())s=L"Piper install failed: "+ReadFirstLine(PiperRoot()+L"\\install_failed.txt");else if(PiperInstalled())s=L"Piper: installed";else s=L"Piper: not installed";SetWindowTextW(g_installButton,PiperInstalled()?L"Update / Repair Piper":L"Download & Install Piper");SetWindowTextW(g_importButton,L"Import Piper Voice");}
else {if(RvcInstalling())s=L"RVC: installing... (large download)";else if(RvcInstallFailed())s=L"RVC install failed: "+ReadFirstLine(RvcRoot()+L"\\install_failed.txt");else if(RvcInstalled()){std::wstring d=ReadFirstLine(RvcDeviceHintPath());s=std::wstring(L"RVC: installed; ")+(d==L"cuda:0"?L"GPU acceleration available":L"CPU mode")+L"\r\nBase Piper voice: "+g_piperVoice;}else s=L"RVC: not installed (Piper is also required)";SetWindowTextW(g_installButton,RvcInstalled()?L"Update / Repair RVC":L"Download & Install RVC");SetWindowTextW(g_importButton,L"Import RVC Model");}
SetWindowTextW(g_statusLabel,s.c_str());EnableWindow(g_installButton,ei!=0&&!(ei==1&&SupertonicInstalling())&&!(ei==2&&PiperInstalling())&&!(ei==3&&RvcInstalling()));EnableWindow(g_importButton,ei!=0);}

bool LaunchHidden(const std::wstring& exe,const std::wstring& args){std::wstring c=L"\""+exe+L"\" "+args;std::vector<wchar_t>b(c.begin(),c.end());b.push_back(0);STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi{};if(CreateProcessW(nullptr,b.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,ModuleDirectory().c_str(),&si,&pi)){TrackChildProcess(pi.hProcess);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return true;}return false;}
void RunInstaller(){int ei=SelectedEngineIndex();if(ei==0)return;const wchar_t* fn=ei==1?L"supertonic_install.ps1":(ei==2?L"piper_install.ps1":L"rvc_install.ps1");std::wstring ps=ModuleDirectory()+L"\\"+fn;if(!FileExists(ps)){SetWindowTextW(g_statusLabel,L"Installer script is missing from bin\\win32.");return;}if(ei==3&&g_rvcProcess.hProcess){TerminateProcess(g_rvcProcess.hProcess,0);WaitForSingleObject(g_rvcProcess.hProcess,2000);CloseHandle(g_rvcProcess.hProcess);CloseHandle(g_rvcProcess.hThread);ZeroMemory(&g_rvcProcess,sizeof(g_rvcProcess));g_rvcLoadedModel.clear();g_rvcAppliedPitch=999;g_rvcDeviceConfigured=false;g_rvcActiveDevice.clear();InterlockedExchange(&g_rvcStarting,0);Sleep(250);}std::wstring a=L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \""+ps+L"\"";if(LaunchHidden(L"powershell.exe",a)){SetWindowTextW(g_statusLabel,L"Installer started...");SetTimer(g_settingsWnd,1,750,nullptr);}else SetWindowTextW(g_statusLabel,L"Failed to start PowerShell installer.");}
void OpenInstallLog(){int ei=SelectedEngineIndex();std::wstring p=ei==1?SupertonicRoot()+L"\\install.log":(ei==2?PiperRoot()+L"\\install.log":RvcRoot()+L"\\install.log");ShellExecuteW(g_settingsWnd,L"open",L"notepad.exe",(L"\""+p+L"\"").c_str(),nullptr,SW_SHOWNORMAL);}
void ImportSupertonicVoice(){wchar_t file[MAX_PATH]={};OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=g_settingsWnd;ofn.lpstrFilter=L"Supertonic voice JSON (*.json)\0*.json\0All files\0*.*\0";ofn.lpstrFile=file;ofn.nMaxFile=MAX_PATH;ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(!GetOpenFileNameW(&ofn))return;const wchar_t*name=wcsrchr(file,L'\\');name=name?name+1:file;CopyFileW(file,(VoicesDirectory()+L"\\"+name).c_str(),FALSE);if(!SupertonicHealthy())StartSupertonicServer();std::wstring args=L"-sS -X POST \"http://127.0.0.1:7788/v1/styles/import?overwrite=true\" -F \"file=@"+std::wstring(file)+L"\"";LaunchHidden(L"curl.exe",args);AddSupertonicVoiceItems();}
void ImportPiperVoice(){wchar_t file[MAX_PATH]={};OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=g_settingsWnd;ofn.lpstrFilter=L"Piper ONNX voice (*.onnx)\0*.onnx\0All files\0*.*\0";ofn.lpstrFile=file;ofn.nMaxFile=MAX_PATH;ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(!GetOpenFileNameW(&ofn))return;std::wstring src=file;const wchar_t*name=wcsrchr(file,L'\\');name=name?name+1:file;std::wstring dst=PiperVoicesDirectory()+L"\\"+name;CopyFileW(src.c_str(),dst.c_str(),FALSE);std::wstring cfg=src+L".json";if(FileExists(cfg)){std::wstring dcfg=dst+L".json";CopyFileW(cfg.c_str(),dcfg.c_str(),FALSE);}AddPiperVoiceItems();SetWindowTextW(g_statusLabel,FileExists(dst+L".json")?L"Piper voice imported.":L"Voice imported, but matching .onnx.json config was not found.");}
void ImportRvcModel(){wchar_t file[MAX_PATH]={};OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=g_settingsWnd;ofn.lpstrFilter=L"RVC model (*.pth)\0*.pth\0All files\0*.*\0";ofn.lpstrFile=file;ofn.nMaxFile=MAX_PATH;ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(!GetOpenFileNameW(&ofn))return;std::wstring src=file;const wchar_t*name=wcsrchr(file,L'\\');name=name?name+1:file;std::wstring stem=name;size_t dot=stem.rfind(L'.');if(dot!=std::wstring::npos)stem.resize(dot);std::wstring dir=RvcModelsDirectory()+L"\\"+stem;CreateDirectoryW(dir.c_str(),nullptr);CopyFileW(src.c_str(),(dir+L"\\"+name).c_str(),FALSE);std::wstring srcDir=src.substr(0,src.find_last_of(L"\\/"));WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileW((srcDir+L"\\*.index").c_str(),&fd);if(f!=INVALID_HANDLE_VALUE){do{CopyFileW((srcDir+L"\\"+fd.cFileName).c_str(),(dir+L"\\"+fd.cFileName).c_str(),FALSE);}while(FindNextFileW(f,&fd));FindClose(f);}g_rvcLoadedModel.clear();g_rvcAppliedPitch=999;AddRvcModelItems();SetWindowTextW(g_statusLabel,L"RVC model imported. Any .index files in the same folder were copied too.");}
void ImportCustomVoice(){int ei=SelectedEngineIndex();if(ei==1)ImportSupertonicVoice();else if(ei==2)ImportPiperVoice();else if(ei==3)ImportRvcModel();}
void PullSettingsFromUI(){g_enabled=SendMessageW(g_enabledCheck,BM_GETCHECK,0,0)==BST_CHECKED;int ei=SelectedEngineIndex();g_engine=ei==1?L"Supertonic":(ei==2?L"Piper":(ei==3?L"PiperRVC":L"SAPI"));int vi=(int)SendMessageW(g_voiceCombo,CB_GETCURSEL,0,0);if(vi>=0){wchar_t b[512]={};SendMessageW(g_voiceCombo,CB_GETLBTEXT,vi,(LPARAM)b);if(ei==1)g_supertonicVoice=b;else if(ei==2)g_piperVoice=b;else if(ei==3){if(_wcsicmp(g_rvcModel.c_str(),b)!=0){g_rvcModel=b;g_rvcLoadedModel.clear();g_rvcAppliedPitch=999;}else g_rvcModel=b;}else g_sapiVoice=b;}if(ei==3&&g_baseVoiceCombo){int bi=(int)SendMessageW(g_baseVoiceCombo,CB_GETCURSEL,0,0);if(bi>=0){wchar_t bb[512]={};SendMessageW(g_baseVoiceCombo,CB_GETLBTEXT,bi,(LPARAM)bb);if(*bb&&_wcsicmp(g_piperVoice.c_str(),bb)!=0){g_piperVoice=bb;if(g_piperProcess.hProcess&&ProcessAlive(g_piperProcess)){TerminateProcess(g_piperProcess.hProcess,0);CloseHandle(g_piperProcess.hProcess);CloseHandle(g_piperProcess.hThread);ZeroMemory(&g_piperProcess,sizeof(g_piperProcess));}}}}int rateValue=g_rateSlider?(int)SendMessageW(g_rateSlider,TBM_GETPOS,0,0):g_rate;if(ei==1)g_supertonicSpeedPct=ClampInt(rateValue,70,200);else if(ei==2)g_piperSpeedPct=ClampInt(rateValue,50,200);else if(ei==3){int newPitch=ClampInt(rateValue,-24,24);if(newPitch!=g_rvcPitch){g_rvcPitch=newPitch;g_rvcAppliedPitch=999;}}else g_rate=ClampInt(rateValue,-10,10);g_volume=g_volumeSlider?ClampInt((int)SendMessageW(g_volumeSlider,TBM_GETPOS,0,0),0,100):g_volume;SaveConfig();if(ei==1&&SupertonicInstalled())StartSupertonicServer();if((ei==2||ei==3)&&PiperInstalled())StartPiperServer();if(ei==3&&RvcInstalled())StartRvcServer();}
LRESULT CALLBACK SettingsProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:{
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
        g_darkBrush=CreateSolidBrush(RGB(25,28,32));g_panelBrush=CreateSolidBrush(RGB(34,38,43));
        g_uiFont=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_titleFont=CreateFontW(-24,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        g_sectionFont=CreateFontW(-16,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        // Fixed-size, aligned layout. Keep all controls in the client area even with long voice names.
        constexpr int left=24, right=608, labelX=24, fieldX=178, fieldW=430;
        auto lab=[&](const wchar_t*t,int x,int y,int ww,int hh=22,HFONT f=nullptr){HWND c=CreateWindowW(L"STATIC",t,WS_CHILD|WS_VISIBLE|SS_NOPREFIX,x,y,ww,hh,h,nullptr,g_self,nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)(f?f:g_uiFont),TRUE);return c;};
        auto line=[&](int y){return CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_ETCHEDHORZ,left,y,right-left,2,h,nullptr,g_self,nullptr);};
        lab(L"Text to Speech",left,18,400,34,g_titleFont);
        g_subtitleLabel=lab(L"Spoken driving instructions and warnings  -  F10 to show or hide",left,53,570,24);
        line(85);
        g_enabledCheck=CreateWindowW(L"BUTTON",L"Enable text to speech",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,left,101,260,26,h,(HMENU)101,g_self,nullptr);
        SetCtlFont(g_enabledCheck);SendMessageW(g_enabledCheck,BM_SETCHECK,g_enabled?BST_CHECKED:BST_UNCHECKED,0);
        lab(L"VOICE ENGINE",left,144,200,23,g_sectionFont);
        lab(L"Engine",labelX,179,148);
        g_engineCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,fieldX,174,fieldW,280,h,(HMENU)102,g_self,nullptr);
        SetCtlFont(g_engineCombo);
        for(const wchar_t* e:{L"Windows SAPI",L"Supertonic",L"Piper",L"Piper + RVC"})SendMessageW(g_engineCombo,CB_ADDSTRING,0,(LPARAM)e);
        SendMessageW(g_engineCombo,CB_SETCURSEL,EngineIndexFromName(g_engine),0);
        g_voiceLabel=lab(L"Voice",labelX,217,148);
        g_voiceCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,fieldX,212,fieldW,320,h,(HMENU)103,g_self,nullptr);
        SetCtlFont(g_voiceCombo);
        g_baseVoiceLabel=lab(L"Base Piper voice",labelX,255,148);
        g_baseVoiceCombo=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|CBS_DROPDOWNLIST|WS_VSCROLL,fieldX,250,fieldW,280,h,(HMENU)112,g_self,nullptr);
        SetCtlFont(g_baseVoiceCombo);ShowWindow(g_baseVoiceLabel,SW_HIDE);ShowWindow(g_baseVoiceCombo,SW_HIDE);
        line(293);
        lab(L"PLAYBACK",left,309,200,23,g_sectionFont);
        g_rateLabel=lab(L"Rate",labelX,350,151);
        g_rateSlider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS|TBS_HORZ,fieldX,340,354,39,h,(HMENU)104,g_self,nullptr);
        g_rateValueLabel=lab(L"",548,350,60);
        lab(L"Volume",labelX,397,151);
        g_volumeSlider=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_AUTOTICKS|TBS_HORZ,fieldX,387,354,39,h,(HMENU)105,g_self,nullptr);
        SendMessageW(g_volumeSlider,TBM_SETRANGE,TRUE,MAKELONG(0,100));
        SendMessageW(g_volumeSlider,TBM_SETTICFREQ,10,0);
        SendMessageW(g_volumeSlider,TBM_SETPOS,TRUE,g_volume);
        g_volumeValueLabel=lab(L"",548,397,60);
        line(440);
        lab(L"ENGINE SETUP",left,456,200,23,g_sectionFont);
        // SS_LEFT without SS_CENTERIMAGE allows multi-line status text to wrap.
        g_statusLabel=CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX,left,486,right-left,60,h,nullptr,g_self,nullptr);
        SetCtlFont(g_statusLabel);
        g_installButton=CreateWindowW(L"BUTTON",L"Install engine",WS_CHILD|WS_VISIBLE,left,558,282,34,h,(HMENU)106,g_self,nullptr);
        SetCtlFont(g_installButton);
        g_importButton=CreateWindowW(L"BUTTON",L"Import voice/model",WS_CHILD|WS_VISIBLE,326,558,282,34,h,(HMENU)107,g_self,nullptr);
        SetCtlFont(g_importButton);
        line(606);
        g_testButton=CreateWindowW(L"BUTTON",L"Test Voice",WS_CHILD|WS_VISIBLE,left,619,105,34,h,(HMENU)108,g_self,nullptr);
        SetCtlFont(g_testButton);
        HWND rules=CreateWindowW(L"BUTTON",L"Edit Replacements",WS_CHILD|WS_VISIBLE,139,619,151,34,h,(HMENU)113,g_self,nullptr);
        SetCtlFont(rules);
        g_logButton=CreateWindowW(L"BUTTON",L"Install Log",WS_CHILD|WS_VISIBLE,300,619,100,34,h,(HMENU)111,g_self,nullptr);
        SetCtlFont(g_logButton);
        HWND save=CreateWindowW(L"BUTTON",L"Save",WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,410,619,100,34,h,(HMENU)109,g_self,nullptr);
        SetCtlFont(save);
        HWND close=CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE,520,619,88,34,h,(HMENU)110,g_self,nullptr);
        SetCtlFont(close);
        RefreshVoiceUI();ConfigureRateSlider(SelectedEngineIndex());UpdateSliderValueLabels();RefreshStatus();SetTimer(h,1,1000,nullptr);return 0;}
    case WM_CTLCOLORSTATIC:{HDC dc=(HDC)w;SetTextColor(dc,RGB(236,239,243));SetBkColor(dc,RGB(25,28,32));return(LRESULT)g_darkBrush;}
    case WM_CTLCOLORBTN:{HDC dc=(HDC)w;SetTextColor(dc,RGB(236,239,243));SetBkColor(dc,RGB(25,28,32));return(LRESULT)g_darkBrush;}
    case WM_HSCROLL:{if((HWND)l==g_volumeSlider){g_volume=ClampInt((int)SendMessageW(g_volumeSlider,TBM_GETPOS,0,0),0,100);UpdateSliderValueLabels();return 0;}if((HWND)l==g_rateSlider){UpdateSliderValueLabels();return 0;}break;}
    case WM_COMMAND:{
        if(LOWORD(w)==102&&HIWORD(w)==CBN_SELCHANGE){RefreshVoiceUI();RefreshStatus();return 0;}
        switch(LOWORD(w)){case 106:RunInstaller();break;case 107:ImportCustomVoice();break;case 108:PullSettingsFromUI();UpdateSliderValueLabels();QueueSpeech(CleanForSpeech(L"Text to speech test. Turn right in one hundred metres."));break;case 109:PullSettingsFromUI();RefreshVoiceUI();RefreshStatus();UpdateSliderValueLabels();SetWindowTextW(g_statusLabel,L"Settings saved.");break;case 110:ShowWindow(h,SW_HIDE);break;case 111:OpenInstallLog();break;case 113:OpenSpeechRules(h);break;}return 0;}
    case WM_TIMER:RefreshStatus();return 0;
    case WM_CLOSE:ShowWindow(h,SW_HIDE);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
DWORD WINAPI SettingsThread(LPVOID){WNDCLASSW wc{};wc.lpfnWndProc=SettingsProc;wc.hInstance=g_self;wc.lpszClassName=L"CCD_TTS_Settings";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=CreateSolidBrush(RGB(25,28,32));RegisterClassW(&wc);HWND owner=nullptr;for(int i=0;i<100&&!owner;++i){owner=GameWindow();Sleep(100);}g_settingsWnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,wc.lpszClassName,L"City Car Driving - Text to Speech",WS_CAPTION|WS_SYSMENU,120,90,656,716,owner,nullptr,g_self,nullptr);if(g_settingsWnd)ShowWindow(g_settingsWnd,SW_HIDE);bool down=false;while(InterlockedCompareExchange(&g_stopping,0,0)==0){bool now=(GetAsyncKeyState(VK_F10)&0x8000)!=0;if(now&&!down&&g_settingsWnd){if(IsWindowVisible(g_settingsWnd))ShowWindow(g_settingsWnd,SW_HIDE);else{RefreshVoiceUI();RefreshStatus();ShowWindow(g_settingsWnd,SW_SHOW);SetForegroundWindow(g_settingsWnd);}}down=now;MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(20);}return 0;}

DWORD WINAPI BootstrapThread(LPVOID){EnsureChildJob();InitializeCriticalSection(&g_queueLock);g_queueEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);LoadConfig();ResetLog();if(g_logStartup)LogLine("CCD_TTS 1.1.0 bootstrap started.");HANDLE speech=CreateThread(nullptr,0,SpeechThread,nullptr,0,nullptr);HANDLE settings=CreateThread(nullptr,0,SettingsThread,nullptr,0,nullptr);if(_wcsicmp(g_engine.c_str(),L"Supertonic")==0&&SupertonicInstalled())StartSupertonicServer();if((_wcsicmp(g_engine.c_str(),L"Piper")==0||_wcsicmp(g_engine.c_str(),L"PiperRVC")==0)&&PiperInstalled())StartPiperServer();if(_wcsicmp(g_engine.c_str(),L"PiperRVC")==0&&RvcInstalled())StartRvcServer();
    HMODULE cegui=nullptr;while(!cegui&&InterlockedCompareExchange(&g_stopping,0,0)==0){cegui=GetModuleHandleW(L"CEGUIBase.dll");if(!cegui)Sleep(100);}if(!cegui)return 0;g_getName=(GetNameFn)GetProcAddress(cegui,"?getName@Window@CEGUI@@QBEABVString@2@XZ");g_getParent=(GetParentFn)GetProcAddress(cegui,"?getParent@Window@CEGUI@@QBEPAV12@XZ");g_stringCStr=(StringCStrFn)GetProcAddress(cegui,"?c_str@String@CEGUI@@QBEPBDXZ");void*dst=GetProcAddress(cegui,"?setText@Window@CEGUI@@QAEXABVString@2@@Z");void*dsp=GetProcAddress(cegui,"?setProperty@PropertySet@CEGUI@@QAEXABVString@2@0@Z");if(!g_getName||!g_getParent||!g_stringCStr||!dst){LogLine("ERROR: Required CEGUI exports missing.");return 0;}void*tt=nullptr,*pt=nullptr;bool ok=InstallInlineHook(dst,(void*)&HookSetText,&tt,7,"CEGUI::Window::setText");if(ok)g_originalSetText=(SetTextFn)tt;if(dsp&&InstallInlineHook(dsp,(void*)&HookSetProperty,&pt,7,"CEGUI::PropertySet::setProperty"))g_originalSetProperty=(SetPropertyFn)pt;if(!ok){LogLine("ERROR: CEGUI text hook failed.");return 0;}if(g_logStartup)LogLine("CCD_TTS 1.1.0 ready. Press F10 for TTS settings.");while(InterlockedCompareExchange(&g_stopping,0,0)==0)Sleep(500);if(speech)CloseHandle(speech);if(settings)CloseHandle(settings);return 0;}
}
BOOL WINAPI DllMain(HINSTANCE i,DWORD r,LPVOID){if(r==DLL_PROCESS_ATTACH){g_self=i;DisableThreadLibraryCalls(i);HANDLE h=CreateThread(nullptr,0,BootstrapThread,nullptr,0,nullptr);if(h)CloseHandle(h);}else if(r==DLL_PROCESS_DETACH){InterlockedExchange(&g_stopping,1);if(g_queueEvent)SetEvent(g_queueEvent);if(g_supertonicProcess.hProcess){TerminateProcess(g_supertonicProcess.hProcess,0);CloseHandle(g_supertonicProcess.hProcess);CloseHandle(g_supertonicProcess.hThread);}if(g_piperProcess.hProcess){TerminateProcess(g_piperProcess.hProcess,0);CloseHandle(g_piperProcess.hProcess);CloseHandle(g_piperProcess.hThread);}if(g_rvcProcess.hProcess){TerminateProcess(g_rvcProcess.hProcess,0);CloseHandle(g_rvcProcess.hProcess);CloseHandle(g_rvcProcess.hThread);}if(g_childJob){CloseHandle(g_childJob);g_childJob=nullptr;}PlaySoundW(nullptr,nullptr,0);}return TRUE;}
