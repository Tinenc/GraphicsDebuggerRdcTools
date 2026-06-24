/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

// ===========================================================================
// TinecmaTool: SetThreadContext-based DLL injection (CrashSight / anti-cheat
// bypass). Instead of CreateRemoteThread(LoadLibraryW, dllPath) -- which is a
// well-known IOC that user-mode anti-cheats such as CrashSight hook -- we:
//   1. enumerate one running thread of the target process,
//   2. SuspendThread + GetThreadContext to snapshot its CPU state,
//   3. allocate a shellcode page in the remote process that:
//        - saves volatile regs / flags,
//        - calls LoadLibraryW(remotePath) (or any (void*)data export),
//        - (function-call variant) writes 1 to a done-flag byte,
//        - restores regs and `ret`s to the original RIP/EIP,
//   4. SetThreadContext to point RIP at the shellcode,
//   5. ResumeThread; the target's own thread now performs the load, so no new
//      remote thread is ever created and the call stack of LoadLibrary is the
//      victim thread, not a fresh worker.
// Set TINECMATOOL_USE_THREADHIJACK_INJECT to 0 to fall back to the legacy
// CreateRemoteThread path.
// ===========================================================================

#ifndef TINECMATOOL_USE_THREADHIJACK_INJECT
#define TINECMATOOL_USE_THREADHIJACK_INJECT 1
#endif

// ===========================================================================
// TinecmaTool: manual-mapped DLL injection (ACE-Base / unsigned-DLL bypass).
// Some kernel-level anti-cheats (e.g. ACE-Base used by Wuthering Waves) hook
// PsSetLoadImageNotifyRoutine and refuse to map our (unsigned) DLL even when
// the LoadLibraryW call originates from the victim thread itself. To work
// around that, this path never calls LoadLibrary for TinecmaTool.dll:
//   1. host reads TinecmaTool.dll from disk and parses its PE,
//   2. host VirtualAllocEx's an image-sized region in the target,
//   3. host writes section data, applies base relocations, resolves the
//      import table by walking each dependency's export directory in the
//      target's address space (no NtMapViewOfSection -> no image-load
//      notify -> not seen by ACE),
//   4. host re-protects each section with its PE characteristics,
//   5. via thread-hijack we call RtlAddFunctionTable (x64 SEH), allocate a
//      TLS slot for the current thread, run each TLS callback, then
//      DllMain(DLL_PROCESS_ATTACH).
// The module is intentionally *not* inserted into PEB.Ldr, so it remains
// invisible to GetModuleHandle / EnumProcessModules - which is exactly what
// keeps it out of anti-cheat module scans. Set
// TINECMATOOL_USE_MANUALMAP_INJECT to 0 to skip this path and use the
// thread-hijack LoadLibrary fallback instead.
// ===========================================================================

#ifndef TINECMATOOL_USE_MANUALMAP_INJECT
#define TINECMATOOL_USE_MANUALMAP_INJECT 1
#endif

namespace
{
// total ms to wait for either DLL appearance (InjectDLL) or done-flag toggle
// (InjectFunctionCall) before giving up
static const DWORD kHijackTimeoutMs = 10000;
static const DWORD kHijackPollMs = 25;

inline void sc_push8(rdcarray<uint8_t> &sc, uint8_t b)
{
  sc.push_back(b);
}
inline void sc_push32(rdcarray<uint8_t> &sc, uint32_t v)
{
  sc.push_back((uint8_t)(v & 0xFF));
  sc.push_back((uint8_t)((v >> 8) & 0xFF));
  sc.push_back((uint8_t)((v >> 16) & 0xFF));
  sc.push_back((uint8_t)((v >> 24) & 0xFF));
}
inline void sc_push64(rdcarray<uint8_t> &sc, uint64_t v)
{
  sc_push32(sc, (uint32_t)v);
  sc_push32(sc, (uint32_t)(v >> 32));
}

// Build shellcode for a "call (paramReg)=arg, then ret to origRip" trampoline.
// If doneFlagAddr != 0, the shellcode also stores 1 to that byte just before
// returning (used by InjectFunctionCall_ThreadHijack to signal completion).
#if defined(_M_X64) || defined(__x86_64__)
static rdcarray<uint8_t> BuildHijackShellcode(uintptr_t arg, uintptr_t funcAddr,
                                           uintptr_t doneFlagAddr, uintptr_t origRip)
{
  rdcarray<uint8_t> sc;
  sc.reserve(96);

  sc_push8(sc, 0x9C);    // pushfq
  sc_push8(sc, 0x50);    // push rax
  sc_push8(sc, 0x51);    // push rcx
  sc_push8(sc, 0x52);    // push rdx
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x50);    // push r8
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x51);    // push r9
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x52);    // push r10
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x53);    // push r11

  // sub rsp, 0x28      ; shadow space (32 bytes) + keep 16-byte alignment after the 7x push above
  sc_push8(sc, 0x48);
  sc_push8(sc, 0x83);
  sc_push8(sc, 0xEC);
  sc_push8(sc, 0x28);

  // mov rcx, <arg>
  sc_push8(sc, 0x48);
  sc_push8(sc, 0xB9);
  sc_push64(sc, (uint64_t)arg);

  // mov rax, <funcAddr>
  sc_push8(sc, 0x48);
  sc_push8(sc, 0xB8);
  sc_push64(sc, (uint64_t)funcAddr);

  // call rax
  sc_push8(sc, 0xFF);
  sc_push8(sc, 0xD0);

  // add rsp, 0x28
  sc_push8(sc, 0x48);
  sc_push8(sc, 0x83);
  sc_push8(sc, 0xC4);
  sc_push8(sc, 0x28);

  if(doneFlagAddr)
  {
    // mov rax, doneFlagAddr ; mov byte [rax], 1
    sc_push8(sc, 0x48);
    sc_push8(sc, 0xB8);
    sc_push64(sc, (uint64_t)doneFlagAddr);
    sc_push8(sc, 0xC6);
    sc_push8(sc, 0x00);
    sc_push8(sc, 0x01);
  }

  sc_push8(sc, 0x41);
  sc_push8(sc, 0x5B);    // pop r11
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x5A);    // pop r10
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x59);    // pop r9
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x58);    // pop r8
  sc_push8(sc, 0x5A);    // pop rdx
  sc_push8(sc, 0x59);    // pop rcx
  sc_push8(sc, 0x58);    // pop rax
  sc_push8(sc, 0x9D);    // popfq

  // push originalRip (low32, sign-extends to 64-bit zero high)
  sc_push8(sc, 0x68);
  sc_push32(sc, (uint32_t)origRip);
  // mov dword [rsp+4], originalRip high32 -- fix the high 32 bits
  sc_push8(sc, 0xC7);
  sc_push8(sc, 0x44);
  sc_push8(sc, 0x24);
  sc_push8(sc, 0x04);
  sc_push32(sc, (uint32_t)(origRip >> 32));
  // ret
  sc_push8(sc, 0xC3);

  return sc;
}
#else
static rdcarray<uint8_t> BuildHijackShellcode(uintptr_t arg, uintptr_t funcAddr,
                                           uintptr_t doneFlagAddr, uintptr_t origEip)
{
  rdcarray<uint8_t> sc;
  sc.reserve(48);

  sc_push8(sc, 0x9C);    // pushfd
  sc_push8(sc, 0x60);    // pushad
  // push <arg>
  sc_push8(sc, 0x68);
  sc_push32(sc, (uint32_t)arg);
  // mov eax, funcAddr
  sc_push8(sc, 0xB8);
  sc_push32(sc, (uint32_t)funcAddr);
  // call eax  ; stdcall, callee cleans up the single push above
  sc_push8(sc, 0xFF);
  sc_push8(sc, 0xD0);

  if(doneFlagAddr)
  {
    // mov byte [doneFlagAddr], 1  ; uses absolute address (no SIB)
    sc_push8(sc, 0xC6);
    sc_push8(sc, 0x05);
    sc_push32(sc, (uint32_t)doneFlagAddr);
    sc_push8(sc, 0x01);
  }

  sc_push8(sc, 0x61);    // popad
  sc_push8(sc, 0x9D);    // popfd
  // push originalEip
  sc_push8(sc, 0x68);
  sc_push32(sc, (uint32_t)origEip);
  // ret
  sc_push8(sc, 0xC3);

  return sc;
}
#endif

// Open one suitable thread of `pid`. Prefer a thread that's already suspended
// (e.g. main thread of a CREATE_SUSPENDED process); otherwise the first
// enumerable one. Caller owns the returned handle.
static HANDLE OpenInjectionThread(DWORD pid)
{
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if(snap == INVALID_HANDLE_VALUE)
    return NULL;

  HANDLE picked = NULL;

  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  if(Thread32First(snap, &te))
  {
    do
    {
      if(te.th32OwnerProcessID != pid)
        continue;

      HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                            FALSE, te.th32ThreadID);
      if(!h)
        continue;

      if(!picked)
      {
        picked = h;
      }
      else
      {
        CloseHandle(h);
      }
    } while(Thread32Next(snap, &te));
  }

  CloseHandle(snap);
  return picked;
}

// Read a single byte from remote process memory. Returns false on read error.
static bool RemoteReadByte(HANDLE hProcess, void *addr, uint8_t &out)
{
  SIZE_T n = 0;
  return ReadProcessMemory(hProcess, addr, &out, 1, &n) && n == 1;
}

// Core thread-hijack routine. funcAddr is invoked with a single pointer arg
// (matches the ABI of both LoadLibraryW and INTERNAL_* exports in this fork).
// If `dataOut`/`dataLen` is provided the data is read back from `argRemote`
// after completion.
static bool ThreadHijackInvoke(HANDLE hProcess, DWORD pid, uintptr_t funcAddr,
                               void *argRemote, void *dataOut, size_t dataLen,
                               const char *debugTag)
{
  if(!pid)
    pid = GetProcessId(hProcess);
  if(!pid)
  {
    RDCERR("ThreadHijackInvoke(%s): no PID for process handle", debugTag);
    return false;
  }

  HANDLE hThread = OpenInjectionThread(pid);
  if(!hThread)
  {
    RDCERR("ThreadHijackInvoke(%s): no openable thread in PID %u (err %u)", debugTag, pid,
           GetLastError());
    return false;
  }

  DWORD prevSusp = SuspendThread(hThread);
  if(prevSusp == (DWORD)-1)
  {
    RDCERR("ThreadHijackInvoke(%s): SuspendThread failed (err %u)", debugTag, GetLastError());
    CloseHandle(hThread);
    return false;
  }

  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_FULL;
  if(!GetThreadContext(hThread, &ctx))
  {
    RDCERR("ThreadHijackInvoke(%s): GetThreadContext failed (err %u)", debugTag, GetLastError());
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }

  uintptr_t origRip = 0;
#if defined(_M_X64) || defined(__x86_64__)
  origRip = (uintptr_t)ctx.Rip;
#else
  origRip = (uintptr_t)ctx.Eip;
#endif

  // Allocate a done-flag byte iff caller wants completion notification.
  void *doneFlag = NULL;
  if(dataOut)
  {
    doneFlag = VirtualAllocEx(hProcess, NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if(!doneFlag)
    {
      RDCERR("ThreadHijackInvoke(%s): VirtualAllocEx(doneFlag) failed (err %u)", debugTag,
             GetLastError());
      ResumeThread(hThread);
      CloseHandle(hThread);
      return false;
    }
    uint8_t zero = 0;
    SIZE_T n = 0;
    WriteProcessMemory(hProcess, doneFlag, &zero, 1, &n);
  }

  rdcarray<uint8_t> shellcode =
      BuildHijackShellcode((uintptr_t)argRemote, funcAddr, (uintptr_t)doneFlag, origRip);

  void *remoteCode = VirtualAllocEx(hProcess, NULL, shellcode.size(), MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
  if(!remoteCode)
  {
    RDCERR("ThreadHijackInvoke(%s): VirtualAllocEx(shellcode) failed (err %u)", debugTag,
           GetLastError());
    if(doneFlag)
      VirtualFreeEx(hProcess, doneFlag, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }

  SIZE_T written = 0;
  if(!WriteProcessMemory(hProcess, remoteCode, shellcode.data(), shellcode.size(), &written) ||
     written != shellcode.size())
  {
    RDCERR("ThreadHijackInvoke(%s): WriteProcessMemory(shellcode) failed (err %u)", debugTag,
           GetLastError());
    VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
    if(doneFlag)
      VirtualFreeEx(hProcess, doneFlag, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }

#if defined(_M_X64) || defined(__x86_64__)
  ctx.Rip = (DWORD64)(uintptr_t)remoteCode;
#else
  ctx.Eip = (DWORD)(uintptr_t)remoteCode;
#endif

  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("ThreadHijackInvoke(%s): SetThreadContext failed (err %u)", debugTag, GetLastError());
    VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
    if(doneFlag)
      VirtualFreeEx(hProcess, doneFlag, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }

  // Resume to whatever suspend count we found + 1 (we added one with our SuspendThread).
  for(DWORD i = 0; i <= prevSusp; i++)
    ResumeThread(hThread);

  bool ok = true;

  if(doneFlag)
  {
    // Poll the done-flag byte (1-byte stores are atomic on x86/x64).
    ok = false;
    for(DWORD waited = 0; waited < kHijackTimeoutMs; waited += kHijackPollMs)
    {
      uint8_t b = 0;
      if(RemoteReadByte(hProcess, doneFlag, b) && b)
      {
        ok = true;
        break;
      }
      Sleep(kHijackPollMs);
    }

    if(!ok)
      RDCERR("ThreadHijackInvoke(%s): timed out waiting for done-flag", debugTag);

    if(ok && dataOut && dataLen)
    {
      SIZE_T n = 0;
      ReadProcessMemory(hProcess, argRemote, dataOut, dataLen, &n);
    }
  }
  else
  {
    // No done-flag: at least give the shellcode a few ms to execute before
    // freeing the remote code page. Callers (e.g. InjectDLL) verify success
    // out-of-band via FindRemoteDLL.
    Sleep(50);
  }

  VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
  if(doneFlag)
    VirtualFreeEx(hProcess, doneFlag, 0, MEM_RELEASE);
  CloseHandle(hThread);
  return ok;
}

static bool InjectDLL_ThreadHijack(HANDLE hProcess, DWORD pid, rdcwstr libName)
{
  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if(!kernel32)
  {
    RDCERR("InjectDLL_ThreadHijack: couldn't get kernel32 handle");
    return false;
  }
  uintptr_t loadLib = (uintptr_t)GetProcAddress(kernel32, "LoadLibraryW");
  if(!loadLib)
  {
    RDCERR("InjectDLL_ThreadHijack: couldn't resolve LoadLibraryW");
    return false;
  }

  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  void *remoteDllPath = VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
  if(!remoteDllPath)
  {
    RDCERR("InjectDLL_ThreadHijack: VirtualAllocEx for dllPath failed (err %u)", GetLastError());
    return false;
  }

  SIZE_T n = 0;
  if(!WriteProcessMemory(hProcess, remoteDllPath, dllPath, sizeof(dllPath), &n))
  {
    RDCERR("InjectDLL_ThreadHijack: WriteProcessMemory for dllPath failed (err %u)", GetLastError());
    VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
    return false;
  }

  // No done-flag: InjectDLL's success is verified externally via FindRemoteDLL.
  bool ok = ThreadHijackInvoke(hProcess, pid, loadLib, remoteDllPath, NULL, 0, "LoadLibraryW");

  VirtualFreeEx(hProcess, remoteDllPath, 0, MEM_RELEASE);
  return ok;
}

static bool InjectFunctionCall_ThreadHijack(HANDLE hProcess, DWORD pid, uintptr_t funcAddr,
                                            void *data, size_t dataLen, const char *debugTag)
{
  if(dataLen == 0)
  {
    RDCERR("InjectFunctionCall_ThreadHijack(%s): invalid empty payload", debugTag);
    return false;
  }

  void *remoteData =
      VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(!remoteData)
  {
    RDCERR("InjectFunctionCall_ThreadHijack(%s): VirtualAllocEx failed (err %u)", debugTag,
           GetLastError());
    return false;
  }

  SIZE_T n = 0;
  if(!WriteProcessMemory(hProcess, remoteData, data, dataLen, &n))
  {
    RDCERR("InjectFunctionCall_ThreadHijack(%s): WriteProcessMemory failed (err %u)", debugTag,
           GetLastError());
    VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
    return false;
  }

  bool ok = ThreadHijackInvoke(hProcess, pid, funcAddr, remoteData, data, dataLen, debugTag);

  VirtualFreeEx(hProcess, remoteData, 0, MEM_RELEASE);
  return ok;
}

// ===========================================================================
// Manual-map helpers (x64 only). 32-bit targets fall back to thread-hijack.
// ===========================================================================

#if defined(_M_X64) || defined(__x86_64__)

// Read an arbitrary blob from the target process. Returns true iff all bytes
// were transferred.
static bool MM_ReadRemote(HANDLE hProcess, const void *addr, void *out, size_t len)
{
  SIZE_T n = 0;
  return ReadProcessMemory(hProcess, addr, out, len, &n) && n == len;
}

// Read a NUL-terminated ASCII string from the target. Caller-bounded; returns
// false if the bound is hit without seeing a NUL.
static bool MM_ReadRemoteCString(HANDLE hProcess, const void *addr, char *out, size_t maxLen)
{
  for(size_t i = 0; i < maxLen; i++)
  {
    SIZE_T n = 0;
    char c = 0;
    if(!ReadProcessMemory(hProcess, (const uint8_t *)addr + i, &c, 1, &n) || n != 1)
      return false;
    out[i] = c;
    if(c == 0)
      return true;
  }
  return false;
}

// Slurp a local file into a byte vector. Caps at 512 MiB so a corrupt path can't
// blow up RAM.
static bool MM_ReadFileBytes(const wchar_t *path, rdcarray<uint8_t> &out)
{
  HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
  if(hFile == INVALID_HANDLE_VALUE)
    return false;
  LARGE_INTEGER size = {};
  if(!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 ||
     size.QuadPart > (LONGLONG)(512ull * 1024ull * 1024ull))
  {
    CloseHandle(hFile);
    return false;
  }
  out.resize((size_t)size.QuadPart);
  DWORD read = 0;
  BOOL ok = ReadFile(hFile, out.data(), (DWORD)size.QuadPart, &read, NULL);
  CloseHandle(hFile);
  return ok && read == (DWORD)size.QuadPart;
}

// Find a loaded module in the target by case-insensitive basename match
// (e.g. "kernel32.dll").
static uintptr_t MM_FindRemoteModuleBase(DWORD pid, const char *basenameLower)
{
  HANDLE snap = INVALID_HANDLE_VALUE;
  for(int retry = 0; retry < 10; retry++)
  {
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if(snap != INVALID_HANDLE_VALUE)
      break;
    if(GetLastError() != ERROR_BAD_LENGTH)
      break;
  }
  if(snap == INVALID_HANDLE_VALUE)
    return 0;

  uintptr_t ret = 0;
  MODULEENTRY32W me = {};
  me.dwSize = sizeof(me);
  if(Module32FirstW(snap, &me))
  {
    do
    {
      char ascii[MAX_MODULE_NAME32 + 1] = {};
      for(int i = 0; me.szModule[i] != 0 && i < MAX_MODULE_NAME32; i++)
        ascii[i] = (char)towlower(me.szModule[i]);
      if(!strcmp(ascii, basenameLower))
      {
        ret = (uintptr_t)me.modBaseAddr;
        break;
      }
    } while(Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  return ret;
}

// Build a shellcode trampoline that calls funcAddr with up to 4 register args
// (Win64 ABI: RCX, RDX, R8, R9), optionally captures RAX into `resultSlot`,
// optionally writes a done-flag, then returns to `origRip` -- same shape as
// BuildHijackShellcode but for arbitrary multi-arg calls.
static rdcarray<uint8_t> BuildCallShellcode(uintptr_t funcAddr, const rdcarray<uintptr_t> &args,
                                            uintptr_t resultSlot, uintptr_t doneFlagAddr,
                                            uintptr_t origRip)
{
  rdcarray<uint8_t> sc;
  sc.reserve(192);

  sc_push8(sc, 0x9C);    // pushfq
  sc_push8(sc, 0x50);    // push rax
  sc_push8(sc, 0x51);    // push rcx
  sc_push8(sc, 0x52);    // push rdx
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x50);    // push r8
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x51);    // push r9
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x52);    // push r10
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x53);    // push r11

  // sub rsp, 0x20  -- shadow space (32 bytes). We pushed 8 8-byte values
  // (pushfq + 7 GPRs) above which is already 16-byte aligned, so a 32-byte
  // adjustment keeps RSP 16-byte aligned before the upcoming `call`.
  sc_push8(sc, 0x48);
  sc_push8(sc, 0x83);
  sc_push8(sc, 0xEC);
  sc_push8(sc, 0x20);

  if(args.size() > 0)
  {
    // mov rcx, imm64
    sc_push8(sc, 0x48);
    sc_push8(sc, 0xB9);
    sc_push64(sc, (uint64_t)args[0]);
  }
  if(args.size() > 1)
  {
    // mov rdx, imm64
    sc_push8(sc, 0x48);
    sc_push8(sc, 0xBA);
    sc_push64(sc, (uint64_t)args[1]);
  }
  if(args.size() > 2)
  {
    // mov r8, imm64
    sc_push8(sc, 0x49);
    sc_push8(sc, 0xB8);
    sc_push64(sc, (uint64_t)args[2]);
  }
  if(args.size() > 3)
  {
    // mov r9, imm64
    sc_push8(sc, 0x49);
    sc_push8(sc, 0xB9);
    sc_push64(sc, (uint64_t)args[3]);
  }

  // mov rax, funcAddr ; call rax
  sc_push8(sc, 0x48);
  sc_push8(sc, 0xB8);
  sc_push64(sc, (uint64_t)funcAddr);
  sc_push8(sc, 0xFF);
  sc_push8(sc, 0xD0);

  // add rsp, 0x20  -- matches the sub rsp, 0x20 above.
  sc_push8(sc, 0x48);
  sc_push8(sc, 0x83);
  sc_push8(sc, 0xC4);
  sc_push8(sc, 0x20);

  if(resultSlot)
  {
    // mov rcx, resultSlot ; mov [rcx], rax
    sc_push8(sc, 0x48);
    sc_push8(sc, 0xB9);
    sc_push64(sc, (uint64_t)resultSlot);
    sc_push8(sc, 0x48);
    sc_push8(sc, 0x89);
    sc_push8(sc, 0x01);
  }

  if(doneFlagAddr)
  {
    // mov rax, doneFlagAddr ; mov byte [rax], 1
    sc_push8(sc, 0x48);
    sc_push8(sc, 0xB8);
    sc_push64(sc, (uint64_t)doneFlagAddr);
    sc_push8(sc, 0xC6);
    sc_push8(sc, 0x00);
    sc_push8(sc, 0x01);
  }

  sc_push8(sc, 0x41);
  sc_push8(sc, 0x5B);    // pop r11
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x5A);    // pop r10
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x59);    // pop r9
  sc_push8(sc, 0x41);
  sc_push8(sc, 0x58);    // pop r8
  sc_push8(sc, 0x5A);    // pop rdx
  sc_push8(sc, 0x59);    // pop rcx
  sc_push8(sc, 0x58);    // pop rax
  sc_push8(sc, 0x9D);    // popfq

  // push origRip ; mov [rsp+4], origRip>>32 ; ret
  sc_push8(sc, 0x68);
  sc_push32(sc, (uint32_t)origRip);
  sc_push8(sc, 0xC7);
  sc_push8(sc, 0x44);
  sc_push8(sc, 0x24);
  sc_push8(sc, 0x04);
  sc_push32(sc, (uint32_t)(origRip >> 32));
  sc_push8(sc, 0xC3);

  return sc;
}

// Generalised thread-hijack call: up to 4 args, optional 8-byte RAX capture.
// resultOut, if non-NULL, receives the 64-bit return value (RAX after the
// call). Internally allocates a 16-byte slot in the target, polls a done flag
// like ThreadHijackInvoke does.
static bool ThreadHijackCall(HANDLE hProcess, DWORD pid, uintptr_t funcAddr,
                             const rdcarray<uintptr_t> &args, uint64_t *resultOut,
                             const char *debugTag)
{
  if(!pid)
    pid = GetProcessId(hProcess);
  if(!pid)
  {
    RDCERR("ThreadHijackCall(%s): no PID for process handle", debugTag);
    return false;
  }
  HANDLE hThread = OpenInjectionThread(pid);
  if(!hThread)
  {
    RDCERR("ThreadHijackCall(%s): no openable thread in PID %u (err %u)", debugTag, pid,
           GetLastError());
    return false;
  }
  DWORD prevSusp = SuspendThread(hThread);
  if(prevSusp == (DWORD)-1)
  {
    RDCERR("ThreadHijackCall(%s): SuspendThread failed (err %u)", debugTag, GetLastError());
    CloseHandle(hThread);
    return false;
  }
  CONTEXT ctx = {};
  ctx.ContextFlags = CONTEXT_FULL;
  if(!GetThreadContext(hThread, &ctx))
  {
    RDCERR("ThreadHijackCall(%s): GetThreadContext failed (err %u)", debugTag, GetLastError());
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }
  uintptr_t origRip = (uintptr_t)ctx.Rip;

  // 16-byte slot: byte 0..7 = result (RAX), byte 8 = done flag.
  void *slot = VirtualAllocEx(hProcess, NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(!slot)
  {
    RDCERR("ThreadHijackCall(%s): VirtualAllocEx(slot) failed (err %u)", debugTag, GetLastError());
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }
  uint8_t zero[16] = {};
  SIZE_T n = 0;
  WriteProcessMemory(hProcess, slot, zero, sizeof(zero), &n);

  uintptr_t resultSlot = resultOut ? (uintptr_t)slot : 0;
  uintptr_t doneFlag = (uintptr_t)slot + 8;

  rdcarray<uint8_t> sc =
      BuildCallShellcode(funcAddr, args, resultSlot, doneFlag, origRip);

  void *remoteCode = VirtualAllocEx(hProcess, NULL, sc.size(), MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
  if(!remoteCode)
  {
    RDCERR("ThreadHijackCall(%s): VirtualAllocEx(code) failed (err %u)", debugTag, GetLastError());
    VirtualFreeEx(hProcess, slot, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }
  if(!WriteProcessMemory(hProcess, remoteCode, sc.data(), sc.size(), &n) || n != sc.size())
  {
    RDCERR("ThreadHijackCall(%s): WriteProcessMemory(code) failed (err %u)", debugTag,
           GetLastError());
    VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, slot, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }

  ctx.Rip = (DWORD64)(uintptr_t)remoteCode;
  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("ThreadHijackCall(%s): SetThreadContext failed (err %u)", debugTag, GetLastError());
    VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
    VirtualFreeEx(hProcess, slot, 0, MEM_RELEASE);
    ResumeThread(hThread);
    CloseHandle(hThread);
    return false;
  }
  for(DWORD i = 0; i <= prevSusp; i++)
    ResumeThread(hThread);

  bool ok = false;
  for(DWORD waited = 0; waited < kHijackTimeoutMs; waited += kHijackPollMs)
  {
    uint8_t b = 0;
    if(RemoteReadByte(hProcess, (uint8_t *)slot + 8, b) && b)
    {
      ok = true;
      break;
    }
    Sleep(kHijackPollMs);
  }
  if(!ok)
  {
    RDCERR("ThreadHijackCall(%s): timed out waiting for done-flag", debugTag);
  }
  if(ok && resultOut)
  {
    uint64_t rax = 0;
    if(MM_ReadRemote(hProcess, slot, &rax, sizeof(rax)))
      *resultOut = rax;
    else
      ok = false;
  }

  VirtualFreeEx(hProcess, remoteCode, 0, MEM_RELEASE);
  VirtualFreeEx(hProcess, slot, 0, MEM_RELEASE);
  CloseHandle(hThread);
  return ok;
}

// Resolve an import by leaning on the *host* process loader. Works for API
// Set names (api-ms-win-*) because LoadLibraryExA in our own process resolves
// them through the host's ApiSet schema -- since host and target share the
// same OS, the resolution is consistent. We then translate the host-side
// function address into the target's identical module via basename + RVA.
//
// This path is essential because the target may be in CREATE_SUSPENDED state
// where calling LoadLibrary inside it would deadlock (LdrInitializeThunk has
// not yet run loader init). It also avoids leaving any IOCs (no remote
// LoadLibrary call shows up to anti-cheat scanners during stage A).
//
// Returns 0 if the host can't load the dll, the function is missing locally,
// or the resolved canonical module isn't present in the target.
// Returns true if `dllName` is an API Set pseudo-DLL (api-ms-win-*,
// ext-ms-win-*). API Sets are resolved by the loader's ApiSet schema and
// only become visible inside a process *after* LdrInitializeThunk runs;
// we must never try to LoadLibrary them inside a CREATE_SUSPENDED target.
static bool MM_IsApiSetName(const char *dllName)
{
  if(!dllName)
    return false;
  size_t len = strlen(dllName);
  if(len < 7)
    return false;
  return _strnicmp(dllName, "api-ms-", 7) == 0 || _strnicmp(dllName, "ext-ms-", 7) == 0;
}

// Build a lowercase basename copy for case-insensitive Toolhelp32 lookup.
static rdcstr MM_BasenameLower(const char *dllName)
{
  rdcstr out;
  for(const char *p = dllName; *p; p++)
    out.push_back((char)tolower((unsigned char)*p));
  return out;
}

// Ensure that a *real* (non-API-Set) dependency DLL is loaded inside the
// target. CREATE_SUSPENDED targets only have the bare minimum loaded by the
// kernel (ntdll, kernel32, kernelbase). Anything else our DLL imports from
// (ws2_32, setupapi, version, ...) is not present, so MM_FindRemoteModuleBase
// returns 0 and host-side RVA translation has no base to anchor against.
//
// At CREATE_SUSPENDED stage the loader is initialised enough that calling
// LoadLibraryA on a *real* PE is safe; what was unsafe (and deadlocked
// previously on SleepConditionVariableSRW) was LoadLibraryA on an API Set
// schema name. That's why we filter those out and rely on host-side
// resolution for them.
//
// Returns the resolved remote base, or 0 for API Sets and on failure.
static uintptr_t MM_EnsureRemoteModule(HANDLE hProcess, DWORD pid, const char *dllName)
{
  if(MM_IsApiSetName(dllName))
    return 0;

  rdcstr lower = MM_BasenameLower(dllName);
  uintptr_t base = MM_FindRemoteModuleBase(pid, lower.c_str());
  if(base)
    return base;

  HMODULE hKernel = GetModuleHandleA("kernel32.dll");
  if(!hKernel)
    return 0;
  FARPROC pLoadLibraryA = GetProcAddress(hKernel, "LoadLibraryA");
  if(!pLoadLibraryA)
    return 0;

  size_t sz = strlen(dllName) + 1;
  void *remoteName =
      VirtualAllocEx(hProcess, NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(!remoteName)
  {
    RDCWARN("ManualMap: VirtualAllocEx(name='%s') failed (err %u)", dllName, GetLastError());
    return 0;
  }
  SIZE_T w = 0;
  if(!WriteProcessMemory(hProcess, remoteName, dllName, sz, &w) || w != sz)
  {
    VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);
    return 0;
  }

  uint64_t hRet = 0;
  rdcarray<uintptr_t> args;
  args.push_back((uintptr_t)remoteName);
  bool ok = ThreadHijackCall(hProcess, pid, (uintptr_t)pLoadLibraryA, args, &hRet,
                             "MM-EnsureLoadLibraryA");
  VirtualFreeEx(hProcess, remoteName, 0, MEM_RELEASE);

  if(!ok)
  {
    RDCWARN("ManualMap: target LoadLibraryA('%s') hijack call failed", dllName);
    return 0;
  }
  if(hRet == 0)
  {
    RDCWARN("ManualMap: target LoadLibraryA('%s') returned NULL", dllName);
    return 0;
  }

  // Toolhelp32 needs a moment to see the freshly loaded module; the inner
  // retry loop in MM_FindRemoteModuleBase already handles transient
  // ERROR_BAD_LENGTH. As a belt-and-braces measure: if the snapshot doesn't
  // see it yet, accept the HMODULE returned by LoadLibraryA itself (it's
  // the module base in the target's address space).
  base = MM_FindRemoteModuleBase(pid, lower.c_str());
  if(!base)
  {
    RDCDEBUG("ManualMap: '%s' loaded at 0x%llx but Toolhelp32 missed it; using HMODULE directly",
             dllName, (unsigned long long)hRet);
    base = (uintptr_t)hRet;
  }
  return base;
}

// Resolve an import via the host's loader. The host process has the same
// ApiSet schema and the same system DLLs as the target (same OS), so the
// RVA we compute on the host side is identical to what the target sees
// once the dependency DLL is loaded. `knownTargetBase`, if non-zero, lets
// the caller skip a repeated Toolhelp32 lookup; otherwise we either look
// it up or load the dep on demand via MM_EnsureRemoteModule.
static uintptr_t MM_ResolveViaHost(HANDLE hProcess, DWORD pid, const char *dllName,
                                   const char *funcName, WORD ordinal,
                                   uintptr_t knownTargetBase)
{
  HMODULE localM = LoadLibraryExA(dllName, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!localM)
    localM = LoadLibraryA(dllName);
  if(!localM)
    return 0;

  FARPROC fp = funcName ? GetProcAddress(localM, funcName)
                        : GetProcAddress(localM, MAKEINTRESOURCEA(ordinal));
  if(!fp)
    return 0;

  // GetModuleFileNameW gives us the *real* DLL on the host side once an API
  // Set has been resolved (e.g. "api-ms-win-core-synch-l1-2-0.dll" ->
  // "kernelbase.dll"). The target sees the same real DLL.
  wchar_t modPath[MAX_PATH] = {};
  if(!GetModuleFileNameW(localM, modPath, MAX_PATH))
    return 0;
  const wchar_t *bn = wcsrchr(modPath, L'\\');
  bn = bn ? bn + 1 : modPath;

  char basenameLower[MAX_PATH] = {};
  for(int i = 0; bn[i] != 0 && i < MAX_PATH - 1; i++)
    basenameLower[i] = (char)towlower((wchar_t)bn[i]);

  uintptr_t targetBase = knownTargetBase;
  if(!targetBase)
    targetBase = MM_FindRemoteModuleBase(pid, basenameLower);

  // If the resolved real DLL is not yet loaded in the target, load it. We
  // only do this when the *original* import name was a real DLL (not an
  // API Set) -- if the user asked us to resolve "api-ms-...", the target's
  // loader has not initialised the API Set schema yet and any
  // LoadLibrary("api-ms-...") inside the target would deadlock. The real
  // DLL behind the API Set will be pulled in transitively by another import
  // (kernel32 -> kernelbase) so we can leave it for that path.
  if(!targetBase && !MM_IsApiSetName(dllName))
    targetBase = MM_EnsureRemoteModule(hProcess, pid, basenameLower);

  if(!targetBase)
    return 0;

  uintptr_t rva = (uintptr_t)fp - (uintptr_t)localM;
  return targetBase + rva;
}

// Resolve a single import (by name or by ordinal) inside a remote module. Walks
// the target's IMAGE_EXPORT_DIRECTORY directly via ReadProcessMemory, so we
// don't have to load the dependency DLL into our own host address space and
// risk version skew.
static uintptr_t MM_RemoteResolveExport(HANDLE hProcess, DWORD pid, uintptr_t moduleBase,
                                        const char *funcName, WORD ordinal, int depth);

static uintptr_t MM_ResolveForwarder(HANDLE hProcess, DWORD pid, const char *forwarder, int depth)
{
  // forwarder format: "DllBaseName.FuncName" or "DllBaseName.#ordinal".
  // DllBaseName may itself be an API Set (api-ms-win-* / ext-ms-win-*) which
  // does *not* exist as a real PE in the target. We always try host-side
  // resolution first, which handles API Sets transparently.
  if(depth > 6)
    return 0;
  const char *dot = strchr(forwarder, '.');
  if(!dot)
    return 0;

  char dllPart[128] = {};
  size_t dllLen = (size_t)(dot - forwarder);
  if(dllLen >= sizeof(dllPart) - 5)
    return 0;
  memcpy(dllPart, forwarder, dllLen);
  // Append ".dll" if the basename doesn't already carry an extension (forwarders
  // like "NTDLL.RtlXxx" omit it).
  bool hasExt = false;
  for(size_t i = 0; i < dllLen; i++)
    if(dllPart[i] == '.')
      hasExt = true;
  if(!hasExt)
    strcat_s(dllPart, ".dll");

  const char *funcPart = dot + 1;
  bool isOrdinal = (funcPart[0] == '#');
  WORD ordinal = isOrdinal ? (WORD)atoi(funcPart + 1) : (WORD)0;

  // 1) Host-side resolution (works for API Sets and all system DLLs). This
  // will also LoadLibraryA the real underlying DLL inside the target if
  // missing, so the RVA-anchored address resolves.
  uintptr_t addr = MM_ResolveViaHost(hProcess, pid, dllPart, isOrdinal ? NULL : funcPart,
                                     ordinal, 0);
  if(addr)
    return addr;

  // 2) Fall back to target-side lookup + recursive export walk. We do *not*
  // try to LoadLibrary the DLL in the target here -- MM_ResolveViaHost
  // already attempted that (for non-API-Set names) so if we got here, either
  // the DLL is API-Set-only or the target rejected LoadLibrary.
  rdcstr lower = strlower(rdcstr(dllPart));
  uintptr_t mod = MM_FindRemoteModuleBase(pid, lower.c_str());
  if(!mod)
    return 0;
  return MM_RemoteResolveExport(hProcess, pid, mod, isOrdinal ? NULL : funcPart, ordinal,
                                depth + 1);
}

static uintptr_t MM_RemoteResolveExport(HANDLE hProcess, DWORD pid, uintptr_t moduleBase,
                                        const char *funcName, WORD ordinal, int depth)
{
  IMAGE_DOS_HEADER dos = {};
  if(!MM_ReadRemote(hProcess, (void *)moduleBase, &dos, sizeof(dos)) ||
     dos.e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  IMAGE_NT_HEADERS64 nt = {};
  if(!MM_ReadRemote(hProcess, (void *)(moduleBase + dos.e_lfanew), &nt, sizeof(nt)) ||
     nt.Signature != IMAGE_NT_SIGNATURE)
    return 0;
  IMAGE_DATA_DIRECTORY exportDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if(exportDir.Size == 0 || exportDir.Size > 0x100000)
    return 0;

  rdcarray<uint8_t> exportBuf;
  exportBuf.resize(exportDir.Size);
  if(!MM_ReadRemote(hProcess, (void *)(moduleBase + exportDir.VirtualAddress), exportBuf.data(),
                    exportDir.Size))
    return 0;
  IMAGE_EXPORT_DIRECTORY *exp = (IMAGE_EXPORT_DIRECTORY *)exportBuf.data();

  rdcarray<DWORD> funcRvas;
  funcRvas.resize(exp->NumberOfFunctions);
  if(!MM_ReadRemote(hProcess, (void *)(moduleBase + exp->AddressOfFunctions), funcRvas.data(),
                    exp->NumberOfFunctions * sizeof(DWORD)))
    return 0;

  DWORD finalRva = 0;
  if(funcName)
  {
    rdcarray<DWORD> nameRvas;
    nameRvas.resize(exp->NumberOfNames);
    if(!MM_ReadRemote(hProcess, (void *)(moduleBase + exp->AddressOfNames), nameRvas.data(),
                      exp->NumberOfNames * sizeof(DWORD)))
      return 0;
    rdcarray<WORD> ordTable;
    ordTable.resize(exp->NumberOfNames);
    if(!MM_ReadRemote(hProcess, (void *)(moduleBase + exp->AddressOfNameOrdinals), ordTable.data(),
                      exp->NumberOfNames * sizeof(WORD)))
      return 0;

    for(DWORD i = 0; i < exp->NumberOfNames; i++)
    {
      char buf[256] = {};
      if(!MM_ReadRemoteCString(hProcess, (void *)(moduleBase + nameRvas[i]), buf, sizeof(buf)))
        continue;
      if(!strcmp(buf, funcName))
      {
        WORD idx = ordTable[i];
        if(idx < exp->NumberOfFunctions)
          finalRva = funcRvas[idx];
        break;
      }
    }
  }
  else
  {
    DWORD idx = (DWORD)ordinal - exp->Base;
    if(idx < exp->NumberOfFunctions)
      finalRva = funcRvas[idx];
  }
  if(!finalRva)
    return 0;

  // Forwarder check: function RVA points into the export directory's data
  // range -> the "function" is actually an ASCII forwarder string.
  if(finalRva >= exportDir.VirtualAddress && finalRva < exportDir.VirtualAddress + exportDir.Size)
  {
    DWORD off = finalRva - exportDir.VirtualAddress;
    if(off >= exportDir.Size)
      return 0;
    char fwd[256] = {};
    strncpy_s(fwd, (char *)exportBuf.data() + off, sizeof(fwd) - 1);
    return MM_ResolveForwarder(hProcess, pid, fwd, depth);
  }

  return moduleBase + finalRva;
}

// Apply IMAGE_REL_BASED_DIR64 / HIGHLOW relocations onto the locally-built
// shadow image, given the actual remote base.
static void MM_ApplyRelocations(uint8_t *image, IMAGE_NT_HEADERS64 *nt, int64_t delta)
{
  IMAGE_DATA_DIRECTORY reloc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
  if(!reloc.VirtualAddress || !reloc.Size)
    return;
  uint8_t *p = image + reloc.VirtualAddress;
  uint8_t *end = p + reloc.Size;
  while(p + sizeof(IMAGE_BASE_RELOCATION) <= end)
  {
    IMAGE_BASE_RELOCATION *blk = (IMAGE_BASE_RELOCATION *)p;
    if(blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) || blk->SizeOfBlock > (DWORD)(end - p))
      break;
    DWORD count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
    WORD *entries = (WORD *)(p + sizeof(IMAGE_BASE_RELOCATION));
    for(DWORD i = 0; i < count; i++)
    {
      WORD type = (WORD)(entries[i] >> 12);
      WORD off = (WORD)(entries[i] & 0xFFF);
      uint8_t *target = image + blk->VirtualAddress + off;
      switch(type)
      {
        case IMAGE_REL_BASED_ABSOLUTE: break;
        case IMAGE_REL_BASED_DIR64: *(uint64_t *)target += (uint64_t)delta; break;
        case IMAGE_REL_BASED_HIGHLOW: *(uint32_t *)target += (uint32_t)delta; break;
        default:
          RDCWARN("ManualMap: unhandled reloc type %u at RVA 0x%x+0x%x", type, blk->VirtualAddress,
                  off);
          break;
      }
    }
    p += blk->SizeOfBlock;
  }
}

// Resolve every import in the locally-built shadow image, patching the IAT
// (FirstThunk) in-place. Returns false on any unresolved import.
static bool MM_ResolveImports(HANDLE hProcess, DWORD pid, uint8_t *image,
                              IMAGE_NT_HEADERS64 *nt)
{
  IMAGE_DATA_DIRECTORY imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if(!imp.VirtualAddress || !imp.Size)
    return true;

  IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR *)(image + imp.VirtualAddress);
  for(; desc->Name; desc++)
  {
    const char *dllName = (const char *)(image + desc->Name);

    // Look up the dependency in the target. For real DLLs not yet loaded
    // (ws2_32, setupapi, version, ...) MM_EnsureRemoteModule will trigger
    // a target-side LoadLibraryA via thread hijack. API Set names
    // intentionally return 0 here -- their real backing DLL is reached
    // through a different import (kernel32 -> kernelbase), so the
    // per-function host-side path resolves them just fine.
    uintptr_t modBase = MM_EnsureRemoteModule(hProcess, pid, dllName);

    DWORD nameThunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
    IMAGE_THUNK_DATA64 *nameThunk = (IMAGE_THUNK_DATA64 *)(image + nameThunkRva);
    IMAGE_THUNK_DATA64 *iatThunk = (IMAGE_THUNK_DATA64 *)(image + desc->FirstThunk);

    for(; nameThunk->u1.AddressOfData; nameThunk++, iatThunk++)
    {
      uintptr_t addr = 0;
      const char *funcName = NULL;
      WORD ord = 0;
      bool byOrdinal = IMAGE_SNAP_BY_ORDINAL64(nameThunk->u1.Ordinal);
      if(byOrdinal)
      {
        ord = (WORD)IMAGE_ORDINAL64(nameThunk->u1.Ordinal);
      }
      else
      {
        IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(image + nameThunk->u1.AddressOfData);
        funcName = ibn->Name;
      }

      // 1) Try the target's own copy (handles forwarders to API Sets via
      // MM_RemoteResolveExport -> MM_ResolveForwarder, which itself prefers
      // host-side resolution).
      if(modBase)
        addr = MM_RemoteResolveExport(hProcess, pid, modBase, funcName, ord, 0);

      // 2) Fall back to host-side resolution. Required for imports whose
      // DLL is an API Set (no real module in the target under the schema
      // name), and also a safety net for exports that don't resolve via
      // the target tables (e.g. version skew during a Windows patch).
      // Pass modBase as the known target base when we have it, to save
      // per-import Toolhelp32 lookups.
      if(!addr)
        addr = MM_ResolveViaHost(hProcess, pid, dllName, funcName, ord, modBase);

      if(!addr)
      {
        if(byOrdinal)
          RDCERR("ManualMap: couldn't resolve ordinal #%u in '%s'", (unsigned)ord, dllName);
        else
          RDCERR("ManualMap: couldn't resolve '%s' in '%s'", funcName, dllName);
        return false;
      }
      iatThunk->u1.Function = (ULONGLONG)addr;
    }
  }
  return true;
}

static DWORD MM_SectionToProtection(DWORD chars)
{
  bool exec = (chars & IMAGE_SCN_MEM_EXECUTE) != 0;
  bool read = (chars & IMAGE_SCN_MEM_READ) != 0;
  bool write = (chars & IMAGE_SCN_MEM_WRITE) != 0;
  if(exec && write)
    return PAGE_EXECUTE_READWRITE;
  if(exec && read)
    return PAGE_EXECUTE_READ;
  if(exec)
    return PAGE_EXECUTE;
  if(write)
    return PAGE_READWRITE;
  if(read)
    return PAGE_READONLY;
  return PAGE_NOACCESS;
}

static void MM_ApplyProtections(HANDLE hProcess, void *remoteImage, IMAGE_NT_HEADERS64 *nt)
{
  DWORD oldProt = 0;
  // headers: read-only is sufficient (PE loader leaves them readable for
  // GetProcAddress et al). We don't insert into PEB.Ldr but TinecmaTool may
  // poke its own headers, so keep them readable.
  if(nt->OptionalHeader.SizeOfHeaders > 0)
    VirtualProtectEx(hProcess, remoteImage, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY,
                     &oldProt);
  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  for(WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
  {
    DWORD sectSize = sec[i].Misc.VirtualSize;
    if(sectSize == 0)
      sectSize = sec[i].SizeOfRawData;
    if(sectSize == 0)
      continue;
    DWORD prot = MM_SectionToProtection(sec[i].Characteristics);
    VirtualProtectEx(hProcess, (uint8_t *)remoteImage + sec[i].VirtualAddress, sectSize, prot,
                     &oldProt);
  }
}

// Set up a TLS slot for the hijacked thread so that DllMain (and code it calls
// on this thread) can access __declspec(thread) / thread_local variables.
// IMPORTANT LIMITATION: this only initialises the slot for the *current*
// (hijacked) thread. Other live threads in the target will read NULL out of
// their slot when first touching a thread-local, so any code path that
// dispatches thread-local-dependent work onto a foreign thread before that
// thread has independently faulted in TLS will misbehave. RenderDoc's capture
// hooks are predominantly main-thread-driven, so this is good enough in
// practice; document the caveat in TINECMATOOL_MANUALMAP_INJECT.md.
static bool MM_SetupTls(HANDLE hProcess, DWORD pid, uintptr_t remoteBase, uint8_t *shadow,
                        IMAGE_NT_HEADERS64 *nt)
{
  IMAGE_DATA_DIRECTORY tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if(tlsDir.Size == 0)
    return true;

  IMAGE_TLS_DIRECTORY64 *tls = (IMAGE_TLS_DIRECTORY64 *)(shadow + tlsDir.VirtualAddress);
  uint64_t rawStart = tls->StartAddressOfRawData;
  uint64_t rawEnd = tls->EndAddressOfRawData;
  if(rawEnd < rawStart)
    return false;
  uint64_t rawSize = rawEnd - rawStart;
  uint64_t zeroFill = tls->SizeOfZeroFill;

  if(rawStart < remoteBase ||
     rawStart - remoteBase >= (uint64_t)nt->OptionalHeader.SizeOfImage)
    return false;
  uint64_t shadowRawOff = rawStart - remoteBase;

  uint64_t indexAddr = tls->AddressOfIndex;
  if(indexAddr < remoteBase ||
     indexAddr - remoteBase + sizeof(DWORD) > (uint64_t)nt->OptionalHeader.SizeOfImage)
    return false;

  HMODULE k = GetModuleHandleA("kernel32.dll");
  if(!k)
    return false;
  uintptr_t tlsAlloc = (uintptr_t)GetProcAddress(k, "TlsAlloc");
  uintptr_t tlsSetValue = (uintptr_t)GetProcAddress(k, "TlsSetValue");
  if(!tlsAlloc || !tlsSetValue)
    return false;

  uint64_t allocRet = 0;
  rdcarray<uintptr_t> noArgs;
  if(!ThreadHijackCall(hProcess, pid, tlsAlloc, noArgs, &allocRet, "MM-TlsAlloc"))
    return false;
  DWORD slot = (DWORD)allocRet;
  if(slot == TLS_OUT_OF_INDEXES)
  {
    RDCERR("ManualMap: TlsAlloc returned TLS_OUT_OF_INDEXES");
    return false;
  }
  RDCLOG("ManualMap: TLS slot %u allocated for module @ 0x%llx", slot, (uint64_t)remoteBase);

  // Patch _tls_index in the remote image.
  SIZE_T n = 0;
  if(!WriteProcessMemory(hProcess, (void *)indexAddr, &slot, sizeof(slot), &n) || n != sizeof(slot))
    return false;

  // Allocate the per-thread TLS data block in the target and copy raw_data.
  SIZE_T blockSize = (SIZE_T)(rawSize + zeroFill + 16);
  void *remoteTls =
      VirtualAllocEx(hProcess, NULL, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(!remoteTls)
    return false;
  if(rawSize)
  {
    if(!WriteProcessMemory(hProcess, remoteTls, shadow + shadowRawOff, (SIZE_T)rawSize, &n) ||
       n != rawSize)
    {
      VirtualFreeEx(hProcess, remoteTls, 0, MEM_RELEASE);
      return false;
    }
  }

  rdcarray<uintptr_t> setArgs;
  setArgs.push_back((uintptr_t)slot);
  setArgs.push_back((uintptr_t)remoteTls);
  if(!ThreadHijackCall(hProcess, pid, tlsSetValue, setArgs, NULL, "MM-TlsSetValue"))
  {
    VirtualFreeEx(hProcess, remoteTls, 0, MEM_RELEASE);
    return false;
  }
  return true;
}

// Walk the TLS callback array (NULL-terminated PIMAGE_TLS_CALLBACK[]) in the
// shadow image, returning the (already-relocated) absolute addresses.
static rdcarray<uintptr_t> MM_GatherTlsCallbacks(uintptr_t remoteBase, uint8_t *shadow,
                                                 IMAGE_NT_HEADERS64 *nt)
{
  rdcarray<uintptr_t> cbs;
  IMAGE_DATA_DIRECTORY tlsDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
  if(tlsDir.Size == 0)
    return cbs;
  IMAGE_TLS_DIRECTORY64 *tls = (IMAGE_TLS_DIRECTORY64 *)(shadow + tlsDir.VirtualAddress);
  if(!tls->AddressOfCallBacks)
    return cbs;
  uint64_t cbAddr = tls->AddressOfCallBacks;
  if(cbAddr < remoteBase ||
     cbAddr - remoteBase >= (uint64_t)nt->OptionalHeader.SizeOfImage)
    return cbs;
  uint64_t *p = (uint64_t *)(shadow + (cbAddr - remoteBase));
  while(*p)
  {
    cbs.push_back((uintptr_t)*p);
    p++;
  }
  return cbs;
}

// Register the x64 exception table so SEH / stack unwind works inside the
// manually mapped module. Best-effort: failure isn't fatal but logs.
static void MM_RegisterExceptionTable(HANDLE hProcess, DWORD pid, uintptr_t remoteBase,
                                      IMAGE_NT_HEADERS64 *nt)
{
  IMAGE_DATA_DIRECTORY excp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if(excp.Size == 0 || excp.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0)
    return;
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  if(!ntdll)
    return;
  uintptr_t rtlAdd = (uintptr_t)GetProcAddress(ntdll, "RtlAddFunctionTable");
  if(!rtlAdd)
    return;
  DWORD count = excp.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
  rdcarray<uintptr_t> args;
  args.push_back(remoteBase + excp.VirtualAddress);
  args.push_back((uintptr_t)count);
  args.push_back(remoteBase);
  uint64_t ret = 0;
  if(!ThreadHijackCall(hProcess, pid, rtlAdd, args, &ret, "MM-RtlAddFunctionTable"))
  {
    RDCWARN("ManualMap: RtlAddFunctionTable failed; SEH unwind may be unreliable");
  }
}

// Main entry: returns the remote image base on success, 0 on any failure
// (the caller should fall back to the thread-hijack LoadLibrary path).
static uintptr_t InjectDLL_ManualMap(HANDLE hProcess, DWORD pid, const rdcwstr &libName)
{
  rdcarray<uint8_t> fileBytes;
  if(!MM_ReadFileBytes(libName.c_str(), fileBytes))
  {
    RDCWARN("ManualMap: couldn't read DLL '%ls' (err %u); fallback engaged", libName.c_str(),
            GetLastError());
    return 0;
  }
  if(fileBytes.size() < sizeof(IMAGE_DOS_HEADER))
    return 0;

  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)fileBytes.data();
  if(dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  if((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > fileBytes.size())
    return 0;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(fileBytes.data() + dos->e_lfanew);
  if(nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  if(nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
  {
    RDCWARN("ManualMap: '%ls' is not AMD64; falling back", libName.c_str());
    return 0;
  }
  if(nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    return 0;
  if(nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
    return 0;
  if(nt->OptionalHeader.SizeOfImage == 0 ||
     nt->OptionalHeader.SizeOfImage > 512 * 1024 * 1024u)
    return 0;

  size_t imageSize = nt->OptionalHeader.SizeOfImage;
  rdcarray<uint8_t> shadow;
  shadow.resize(imageSize);
  memset(shadow.data(), 0, imageSize);

  if(nt->OptionalHeader.SizeOfHeaders > imageSize)
    return 0;
  memcpy(shadow.data(), fileBytes.data(), nt->OptionalHeader.SizeOfHeaders);

  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  for(WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
  {
    if(sec[i].SizeOfRawData == 0)
      continue;
    if((size_t)sec[i].VirtualAddress + sec[i].SizeOfRawData > imageSize)
      return 0;
    if((size_t)sec[i].PointerToRawData + sec[i].SizeOfRawData > fileBytes.size())
      return 0;
    memcpy(shadow.data() + sec[i].VirtualAddress, fileBytes.data() + sec[i].PointerToRawData,
           sec[i].SizeOfRawData);
  }

  // Try the preferred image base first so we can keep relocations a no-op.
  void *remoteImage = VirtualAllocEx(hProcess, (LPVOID)nt->OptionalHeader.ImageBase, imageSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if(!remoteImage)
  {
    remoteImage = VirtualAllocEx(hProcess, NULL, imageSize, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
  }
  if(!remoteImage)
  {
    RDCERR("ManualMap: VirtualAllocEx(%zu bytes) failed (err %u)", imageSize, GetLastError());
    return 0;
  }

  int64_t delta = (int64_t)((uintptr_t)remoteImage - nt->OptionalHeader.ImageBase);
  if(delta != 0)
    MM_ApplyRelocations(shadow.data(), nt, delta);

  if(!MM_ResolveImports(hProcess, pid, shadow.data(), nt))
  {
    VirtualFreeEx(hProcess, remoteImage, 0, MEM_RELEASE);
    return 0;
  }

  // Commit the fully-resolved image to the target in one shot.
  SIZE_T n = 0;
  if(!WriteProcessMemory(hProcess, remoteImage, shadow.data(), imageSize, &n) || n != imageSize)
  {
    RDCERR("ManualMap: WriteProcessMemory(image) failed (err %u)", GetLastError());
    VirtualFreeEx(hProcess, remoteImage, 0, MEM_RELEASE);
    return 0;
  }

  MM_ApplyProtections(hProcess, remoteImage, nt);

  if(!MM_SetupTls(hProcess, pid, (uintptr_t)remoteImage, shadow.data(), nt))
  {
    RDCWARN("ManualMap: TLS setup failed; continuing without per-thread storage (may crash on "
            "first thread_local access)");
  }

  MM_RegisterExceptionTable(hProcess, pid, (uintptr_t)remoteImage, nt);

  // TLS callbacks (run on the hijacked thread, in declaration order).
  {
    rdcarray<uintptr_t> cbs = MM_GatherTlsCallbacks((uintptr_t)remoteImage, shadow.data(), nt);
    for(size_t i = 0; i < cbs.size(); i++)
    {
      rdcarray<uintptr_t> args;
      args.push_back((uintptr_t)remoteImage);
      args.push_back((uintptr_t)DLL_PROCESS_ATTACH);
      args.push_back(0);
      if(!ThreadHijackCall(hProcess, pid, cbs[i], args, NULL, "MM-TLSCallback"))
      {
        RDCERR("ManualMap: TLS callback %zu failed", i);
        // Don't free the image - whatever did run may have grabbed handles.
        return 0;
      }
    }
  }

  // DllMain (entry point).
  uintptr_t entry = (uintptr_t)remoteImage + nt->OptionalHeader.AddressOfEntryPoint;
  if(nt->OptionalHeader.AddressOfEntryPoint != 0)
  {
    rdcarray<uintptr_t> args;
    args.push_back((uintptr_t)remoteImage);
    args.push_back((uintptr_t)DLL_PROCESS_ATTACH);
    args.push_back(0);
    uint64_t ret = 0;
    if(!ThreadHijackCall(hProcess, pid, entry, args, &ret, "MM-DllMain"))
    {
      RDCERR("ManualMap: DllMain invocation failed");
      return 0;
    }
    if(ret == 0)
    {
      RDCERR("ManualMap: DllMain returned FALSE for '%ls'", libName.c_str());
      return 0;
    }
  }

  RDCLOG("ManualMap: '%ls' mapped at 0x%llx (size=%zu, delta=%lld)", libName.c_str(),
         (uint64_t)remoteImage, imageSize, (long long)delta);

  return (uintptr_t)remoteImage;
}

#else    // !x64

static uintptr_t InjectDLL_ManualMap(HANDLE, DWORD, const rdcwstr &)
{
  // 32-bit manual map not implemented; let the caller fall back.
  return 0;
}

#endif    // x64

}    // anonymous namespace

// Returns the remote module base when the manual-map path is taken (the
// module is *not* present in PEB.Ldr in that case, so FindRemoteDLL cannot
// see it; callers must prefer the return value over FindRemoteDLL). Returns 0
// when manual-map was bypassed and a real LoadLibrary path completed -- in
// that case FindRemoteDLL works as before.
uintptr_t InjectDLL(HANDLE hProcess, rdcwstr libName)
{
  DWORD pid = GetProcessId(hProcess);

#if TINECMATOOL_USE_MANUALMAP_INJECT
  {
    uintptr_t mappedBase = InjectDLL_ManualMap(hProcess, pid, libName);
    if(mappedBase != 0)
      return mappedBase;
    RDCWARN("Manual-map inject failed for '%ls'; falling back to thread-hijack LoadLibrary",
            libName.c_str());
  }
#endif

#if TINECMATOOL_USE_THREADHIJACK_INJECT
  if(InjectDLL_ThreadHijack(hProcess, pid, libName))
    return 0;
  RDCWARN("Thread-hijack DLL inject failed; falling back to CreateRemoteThread for '%ls'",
          libName.c_str());
#endif

  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return 0;
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(remoteMem)
  {
    BOOL success = WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, sizeof(dllPath), NULL);
    if(success)
    {
      HANDLE hThread = CreateRemoteThread(
          hProcess, NULL, 1024 * 1024U,
          (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW"), remoteMem, 0, NULL);
      if(hThread)
      {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
      }
      else
      {
        RDCERR("Couldn't create remote thread for LoadLibraryW: %u", GetLastError());
      }
    }
    else
    {
      RDCERR("Couldn't write remote memory %p with dllPath '%ls': %u", remoteMem, dllPath,
             GetLastError());
    }

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  }
  else
  {
    RDCERR("Couldn't allocate remote memory for DLL '%ls': %u", libName.c_str(), GetLastError());
  }
  return 0;
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

void InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

#if TINECMATOOL_USE_THREADHIJACK_INJECT
  if(InjectFunctionCall_ThreadHijack(hProcess, GetProcessId(hProcess), func_remote, data, dataLen,
                                     funcName))
    return;
  RDCWARN("Thread-hijack call inject failed for %s; falling back to CreateRemoteThread", funcName);
#endif

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  HANDLE hThread =
      CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
  WaitForSingleObject(hThread, INFINITE);

  ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }

  return pi;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit renderdoccmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build. Please run a "
                       "64-bit build");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness renderdoccmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\TinecmaToolcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\Win32\\Release\\TinecmaToolcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\TinecmaToolcmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\TinecmaToolcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\x64\\Release\\TinecmaToolcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\TinecmaToolcmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --capfile=\"%ls\" --debuglog=\"%ls\" --capopts=\"%hs\"",
        renderdocPath, pid, wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false,
                                   CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit renderdoccmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit renderdoccmd to capture 32-bit program."
          "If this is a locally built tool you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit renderdoccmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  uintptr_t manualMapBase = InjectDLL(hProcess, renderdocPath);

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);

  // Manual-mapped modules don't appear in PEB.Ldr, so trust InjectDLL's
  // returned base in that case; otherwise fall back to module enumeration.
  uintptr_t loc =
      manualMapBase ? manualMapBase : FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");

  CloseHandle(hProcess);
  hProcess = NULL;

  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  if(loc == 0)
  {
    SET_ERROR_RESULT(
        result.first, ResultCode::InjectionFailed,
        "Failed to inject %s.dll into process. Check that the process did not crash or exit "
        "early in initialisation, e.g. if the working directory is incorrectly set.",
        rdoc_dll);
  }
  else
  {
    hProcess =
        OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                        PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                    FALSE, pid);

    if(!hProcess)
    {
      SET_ERROR_RESULT(result.first, ResultCode::InjectionFailed,
                       "Couldn't reopen process %u after injection (err %u).", pid, GetLastError());
    }
    else
    {
      // safe to cast away the const as we know these functions don't modify the parameters

      if(!capturefile.empty())
        InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureFile", (void *)capturefile.c_str(),
                           capturefile.size() + 1);

      rdcstr debugLogfile = RDCGETLOGFILE();

      InjectFunctionCall(hProcess, loc, "INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(),
                         debugLogfile.size() + 1);

      InjectFunctionCall(hProcess, loc, "INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts,
                         sizeof(CaptureOptions));

      InjectFunctionCall(hProcess, loc, "INTERNAL_GetTargetControlIdent", &result.second,
                         sizeof(result.second));

      if(!env.empty())
      {
        for(const EnvironmentModification &e : env)
        {
          rdcstr name = e.name.trimmed();
          rdcstr value = e.value;
          EnvMod mod = e.mod;
          EnvSep sep = e.sep;

          if(name == "")
            break;

          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModName", (void *)name.c_str(),
                             name.size() + 1);
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvModValue", (void *)value.c_str(),
                             value.size() + 1);
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvSep", &sep, sizeof(sep));
          InjectFunctionCall(hProcess, loc, "INTERNAL_EnvMod", &mod, sizeof(mod));
        }

        // parameter is unused
        void *dummy = NULL;
        InjectFunctionCall(hProcess, loc, "INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy));
      }
    }
  }

  if(waitForExit && hProcess)
    WaitForSingleObject(hProcess, INFINITE);

  if(hProcess)
    CloseHandle(hProcess);

  return result;
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons this tool does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Failed to launch process.");
    return {result, 0};
  }

  rdcpair<RDResult, uint32_t> ret = InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false);

  CloseHandle(pi.hProcess);
  ResumeThread(pi.hThread);
  ResumeThread(pi.hThread);

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
                      "Check that the tool is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "The tool is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where the tool is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "The tool is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where the tool is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"TinecmaTool_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native renderdoccmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\TinecmaToolcmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just renderdocshim64.dll
  shimpathNative = renderdocPath + "\\TinecmaToolshim64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\TinecmaToolshim32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\TinecmaToolcmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\TinecmaToolshim32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\TinecmaToolcmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\TinecmaToolshim32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\TinecmaToolcmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\TinecmaToolshim32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
