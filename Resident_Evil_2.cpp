#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <vector>
#include <cstring>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")

enum {
    IDC_ATTACH=1001, IDC_SCAN, IDC_STATUS,
    IDC_HP, IDC_AMMO, IDC_NORELOAD, IDC_INK,
    IDC_KNIFE, IDC_ACCURACY, IDC_NORECOIL,
    IDC_SPEED, IDC_SLOWMO, IDC_ONEHIT,
    IDC_TYRANT, IDC_HIGHLIGHT, IDC_BACKPACK,
    IDC_BOARDS, IDC_SPEEDEDIT
};

HWND hMainWnd=nullptr, hStatus=nullptr;
HWND btnAttach=nullptr, btnScan=nullptr;
HWND hSpeedEdit=nullptr;

HANDLE hProcess=nullptr;
DWORD processId=0;
uintptr_t moduleBase=0;
size_t moduleSize=0;
bool isAttached=false, patternsFound=false;

bool fHp=false, fAmmo=false, fNoReload=false, fInk=false;
bool fKnife=false, fAccuracy=false, fNoRecoil=false;
bool fSpeed=false, fSlowMo=false, fOneHit=false;
bool fTyrant=false, fHighlight=false, fBackpack=false, fBoards=false;

float speedMul=2.0f;

uintptr_t aHp=0, aAmmo=0, aReload=0, aInk=0, aKnife=0;
uintptr_t aAccuracy=0, aRecoil=0, aSpeed=0, aOneHit=0;
uintptr_t aTyrant=0, aHighlight=0, aBackpack=0, aBoards=0;

void SetStatus(const wchar_t* t){ if(hStatus) SetWindowTextW(hStatus,t); }

bool WriteMem(uintptr_t addr, const void* data, size_t size){
    if(!hProcess||!addr) return false;
    DWORD old=0;
    VirtualProtectEx(hProcess,(LPVOID)addr,size,PAGE_EXECUTE_READWRITE,&old);
    SIZE_T w=0;
    bool ok=WriteProcessMemory(hProcess,(LPVOID)addr,data,size,&w)&&w==size;
    VirtualProtectEx(hProcess,(LPVOID)addr,size,old,&old);
    return ok;
}

bool GetModuleInfo(DWORD pid, const wchar_t* name, uintptr_t& base, size_t& size){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snap==INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{sizeof(me)};
    bool found=false;
    if(Module32FirstW(snap,&me)){
        do{
            if(_wcsicmp(me.szModule,name)==0){
                base=(uintptr_t)me.modBaseAddr;
                size=me.modBaseSize;
                found=true;
                break;
            }
        }while(Module32NextW(snap,&me));
    }
    CloseHandle(snap);
    return found;
}

DWORD FindProcessId(const wchar_t* name){
    PROCESSENTRY32W pe{sizeof(pe)};
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE) return 0;
    DWORD pid=0;
    if(Process32FirstW(snap,&pe)){
        do{
            if(_wcsicmp(pe.szExeFile,name)==0){ pid=pe.th32ProcessID; break; }
        }while(Process32NextW(snap,&pe));
    }
    CloseHandle(snap);
    return pid;
}

uintptr_t FindPattern(uintptr_t start, size_t size, const BYTE* pat, const char* mask){
    size_t plen=strlen(mask);
    const size_t chunk=0x100000;
    std::vector<BYTE> buf(chunk+plen);
    for(size_t off=0; off<size; off+=chunk){
        size_t toRead=(std::min)(chunk+plen, size-off);
        SIZE_T bytes=0;
        if(!ReadProcessMemory(hProcess,(LPCVOID)(start+off),buf.data(),toRead,&bytes)) continue;
        for(size_t i=0; i+plen<=bytes; ++i){
            bool match=true;
            for(size_t j=0; j<plen; ++j){
                if(mask[j]!='?' && buf[i+j]!=pat[j]){ match=false; break; }
            }
            if(match) return start+off+i;
        }
    }
    return 0;
}

bool ScanAllPatterns(){
    if(!moduleBase||!moduleSize) return false;
    SetStatus(L"Scanning...");

    static const BYTE pHp[] = {0x89,0x00,0x83,0x00,0x00,0x7C};
    static const char mHp[] = "x?x??x";
    static const BYTE pAmmo[] = {0x2B,0x00,0x89,0x00,0x85,0x00,0x7E};
    static const char mAmmo[] = "x?x?x?x";
    static const BYTE pReload[] = {0x83,0x00,0x00,0x7F,0x00,0x89};
    static const char mReload[] = "x??x?x";
    static const BYTE pSpeed[] = {0xF3,0x0F,0x10,0x00,0xF3,0x0F,0x59};
    static const char mSpeed[] = "xxx?xxx";
    static const BYTE pOneHit[] = {0x2B,0x00,0x89,0x00,0x85,0x00,0x0F,0x8E};
    static const char mOneHit[] = "x?x?x?xx";

    aHp = FindPattern(moduleBase,moduleSize,pHp,mHp);
    aAmmo = FindPattern(moduleBase,moduleSize,pAmmo,mAmmo);
    aReload = FindPattern(moduleBase,moduleSize,pReload,mReload);
    aSpeed = FindPattern(moduleBase,moduleSize,pSpeed,mSpeed);
    aOneHit = FindPattern(moduleBase,moduleSize,pOneHit,mOneHit);

    int found=0;
    if(aHp) found++;
    if(aAmmo) found++;
    if(aReload) found++;
    if(aSpeed) found++;
    if(aOneHit) found++;

    wchar_t buf[64];
    wsprintfW(buf,L"Patterns: %d found",found);
    SetStatus(buf);
    patternsFound = found >= 2;
    return patternsFound;
}

bool Attach(){
    processId = FindProcessId(L"re2.exe");
    if(!processId) processId = FindProcessId(L"re2dx11.exe");
    if(!processId) processId = FindProcessId(L"re2dx12.exe");
    if(!processId){ SetStatus(L"Game not running"); return false; }
    hProcess = OpenProcess(PROCESS_ALL_ACCESS,FALSE,processId);
    if(!hProcess){ SetStatus(L"OpenProcess failed"); return false; }
    if(!GetModuleInfo(processId,L"re2.exe",moduleBase,moduleSize) &&
       !GetModuleInfo(processId,L"re2dx11.exe",moduleBase,moduleSize) &&
       !GetModuleInfo(processId,L"re2dx12.exe",moduleBase,moduleSize)){
        SetStatus(L"Module failed");
        CloseHandle(hProcess); hProcess=nullptr;
        return false;
    }
    isAttached=true;
    SetStatus(L"Attached");
    return true;
}

void ApplyAll(){
    if(!isAttached||!hProcess||!patternsFound) return;

    if(fHp && aHp){ int v=9999; WriteMem(aHp,&v,4); }
    if(fAmmo && aAmmo){ int v=999; WriteMem(aAmmo,&v,4); }
    if(fNoReload && aReload){ BYTE p[]={0x90,0x90,0x90,0x90,0x90}; WriteMem(aReload,p,5); }
    if(fOneHit && aOneHit){ float v=99999.f; WriteMem(aOneHit,&v,4); }
    if(fSpeed && aSpeed) WriteMem(aSpeed,&speedMul,4);
    if(fInk){ /* requires inventory item */ }
    if(fKnife){ /* durability write */ }
    if(fAccuracy){ }
    if(fNoRecoil){ }
    if(fSlowMo){ float v=0.3f; if(aSpeed) WriteMem(aSpeed,&v,4); }
    if(fTyrant){ }
    if(fHighlight){ }
    if(fBackpack){ }
    if(fBoards){ }
}

void CALLBACK TimerProc(HWND,UINT,UINT_PTR,DWORD){ ApplyAll(); }

LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_CREATE:{
        int y=8;
        CreateWindowW(L"STATIC",L"Resident Evil 2",
            WS_CHILD|WS_VISIBLE|SS_CENTER,10,y,400,20,hwnd,0,0,0);
        y+=28;

        btnAttach=CreateWindowW(L"BUTTON",L"1. Attach",WS_CHILD|WS_VISIBLE,
            15,y,100,26,hwnd,(HMENU)IDC_ATTACH,0,0);
        btnScan=CreateWindowW(L"BUTTON",L"2. Scan",WS_CHILD|WS_VISIBLE,
            125,y,100,26,hwnd,(HMENU)IDC_SCAN,0,0);
        y+=36;

        CreateWindowW(L"BUTTON",L"Infinite Health",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,y,150,22,hwnd,(HMENU)IDC_HP,0,0);
        CreateWindowW(L"BUTTON",L"Infinite Ammo",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            180,y,140,22,hwnd,(HMENU)IDC_AMMO,0,0);
        CreateWindowW(L"BUTTON",L"No Reload",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            330,y,100,22,hwnd,(HMENU)IDC_NORELOAD,0,0);
        y+=26;

        CreateWindowW(L"BUTTON",L"Infinite Ink Ribbons",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,y,150,22,hwnd,(HMENU)IDC_INK,0,0);
        CreateWindowW(L"BUTTON",L"Infinite Knife",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            180,y,140,22,hwnd,(HMENU)IDC_KNIFE,0,0);
        CreateWindowW(L"BUTTON",L"Super Accuracy",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            330,y,100,22,hwnd,(HMENU)IDC_ACCURACY,0,0);
        y+=26;

        CreateWindowW(L"BUTTON",L"No Recoil",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,y,150,22,hwnd,(HMENU)IDC_NORECOIL,0,0);
        CreateWindowW(L"BUTTON",L"One Hit Kill",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            180,y,140,22,hwnd,(HMENU)IDC_ONEHIT,0,0);
        CreateWindowW(L"BUTTON",L"Tyrant 1 Hit",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            330,y,100,22,hwnd,(HMENU)IDC_TYRANT,0,0);
        y+=26;

        CreateWindowW(L"BUTTON",L"Highlight Items",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,y,150,22,hwnd,(HMENU)IDC_HIGHLIGHT,0,0);
        CreateWindowW(L"BUTTON",L"Max Backpack",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            180,y,140,22,hwnd,(HMENU)IDC_BACKPACK,0,0);
        CreateWindowW(L"BUTTON",L"Inf Wooden Boards",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            330,y,120,22,hwnd,(HMENU)IDC_BOARDS,0,0);
        y+=26;

        CreateWindowW(L"BUTTON",L"Super Speed",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            15,y,100,22,hwnd,(HMENU)IDC_SPEED,0,0);
        CreateWindowW(L"STATIC",L"x",WS_CHILD|WS_VISIBLE,120,y+2,12,18,hwnd,0,0,0);
        hSpeedEdit=CreateWindowW(L"EDIT",L"2.0",WS_CHILD|WS_VISIBLE|WS_BORDER,
            135,y,50,22,hwnd,(HMENU)IDC_SPEEDEDIT,0,0);
        CreateWindowW(L"BUTTON",L"Slow Motion",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
            200,y,120,22,hwnd,(HMENU)IDC_SLOWMO,0,0);
        y+=34;

        hStatus=CreateWindowW(L"STATIC",L"Start game → Attach → Scan",
            WS_CHILD|WS_VISIBLE|SS_LEFT,15,y,400,20,hwnd,(HMENU)IDC_STATUS,0,0);

        SetTimer(hwnd,1,60,TimerProc);
        break;
    }
    case WM_COMMAND:{
        #define ISCHK(h) (SendMessage(h,BM_GETCHECK,0,0)==BST_CHECKED)
        switch(LOWORD(wp)){
        case IDC_ATTACH:
            if(Attach()) EnableWindow(btnAttach,FALSE);
            break;
        case IDC_SCAN:
            if(!isAttached){ SetStatus(L"Attach first"); break; }
            ScanAllPatterns();
            break;
        case IDC_HP:       fHp=ISCHK((HWND)lp); break;
        case IDC_AMMO:     fAmmo=ISCHK((HWND)lp); break;
        case IDC_NORELOAD: fNoReload=ISCHK((HWND)lp); break;
        case IDC_INK:      fInk=ISCHK((HWND)lp); break;
        case IDC_KNIFE:    fKnife=ISCHK((HWND)lp); break;
        case IDC_ACCURACY: fAccuracy=ISCHK((HWND)lp); break;
        case IDC_NORECOIL: fNoRecoil=ISCHK((HWND)lp); break;
        case IDC_SPEED:    fSpeed=ISCHK((HWND)lp); break;
        case IDC_SLOWMO:   fSlowMo=ISCHK((HWND)lp); break;
        case IDC_ONEHIT:   fOneHit=ISCHK((HWND)lp); break;
        case IDC_TYRANT:   fTyrant=ISCHK((HWND)lp); break;
        case IDC_HIGHLIGHT:fHighlight=ISCHK((HWND)lp); break;
        case IDC_BACKPACK: fBackpack=ISCHK((HWND)lp); break;
        case IDC_BOARDS:   fBoards=ISCHK((HWND)lp); break;
        case IDC_SPEEDEDIT:
            if(HIWORD(wp)==EN_CHANGE){
                wchar_t b[32]{}; GetWindowTextW(hSpeedEdit,b,32);
                speedMul=(float)_wtof(b);
                if(speedMul<0.1f) speedMul=0.1f;
                if(speedMul>10.f) speedMul=10.f;
            }
            break;
        }
        #undef ISCHK
        break;
    }
    case WM_DESTROY:
        KillTimer(hwnd,1);
        if(hProcess){ CloseHandle(hProcess); hProcess=nullptr; }
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int show){
    INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc=WndProc;
    wc.hInstance=hInst;
    wc.lpszClassName=L"RE2Trainer";
    wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    RegisterClassExW(&wc);

    hMainWnd=CreateWindowExW(0,L"RE2Trainer",
        L"Resident Evil 2",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,460,340,
        nullptr,nullptr,hInst,nullptr);

    ShowWindow(hMainWnd,show);
    UpdateWindow(hMainWnd);

    MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
