// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//*****************************************************************************
// Debug.cpp
//
// Helper code for debugging.
//*****************************************************************************
//


#include "stdafx.h"
#include "utilcode.h"
#include "ex.h"
#include "corexcep.h"

#ifdef _DEBUG
#define LOGGING
#endif


#include "log.h"

extern "C" _CRTIMP int __cdecl _flushall(void);

#ifdef HOST_WINDOWS
void CreateCrashDumpIfEnabled(bool stackoverflow = false);
#endif

// Global state counter to implement SUPPRESS_ALLOCATION_ASSERTS_IN_THIS_SCOPE.
Volatile<LONG> g_DbgSuppressAllocationAsserts = 0;

static void GetExecutableFileNameUtf8(SString& value)
{
    CONTRACTL
    {
        THROWS;
        GC_NOTRIGGER;
    }
    CONTRACTL_END;

    SString tmp;
    WCHAR * pCharBuf = tmp.OpenUnicodeBuffer(_MAX_PATH);
    DWORD numChars = GetModuleFileNameW(0 /* Get current executable */, pCharBuf, _MAX_PATH);
    tmp.CloseBuffer(numChars);

    tmp.ConvertToUTF8(value);
}

#ifdef _DEBUG


//*****************************************************************************
// This struct tracks the asserts we want to ignore in the rest of this
// run of the application.
//*****************************************************************************
struct _DBGIGNOREDATA
{
    char        rcFile[_MAX_PATH];
    int        iLine;
    bool        bIgnore;
};

typedef CDynArray<_DBGIGNOREDATA> DBGIGNORE;
static BYTE grIgnoreMemory[sizeof(DBGIGNORE)];
inline DBGIGNORE* GetDBGIGNORE()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;

    static bool fInit; // = false;
    if (!fInit)
    {
        SCAN_IGNORE_THROW; // Doesn't really throw here.
        new (grIgnoreMemory) CDynArray<_DBGIGNOREDATA>();
        fInit = true;
    }

    return (DBGIGNORE*)grIgnoreMemory;
}

// Continue the app on an assert. Still output the assert, but
// Don't throw up a GUI. This is useful for testing fatal error
// paths (like FEEE) where the runtime asserts.
BOOL ContinueOnAssert()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    static ConfigDWORD fNoGui;
    return fNoGui.val(CLRConfig::INTERNAL_ContinueOnAssert);
}

void DoRaiseExceptionOnAssert(DWORD chance)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_SUPPORTS_DAC;

#if !defined(DACCESS_COMPILE)
    if (chance)
    {
#ifndef TARGET_UNIX
        PAL_TRY_NAKED
        {
            RaiseException(EXCEPTION_INTERNAL_ASSERT, 0, 0, NULL);
        }
        PAL_EXCEPT_NAKED((chance == 1) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
        PAL_ENDTRY_NAKED
#else // TARGET_UNIX
        // For PAL always raise the exception.
        RaiseException(EXCEPTION_INTERNAL_ASSERT, 0, 0, NULL);
#endif // TARGET_UNIX
    }
#endif // !DACCESS_COMPILE
}

enum RaiseOnAssertOptions { rTestAndRaise, rTestOnly };

BOOL RaiseExceptionOnAssert(RaiseOnAssertOptions option = rTestAndRaise)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_SUPPORTS_DAC;

    // ok for debug-only code to take locks
    CONTRACT_VIOLATION(TakesLockViolation);

    DWORD fRet = 0;

#if !defined(DACCESS_COMPILE)
    static ConfigDWORD fRaiseExceptionOnAssert;
    //
    // we don't want this config key to affect mscordacwks as well!
    //
    EX_TRY
    {
        fRet = fRaiseExceptionOnAssert.val(CLRConfig::INTERNAL_RaiseExceptionOnAssert);
    }
    EX_CATCH
    {
    }
    EX_END_CATCH(SwallowAllExceptions);

    if (option == rTestAndRaise && fRet != 0)
    {
        DoRaiseExceptionOnAssert(fRet);
    }
#endif // !DACCESS_COMPILE

    return fRet != 0;
}

BOOL DebugBreakOnAssert()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_SUPPORTS_DAC;

    // ok for debug-only code to take locks
    CONTRACT_VIOLATION(TakesLockViolation);

    BOOL fRet = FALSE;

#ifndef DACCESS_COMPILE
    static ConfigDWORD fDebugBreak;
    //
    // we don't want this config key to affect mscordacwks as well!
    //
    EX_TRY
    {
        fRet = fDebugBreak.val(CLRConfig::INTERNAL_DebugBreakOnAssert);
    }
    EX_CATCH
    {
    }
    EX_END_CATCH(SwallowAllExceptions);
#endif // DACCESS_COMPILE

    return fRet;
}

VOID DECLSPEC_NORETURN TerminateOnAssert()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    ShutdownLogging();
#ifdef HOST_WINDOWS
    CreateCrashDumpIfEnabled();
#endif
    RaiseFailFastException(NULL, NULL, 0);
}

VOID LogAssert(
    LPCSTR      szFile,
    int         iLine,
    LPCSTR      szExpr
)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    // Log asserts to the stress log. Note that we can't include the szExpr b/c that
    // may not be a string literal (particularly for formatt-able asserts).
    STRESS_LOG2(LF_ASSERT, LL_ALWAYS, "ASSERT:%s, line:%d\n", szFile, iLine);

    SYSTEMTIME st;
#ifndef TARGET_UNIX
    GetLocalTime(&st);
#else
    GetSystemTime(&st);
#endif

    PathString exename;
    WszGetModuleFileName(NULL, exename);

    LOG((LF_ASSERT,
         LL_FATALERROR,
         "FAILED ASSERT(PID %d [0x%08x], Thread: %d [0x%x]) (%lu/%lu/%lu: %02lu:%02lu:%02lu %s): File: %s, Line %d : %s\n",
         GetCurrentProcessId(),
         GetCurrentProcessId(),
         GetCurrentThreadId(),
         GetCurrentThreadId(),
         (ULONG)st.wMonth,
         (ULONG)st.wDay,
         (ULONG)st.wYear,
         1 + (( (ULONG)st.wHour + 11 ) % 12),
         (ULONG)st.wMinute,
         (ULONG)st.wSecond,
         (st.wHour < 12) ? "am" : "pm",
         szFile,
         iLine,
         szExpr));
    LOG((LF_ASSERT, LL_FATALERROR, "RUNNING EXE: %ws\n", exename.GetUnicode()));
}

//*****************************************************************************

BOOL LaunchJITDebugger()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    BOOL fSuccess = FALSE;
#ifndef TARGET_UNIX
    EX_TRY
    {
        SString debugger;
        GetDebuggerSettingInfo(debugger, NULL);

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;

        // We can leave this event as it is since it is inherited by a child process.
        // We will block one scheduler, but the process is asking a user if they want to attach debugger.
        HandleHolder eventHandle = WszCreateEvent(&sa, TRUE, FALSE, NULL);
        if (eventHandle == NULL)
            ThrowOutOfMemory();

        SString cmdLine;
        cmdLine.Printf(debugger, GetCurrentProcessId(), eventHandle.GetValue());

        STARTUPINFO StartupInfo;
        memset(&StartupInfo, 0, sizeof(StartupInfo));
        StartupInfo.cb = sizeof(StartupInfo);
        StartupInfo.lpDesktop = const_cast<LPWSTR>(W("Winsta0\\Default"));

        PROCESS_INFORMATION ProcessInformation;
        if (WszCreateProcess(NULL, cmdLine, NULL, NULL, TRUE, 0, NULL, NULL, &StartupInfo, &ProcessInformation))
        {
            WaitForSingleObject(eventHandle.GetValue(), INFINITE);
        }

        fSuccess = TRUE;
    }
    EX_CATCH
    {
    }
    EX_END_CATCH(SwallowAllExceptions);
#endif // !TARGET_UNIX
    return fSuccess;
}


//*****************************************************************************
// This function is called in order to ultimately return an out of memory
// failed hresult.  But this code will check what environment you are running
// in and give an assert for running in a debug build environment.  Usually
// out of memory on a dev machine is a bogus allocation, and this allows you
// to catch such errors.  But when run in a stress envrionment where you are
// trying to get out of memory, assert behavior stops the tests.
//*****************************************************************************
HRESULT _OutOfMemory(LPCSTR szFile, int iLine)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    printf("WARNING: Out of memory condition being issued from: %s, line %d\n", szFile, iLine);
    return (E_OUTOFMEMORY);
}

static const char * szLowMemoryAssertMessage = "Assert failure (unable to format)";

//*****************************************************************************
// This function will handle ignore codes and tell the user what is happening.
//*****************************************************************************
bool _DbgBreakCheck(
    LPCSTR      szFile,
    int         iLine,
    LPCUTF8     szExpr,
    BOOL        fConstrained)
{
    STATIC_CONTRACT_THROWS;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_DEBUG_ONLY;

    DBGIGNORE* pDBGIFNORE = GetDBGIGNORE();
    _DBGIGNOREDATA *psData;
    int i;

    // Check for ignore all.
    for (i = 0, psData = pDBGIFNORE->Ptr();  i < pDBGIFNORE->Count();  i++, psData++)
    {
        if (psData->iLine == iLine && SString::_stricmp(psData->rcFile, szFile) == 0 && psData->bIgnore == true)
        {
            return false;
        }
    }

    CONTRACT_VIOLATION(FaultNotFatal | GCViolation | TakesLockViolation);

    char formatBuffer[4096];

    SString modulePath;
    BOOL formattedMessages = FALSE;

    // If we are low on memory we cannot even format a message. If this happens we want to
    // contain the exception here but display as much information as we can about the exception.
    if (!fConstrained)
    {
        EX_TRY
        {
            GetExecutableFileNameUtf8(modulePath);

            sprintf_s(formatBuffer, sizeof(formatBuffer),
                "\nAssert failure(PID %d [0x%08x], Thread: %d [0x%04x]): %s\n"
                "    File: %s Line: %d\n"
                "    Image: %s\n\n",
                GetCurrentProcessId(), GetCurrentProcessId(),
                GetCurrentThreadId(), GetCurrentThreadId(),
                szExpr, szFile, iLine, modulePath.GetUTF8());

            formattedMessages = TRUE;
        }
        EX_CATCH
        {
        }
        EX_END_CATCH(SwallowAllExceptions);
    }

    // Emit assert in debug output and console for easy access.
    if (formattedMessages)
    {
        OutputDebugStringUtf8(formatBuffer);
        fprintf(stderr, formatBuffer);
    }
    else
    {
        // Note: we cannot convert to unicode or concatenate in this situation.
        OutputDebugStringUtf8(szLowMemoryAssertMessage);
        OutputDebugStringUtf8("\n");
        OutputDebugStringUtf8(szFile);
        OutputDebugStringUtf8("\n");
        OutputDebugStringUtf8(szExpr);
        OutputDebugStringUtf8("\n");
        printf(szLowMemoryAssertMessage);
        printf("\n");
        printf(szFile);
        printf("\n");
        printf("%s", szExpr);
        printf("\n");
    }

    LogAssert(szFile, iLine, szExpr);
    FlushLogging();         // make certain we get the last part of the log
    _flushall();

    if (ContinueOnAssert())
    {
        return false;       // don't stop debugger. No gui.
    }

    if (IsDebuggerPresent() || DebugBreakOnAssert())
    {
        return true;       // like a retry
    }

    TerminateOnAssert();
    UNREACHABLE();
}

bool _DbgBreakCheckNoThrow(
    LPCSTR      szFile,
    int         iLine,
    LPCSTR      szExpr,
    BOOL        fConstrained)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_DEBUG_ONLY;

    bool failed = false;
    bool result = false;
    EX_TRY
    {
        result = _DbgBreakCheck(szFile, iLine, szExpr, fConstrained);
    }
    EX_CATCH
    {
        failed = true;
    }
    EX_END_CATCH(SwallowAllExceptions);

    if (failed)
    {
        return true;
    }
    return result;
}

#ifndef TARGET_UNIX
// Get the timestamp from the PE file header.  This is useful
unsigned DbgGetEXETimeStamp()
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_DEBUG_ONLY;

    static ULONG cache = 0;
    if (cache == 0) {
        // Use GetModuleHandleA to avoid contracts - this results in a recursive loop initializing the
        // debug allocator.
        BYTE* imageBase = (BYTE*) GetModuleHandleA(NULL);
        if (imageBase == 0)
            return(0);
        IMAGE_DOS_HEADER *pDOS = (IMAGE_DOS_HEADER*) imageBase;
        if ((pDOS->e_magic != VAL16(IMAGE_DOS_SIGNATURE)) || (pDOS->e_lfanew == 0))
            return(0);

        IMAGE_NT_HEADERS *pNT = (IMAGE_NT_HEADERS*) (VAL32(pDOS->e_lfanew) + imageBase);
        cache = VAL32(pNT->FileHeader.TimeDateStamp);
    }

    return cache;
}
#endif // TARGET_UNIX

// Called from within the IfFail...() macros.  Set a breakpoint here to break on
// errors.
VOID DebBreak()
{
  STATIC_CONTRACT_LEAF;
  static int i = 0;  // add some code here so that we'll be able to set a BP
  i++;
}

VOID DebBreakHr(HRESULT hr)
{
  STATIC_CONTRACT_LEAF;
  static int i = 0;  // add some code here so that we'll be able to set a BP
  _ASSERTE(hr != (HRESULT) 0xcccccccc);
  i++;

  // @CONSIDER: We maybe want this on retail builds.
  #ifdef _DEBUG
  static DWORD dwBreakHR = 99;

  if (dwBreakHR == 99)
         dwBreakHR = CLRConfig::GetConfigValue(CLRConfig::INTERNAL_BreakOnHR);
  if (dwBreakHR == (DWORD)hr)
  {
    _DbgBreak();
  }
  #endif
}

#ifndef TARGET_UNIX
CHAR g_szExprWithStack2[10480];
#endif
void *dbgForceToMemory;     // dummy pointer that pessimises enregistration

int g_BufferLock = -1;

VOID DbgAssertDialog(const char *szFile, int iLine, const char *szExpr)
{
    STATIC_CONTRACT_NOTHROW;
    STATIC_CONTRACT_GC_NOTRIGGER;
    STATIC_CONTRACT_FORBID_FAULT;
    STATIC_CONTRACT_SUPPORTS_DAC_HOST_ONLY;

    DEBUG_ONLY_FUNCTION;

#ifdef DACCESS_COMPILE
    // In the DAC case, asserts can mean one of two things.
    // Either there is a bug in the DAC infrastructure itself (a real assert), or just
    // that the target is corrupt or being accessed at an inconsistent state (a "target
    // consistency failure").  For target consistency failures, we need a mechanism to disable them
    // (without affecting other asserts) so that we can test corrupt / inconsistent targets.

    // @dbgtodo  DAC: For now we're treating all asserts as if they are target consistency checks.
    // In the future we should differentiate the two so that real asserts continue to fire, even when
    // we expect the target to be inconsistent.  See DevDiv Bugs 31674.
    if( !DacTargetConsistencyAssertsEnabled() )
    {
        return;
    }
#endif // #ifndef DACCESS_COMPILE

    // We increment this every time we use the SUPPRESS_ALLOCATION_ASSERTS_IN_THIS_SCOPE
    // macro below.  If it is a big number it means either a lot of threads are asserting
    // or we have a recursion in the Assert logic (usually the latter).  At least with this
    // code in place, we don't get stack overflow (and the process torn down).
    // the correct fix is to avoid calling asserting when allocating memory with an assert.
    if (g_DbgSuppressAllocationAsserts > 16)
        DebugBreak();

    SUPPRESS_ALLOCATION_ASSERTS_IN_THIS_SCOPE;

    // Raising the assert dialog can cause us to re-enter the host when allocating
    // memory for the string.  Since this is debug-only code, we can safely skip
    // violation asserts here, particularly since they can also cause infinite
    // recursion.
    PERMANENT_CONTRACT_VIOLATION(HostViolation, ReasonDebugOnly);

    dbgForceToMemory = &szFile;     //make certain these args are available in the debugger
    dbgForceToMemory = &iLine;
    dbgForceToMemory = &szExpr;

    RaiseExceptionOnAssert(rTestAndRaise);

    BOOL fConstrained = FALSE;

    DWORD dwAssertStacktrace = CLRConfig::GetConfigValue(CLRConfig::INTERNAL_AssertStacktrace);

    LONG lAlreadyOwned = InterlockedExchange((LPLONG)&g_BufferLock, 1);
    if (fConstrained || dwAssertStacktrace == 0 || lAlreadyOwned == 1)
    {
        if (_DbgBreakCheckNoThrow(szFile, iLine, szExpr, fConstrained))
        {
            _DbgBreak();
        }
    }
    else
    {
        char *szExprToDisplay = (char*)szExpr;
#ifdef TARGET_UNIX
        BOOL fGotStackTrace = TRUE;
#else
        BOOL fGotStackTrace = FALSE;
#ifndef DACCESS_COMPILE
        EX_TRY
        {
            FAULT_NOT_FATAL();
            szExprToDisplay = &g_szExprWithStack2[0];
            strcpy(szExprToDisplay, szExpr);
            strcat_s(szExprToDisplay, ARRAY_SIZE(g_szExprWithStack2), "\n\n");
            GetStringFromStackLevels(1, 10, szExprToDisplay + strlen(szExprToDisplay));
            fGotStackTrace = TRUE;
        }
        EX_CATCH
        {
        }
        EX_END_CATCH(SwallowAllExceptions);
#endif  // DACCESS_COMPILE
#endif  // TARGET_UNIX

        if (_DbgBreakCheckNoThrow(szFile, iLine, szExprToDisplay, !fGotStackTrace))
        {
            _DbgBreak();
        }

        g_BufferLock = 0;
    }
} // DbgAssertDialog

#if !defined(DACCESS_COMPILE)
//-----------------------------------------------------------------------------
// Returns an a stacktrace for a given context.
// Very useful inside exception filters.
// Returns true if successful, false on failure (such as OOM).
// This never throws.
//-----------------------------------------------------------------------------
bool GetStackTraceAtContext(SString & s, CONTEXT * pContext)
{
    SUPPRESS_ALLOCATION_ASSERTS_IN_THIS_SCOPE;
    STATIC_CONTRACT_DEBUG_ONLY;

     // NULL means use the current context.
    bool fSuccess = false;

    FAULT_NOT_FATAL();

#ifndef TARGET_UNIX
    EX_TRY
    {
        const int cTotal = cfrMaxAssertStackLevels - 1;
        // If we have a supplied context, then don't skip any frames. Else we'll
        // be using the current context, so skip this frame.
        const int cSkip = (pContext == NULL) ? 1 : 0;
        char * szString = s.OpenUTF8Buffer(cchMaxAssertStackLevelStringLen * cTotal);
        GetStringFromStackLevels(cSkip, cTotal, szString, pContext);
        s.CloseBuffer((COUNT_T) strlen(szString));

        // If we made it this far w/o throwing, we succeeded.
        fSuccess = true;
    }
    EX_CATCH
    {
        // Nothing to do here.
    }
    EX_END_CATCH(SwallowAllExceptions);
#endif // TARGET_UNIX

    return fSuccess;
} // GetStackTraceAtContext
#endif // !defined(DACCESS_COMPILE)
#endif // _DEBUG

/****************************************************************************
   The following two functions are defined to allow Free builds to call
   DebugBreak or to Assert with a stack trace for unexpected fatal errors.
   Typically these paths are enabled via a registry key in a Free Build
*****************************************************************************/

VOID __FreeBuildDebugBreak()
{
    WRAPPER_NO_CONTRACT; // If we're calling this, we're well past caring about contract consistency!

    if (CLRConfig::GetConfigValue(CLRConfig::INTERNAL_BreakOnRetailAssert))
    {
        DebugBreak();
    }
}

void *freForceToMemory;     // dummy pointer that pessimises enregistration

void DECLSPEC_NORETURN __FreeBuildAssertFail(const char *szFile, int iLine, const char *szExpr)
{
    WRAPPER_NO_CONTRACT; // If we're calling this, we're well past caring about contract consistency!

    freForceToMemory = &szFile;     //make certain these args are available in the debugger
    freForceToMemory = &iLine;
    freForceToMemory = &szExpr;

    __FreeBuildDebugBreak();

    SString buffer;
    SString modulePath;

    GetExecutableFileNameUtf8(modulePath);

    buffer.Printf("CLR: Assert failure(PID %d [0x%08x], Thread: %d [0x%x]): %s\n"
                "    File: %s, Line: %d Image:\n%s\n",
                GetCurrentProcessId(), GetCurrentProcessId(),
                GetCurrentThreadId(), GetCurrentThreadId(),
                szExpr, szFile, iLine, modulePath.GetUTF8());
    OutputDebugStringUtf8(buffer.GetUTF8());

    // Write out the error to the console
    printf(buffer.GetUTF8());

    // Log to the stress log. Note that we can't include the szExpr b/c that
    // may not be a string literal (particularly for formatt-able asserts).
    STRESS_LOG2(LF_ASSERT, LL_ALWAYS, "ASSERT:%s, line:%d\n", szFile, iLine);

    FlushLogging();         // make certain we get the last part of the log

    _flushall();

    ShutdownLogging();

#ifdef HOST_WINDOWS
    CreateCrashDumpIfEnabled();
#endif
    RaiseFailFastException(NULL, NULL, 0);

    UNREACHABLE();
}
