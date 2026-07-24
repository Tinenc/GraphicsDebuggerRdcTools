// TinecmaTool PE-import-inject PoC DLL.
//
// Purpose: prove that an extra entry surgically added to a target EXE/DLL's
// IMAGE_DIRECTORY_ENTRY_IMPORT actually causes ntdll's loader to call our
// DllMain during LdrInitializeThunk, and confirm (via the PEB.Ldr dump
// below) at what point in the loader's order we run relative to the
// anti-cheat module (ACE-Base.dll for Wuthering Waves).
//
// Build (Developer PowerShell for VS 2022):
//   cl /nologo /LD /Zi /O1 /MD /std:c++17 ^
//      /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 ^
//      TinecmaTool_PoC.cpp ^
//      /link /OUT:TinecmaTool_PoC.dll user32.lib kernel32.lib advapi32.lib
//
// The DLL exports _TnT_Entry (a no-op) so the patcher's IDT entry has at
// least one valid IMAGE_THUNK_DATA pointing somewhere in our export table;
// without that ntdll would skip the descriptor as malformed.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <string.h>

// Exported no-op symbol referenced by the injected IMAGE_THUNK_DATA. Loader
// resolves it like any other import; we never actually need it to be called.
extern "C" __declspec(dllexport) void TnT_Entry()
{
}

// ---- logging ---------------------------------------------------------------

static void LogPath(char *out, size_t outLen)
{
  char tmp[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableA("TEMP", tmp, MAX_PATH);
  if(n == 0 || n >= MAX_PATH)
    strcpy_s(tmp, MAX_PATH, "C:\\Windows\\Temp");
  // %TEMP%\TinecmaTool_PoC\<exe>_<pid>.log
  char dir[MAX_PATH] = {};
  sprintf_s(dir, MAX_PATH, "%s\\TinecmaTool_PoC", tmp);
  CreateDirectoryA(dir, NULL);
  char exePath[MAX_PATH] = {};
  GetModuleFileNameA(NULL, exePath, MAX_PATH);
  const char *exe = strrchr(exePath, '\\');
  exe = exe ? exe + 1 : exePath;
  DWORD pid = GetCurrentProcessId();
  sprintf_s(out, outLen, "%s\\%s_%lu.log", dir, exe, pid);
}

static void Log(const char *fmt, ...)
{
  char path[MAX_PATH] = {};
  LogPath(path, MAX_PATH);

  HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if(h == INVALID_HANDLE_VALUE)
    return;
  SetFilePointer(h, 0, NULL, FILE_END);

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char buf[4096] = {};
  int n = sprintf_s(buf, sizeof(buf), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu tid=%lu ",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    st.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId());

  va_list ap;
  va_start(ap, fmt);
  int n2 = vsprintf_s(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if(n2 < 0)
    n2 = 0;
  n += n2;
  if((size_t)n + 2 < sizeof(buf))
  {
    buf[n++] = '\r';
    buf[n++] = '\n';
  }
  DWORD w = 0;
  WriteFile(h, buf, (DWORD)n, &w, NULL);
  CloseHandle(h);
}

// ---- PEB.Ldr walk ----------------------------------------------------------
//
// Walking PEB.Ldr from DllMain is generally risky (loader lock is held), but
// for *read-only* iteration it's well documented to be safe. We just want a
// snapshot of every module already linked into the InLoadOrder list at the
// moment our DllMain runs -- this tells us whether ACE-Base.dll has loaded
// yet.

#ifdef _WIN64
typedef struct _PEB_LDR_DATA_T
{
  ULONG Length;
  BOOLEAN Initialized;
  PVOID SsHandle;
  LIST_ENTRY InLoadOrderModuleList;
  LIST_ENTRY InMemoryOrderModuleList;
  LIST_ENTRY InInitializationOrderModuleList;
} PEB_LDR_DATA_T;

typedef struct _LDR_DATA_TABLE_ENTRY_T
{
  LIST_ENTRY InLoadOrderLinks;
  LIST_ENTRY InMemoryOrderLinks;
  LIST_ENTRY InInitializationOrderLinks;
  PVOID DllBase;
  PVOID EntryPoint;
  ULONG SizeOfImage;
  UNICODE_STRING FullDllName;
  UNICODE_STRING BaseDllName;
  ULONG Flags;
  USHORT LoadCount;
  USHORT TlsIndex;
} LDR_DATA_TABLE_ENTRY_T;
#endif

static void DumpLoaderModules()
{
#ifdef _WIN64
  PEB *peb = (PEB *)__readgsqword(0x60);
  if(!peb || !peb->Ldr)
  {
    Log("PEB.Ldr is NULL -- loader not initialised yet (we're VERY early)");
    return;
  }
  PEB_LDR_DATA_T *ldr = (PEB_LDR_DATA_T *)peb->Ldr;
  Log("PEB.Ldr modules (in-load-order):");
  LIST_ENTRY *head = &ldr->InLoadOrderModuleList;
  LIST_ENTRY *cur = head->Flink;
  int idx = 0;
  while(cur && cur != head && idx < 256)
  {
    LDR_DATA_TABLE_ENTRY_T *e = CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY_T, InLoadOrderLinks);
    if(e->BaseDllName.Buffer && e->BaseDllName.Length)
    {
      char ascii[260] = {};
      size_t want = e->BaseDllName.Length / sizeof(wchar_t);
      if(want >= sizeof(ascii))
        want = sizeof(ascii) - 1;
      for(size_t i = 0; i < want; i++)
        ascii[i] = (char)e->BaseDllName.Buffer[i];
      Log("  [%2d] base=0x%p size=0x%08x  %s", idx, e->DllBase, e->SizeOfImage, ascii);
    }
    cur = cur->Flink;
    idx++;
  }
  Log("PEB.Ldr dump end (%d modules)", idx);
#else
  Log("DumpLoaderModules: 32-bit build -- skipping PEB.Ldr walk");
#endif
}

// ---- DllMain ---------------------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
  switch(reason)
  {
    case DLL_PROCESS_ATTACH:
    {
      DisableThreadLibraryCalls(hModule);
      char exePath[MAX_PATH] = {};
      GetModuleFileNameA(NULL, exePath, MAX_PATH);
      char myPath[MAX_PATH] = {};
      GetModuleFileNameA(hModule, myPath, MAX_PATH);
      Log("=== DLL_PROCESS_ATTACH ===");
      Log("exe: %s", exePath);
      Log("self: %s @ 0x%p", myPath, (void *)hModule);
      DumpLoaderModules();
      break;
    }
    case DLL_PROCESS_DETACH:
      Log("=== DLL_PROCESS_DETACH (reserved=%p) ===", lpReserved);
      break;
    default:
      break;
  }
  return TRUE;
}
