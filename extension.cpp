/**
 * csgo_steamfix - SourceMod Extension
 *
 * Patches the CS:GO dedicated server engine to allow clients using the
 * archived CS:GO build (Steam app 4465480) to connect to community servers.
 */

#include "extension.h"
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <fcntl.h>
#endif

CCSGOSteamFix g_SteamFix;
SMEXT_LINK(&g_SteamFix);

// Linux:   FF 24 85 ?? ?? ?? ?? 8D B4 26 ?? ?? ?? ?? 31 F6
static const unsigned char g_sigLinux[]  = { 0xFF, 0x24, 0x85, 0x00, 0x00, 0x00, 0x00, 0x8D, 0xB4, 0x26, 0x00, 0x00, 0x00, 0x00, 0x31, 0xF6 };
static const unsigned char g_maskLinux[] = { 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
static const size_t g_sigLinuxLen = sizeof(g_sigLinux);

// Windows: FF 24 85 ?? ?? ?? ?? FF 75 ?? 68
static const unsigned char g_sigWin[]    = { 0xFF, 0x24, 0x85, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x75, 0x00, 0x68 };
static const unsigned char g_maskWin[]   = { 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF };
static const size_t g_sigWinLen = sizeof(g_sigWin);

static bool      g_patched    = false;
static uint32_t  g_origValue  = 0;
static uint32_t *g_patchAddr  = NULL;

static bool SetMemoryWritable(void *addr, size_t len)
{
#ifdef _WIN32
    DWORD oldProt;
    return VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &oldProt) != 0;
#else
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t base = (uintptr_t)addr & ~((uintptr_t)pageSize - 1);
    size_t totalLen = ((uintptr_t)addr + len) - base;
    return mprotect((void *)base, totalLen, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
#endif
}

static void *SigScan(void *base, size_t len, const unsigned char *sig, const unsigned char *mask, size_t sigLen)
{
    unsigned char *start = (unsigned char *)base;
    unsigned char *end   = start + len - sigLen;

    for (unsigned char *p = start; p <= end; ++p)
    {
        bool found = true;
        for (size_t i = 0; i < sigLen; ++i)
        {
            if (mask[i] != 0x00 && (p[i] != sig[i]))
            {
                found = false;
                break;
            }
        }
        if (found)
            return (void *)p;
    }
    return NULL;
}

struct ModuleInfo_t
{
    void  *base;
    size_t size;
};

#ifdef _WIN32

static ModuleInfo_t GetEngineModule()
{
    ModuleInfo_t info;
    info.base = NULL;
    info.size = 0;

    HMODULE hMod = GetModuleHandleA("engine.dll");
    if (!hMod)
        return info;

    MODULEINFO mi;
    memset(&mi, 0, sizeof(mi));
    if (GetModuleInformation(GetCurrentProcess(), hMod, &mi, sizeof(mi)))
    {
        info.base = mi.lpBaseOfDll;
        info.size = mi.SizeOfImage;
    }
    return info;
}

#else

static uintptr_t ParseHex(const char *s, const char **out)
{
    uintptr_t val = 0;
    while (*s)
    {
        char c = *s;
        if (c >= '0' && c <= '9')      val = (val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') val = (val << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = (val << 4) | (c - 'A' + 10);
        else break;
        s++;
    }
    if (out) *out = s;
    return val;
}

static ModuleInfo_t GetEngineModuleFromMaps(const char *needle)
{
    ModuleInfo_t result;
    result.base = NULL;
    result.size = 0;

    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0)
        return result;

    uintptr_t lo = (uintptr_t)-1, hi = 0;
    bool found = false;

    char buf[4096];
    char line[512];
    int linePos = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
        for (ssize_t i = 0; i < n; i++)
        {
            if (buf[i] == '\n' || linePos >= (int)sizeof(line) - 1)
            {
                line[linePos] = '\0';
                if (strstr(line, needle))
                {
                    found = true;
                    const char *p = line;
                    uintptr_t start = ParseHex(p, &p);
                    if (*p == '-')
                    {
                        p++;
                        uintptr_t end = ParseHex(p, &p);
                        if (start < lo) lo = start;
                        if (end   > hi) hi = end;
                    }
                }
                linePos = 0;
            }
            else
            {
                line[linePos++] = buf[i];
            }
        }
    }
    close(fd);

    if (found && lo < hi)
    {
        result.base = (void *)lo;
        result.size = (size_t)(hi - lo);
    }
    return result;
}

static ModuleInfo_t GetEngineModule()
{
    static const char *names[] = {
        "/engine.so",
        "/engine_srv.so",
        NULL
    };

    for (int i = 0; names[i]; i++)
    {
        ModuleInfo_t info = GetEngineModuleFromMaps(names[i]);
        if (info.base && info.size)
            return info;
    }

    ModuleInfo_t empty;
    empty.base = NULL;
    empty.size = 0;
    return empty;
}

#endif

static void LogMsg(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    smutils->LogMessage(myself, "%s", buf);
}

bool CCSGOSteamFix::SDK_OnLoad(char *error, size_t maxlength, bool late)
{
    ModuleInfo_t eng = GetEngineModule();
    if (!eng.base || !eng.size)
    {
        snprintf(error, maxlength, "[APPID] failed to locate engine module");
        return false;
    }

#ifdef _WIN32

    void *hit = SigScan(eng.base, eng.size, g_sigWin, g_maskWin, g_sigWinLen);
    if (!hit)
    {
        snprintf(error, maxlength, "[APPID] signature not found (Windows)");
        return false;
    }

    uint32_t jtAddr = *(uint32_t *)((unsigned char *)hit + 3);
    uint32_t *jt    = (uint32_t *)jtAddr;

    unsigned char *jaInstr = (unsigned char *)hit - 6;
    int32_t rel32 = *(int32_t *)(jaInstr + 2);
    uint32_t successVA = (uint32_t)(uintptr_t)(jaInstr + 6) + rel32;

    g_patchAddr = &jt[3];
    g_origValue = jt[3];

    if (!SetMemoryWritable(g_patchAddr, sizeof(uint32_t)))
    {
        snprintf(error, maxlength, "[APPID] VirtualProtect failed");
        return false;
    }

    *g_patchAddr = successVA;
    g_patched = true;

#else

    void *hit = SigScan(eng.base, eng.size, g_sigLinux, g_maskLinux, g_sigLinuxLen);
    if (!hit)
    {
        snprintf(error, maxlength, "[APPID] signature not found (Linux)");
        return false;
    }

    uint32_t jtAddr = *(uint32_t *)((unsigned char *)hit + 3);
    uint32_t *jt    = (uint32_t *)(uintptr_t)jtAddr;

    g_patchAddr = &jt[4];
    g_origValue = jt[4];

    if (!SetMemoryWritable(g_patchAddr, sizeof(uint32_t)))
    {
        snprintf(error, maxlength, "[APPID] mprotect failed");
        return false;
    }

    *g_patchAddr = jt[0];
    g_patched = true;

#endif

    LogMsg("[APPID] engine patched!");
    return true;
}

void CCSGOSteamFix::SDK_OnUnload()
{
    if (g_patched && g_patchAddr)
    {
        SetMemoryWritable(g_patchAddr, sizeof(uint32_t));
        *g_patchAddr = g_origValue;
        g_patched = false;
        LogMsg("[APPID] engine restored to original state");
    }
}
