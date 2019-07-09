#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include "csvc.h"
#include "3gx.h"
#include "plugin.h"
#include "ifile.h"
#include "ifile.h"
#include "utils.h"

// Use a global to avoid stack overflow, those structs are quite heavy
static FS_DirectoryEntry   g_entries[10];

static char        g_path[256];
static const char *g_dirPath = "/luma/plugins/%016llX";
static const char *g_defaultPath = "/luma/plugins/default.3gx";

// pluginLoader.s
void        gamePatchFunc(void);

static u32     strlen16(const u16 *str)
{
    if (!str) return 0;

    const u16 *strEnd = str;

    while (*strEnd) ++strEnd;

    return strEnd - str;
}

static Result   FindPluginFile(u64 tid)
{
    char                filename[256];
    u32                 entriesNb = 0;
    bool                found = false;
    Handle              dir = 0;
    Result              res;
    FS_Archive          sdmcArchive = 0;
    FS_DirectoryEntry * entries = g_entries;
    
    memset(entries, 0, sizeof(g_entries));
    sprintf(g_path, g_dirPath, tid);

    if (R_FAILED((res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))))
        goto exit;

    if (R_FAILED((res = FSUSER_OpenDirectory(&dir, sdmcArchive, fsMakePath(PATH_ASCII, g_path)))))
        goto exit;

    strcat(g_path, "/");
    while (!found && R_SUCCEEDED(FSDIR_Read(dir, &entriesNb, 10, entries)))
    {
        if (entriesNb == 0)
            break;

        static const u16 *   validExtension = u"3gx";

        for (u32 i = 0; i < entriesNb; ++i)
        {
            FS_DirectoryEntry *entry = &entries[i];

            // If entry is a folder, skip it
            if (entry->attributes & 1)
                continue;

            // Check extension
            u32 size = strlen16(entry->name);
            if (size <= 5)
                continue;

            u16 *fileExt = entry->name + size - 3;

            if (memcmp(fileExt, validExtension, 3 * sizeof(u16)))
                continue;

            // Convert name from utf16 to utf8
            int units = utf16_to_utf8((u8 *)filename, entry->name, 100);
            if (units == -1)
                continue;
            filename[units] = 0;
            found = true;
            break;
        }
    }

    if (!found)
        res = MAKERESULT(28, 4, 0, 1018);
    else
    {
        u32 len = strlen(g_path);
        filename[256 - len] = 0;
        strcat(g_path, filename);
        PluginLoaderCtx.pluginPath = g_path;
    }

exit:
    FSDIR_Close(dir);
    FSUSER_CloseArchive(sdmcArchive);

    return res;
}

static Result   OpenFile(IFile *file, const char *path)
{
    return IFile_Open(file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
}

static Result   OpenPluginFile(u64 tid, IFile *plugin)
{
    if (R_FAILED(FindPluginFile(tid)) || OpenFile(plugin, g_path))
    {
        // Try to open default plugin
        if (OpenFile(plugin, g_defaultPath))
            return -1;

        PluginLoaderCtx.pluginPath = g_defaultPath;
        PluginLoaderCtx.header.isDefaultPlugin = 1;
        return 0;
    }

    return 0;
}

static Result   CheckPluginCompatibility(_3gx_Header *header, u32 processTitle)
{
    static char   errorBuf[0x100];

    if (header->targets.count == 0)
        return 0;

    for (u32 i = 0; i < header->targets.count; ++i)
    {
        if (header->targets.titles[i] == processTitle)
            return 0;
    }

    sprintf(errorBuf, "The plugin - %s -\nis not compatible with this game.\n" \
                      "Contact \"%s\" for more infos.", header->infos.titleMsg, header->infos.authorMsg);
    
    PluginLoaderCtx.error.message = errorBuf;

    return -1;
}

bool     TryToLoadPlugin(Handle process)
{
    u64             tid;
    u64             fileSize;
    IFile           plugin;
    Result          res;
    _3gx_Header     *header;
    _3gx_Executable *exeHdr;
    PluginLoaderContext *ctx = &PluginLoaderCtx;
    PluginHeader        *pluginHeader = &ctx->header;

    // Get title id
    svcGetProcessInfo((s64 *)&tid, process, 0x10001);

    memset(pluginHeader, 0, sizeof(PluginHeader));
    pluginHeader->magic = HeaderMagic;

    // Try to open plugin file
    if (ctx->useUserLoadParameters && (u32)tid == ctx->userLoadParameters.lowTitleId)
    {
        ctx->useUserLoadParameters = false;
        if (OpenFile(&plugin, ctx->userLoadParameters.path))
            return false;

        ctx->pluginPath = ctx->userLoadParameters.path;
        if (MemoryBlock__IsReady())
        {
            IFile_Close(&plugin);
            return false;
        }

        memcpy(pluginHeader->config, ctx->userLoadParameters.config, 32 * sizeof(u32));
    }
    else
    {
        if (R_FAILED(OpenPluginFile(tid, &plugin)))
            return false;

        if (MemoryBlock__IsReady())
        {
            IFile_Close(&plugin);
            return false;
        }
    }

    if (R_FAILED((res = IFile_GetSize(&plugin, &fileSize))))
        ctx->error.message = "Couldn't get file size";

    // Check 3GX file signature
    if (!res && R_FAILED(res = Check_3gx_Magic(&plugin)))
    {
        if (R_MODULE(res) == RM_LDR)
        {
            if (R_DESCRIPTION(res) == 1) ctx->error.message = "Invalid plugin file\nNot a valid 3GX plugin format!";
            if (R_DESCRIPTION(res) == 2) ctx->error.message = "Outdated plugin file\nCheck for an updated plugin";
            else if (R_DESCRIPTION(res) == 3) ctx->error.message = "Outdated plugin loader\nCheck for Luma3DS updates";
        } else ctx->error.message = "Couldn't read file";
    }
    // Plugins will not exceed 5MB so this is fine
    header = (_3gx_Header *)(ctx->memblock.memblock + MemBlockSize - (u32)fileSize);

    // Read header
    if (!res && R_FAILED((res = Read_3gx_Header(&plugin, header))))
        ctx->error.message = "Couldn't read file";

    // Check titles compatibility
    if (!res) res = CheckPluginCompatibility(header, (u32)tid);

    // Read code
    if (!res && R_FAILED(res = Read_3gx_LoadSegments(&plugin, header, ctx->memblock.memblock + sizeof(PluginHeader))))
        ctx->error.message = "Couldn't read plugin's code";

    if (R_FAILED(res))
    {
        ctx->error.code = res;
        goto exitFail;
    }

    pluginHeader->version = header->version;
    // Code size must be page aligned
    exeHdr = &header->executable;
    pluginHeader->exeSize = (sizeof(PluginHeader) + exeHdr->codeSize + exeHdr->rodataSize + exeHdr->dataSize + exeHdr->bssSize + 0x1000) & ~0xFFF;
    pluginHeader->heapVA = 0x06000000;
    pluginHeader->heapSize = MemBlockSize - pluginHeader->exeSize;
    pluginHeader->plgldrEvent = ctx->plgEventPA;
    pluginHeader->plgldrReply = ctx->plgReplyPA;

    // Clear old event data
    PLG__NotifyEvent(PLG_OK, false);

    // Copy header to memblock
    memcpy(ctx->memblock.memblock, pluginHeader, sizeof(PluginHeader));
    // Clear heap
    memset(ctx->memblock.memblock + pluginHeader->exeSize, 0, pluginHeader->heapSize);

    // Enforce RWX mmu mapping
    svcControlProcess(process, PROCESSOP_SET_MMU_TO_RWX, 0, 0);
    // Ask the kernel to signal when the process is about to be terminated
    svcControlProcess(process, PROCESSOP_SIGNAL_ON_EXIT, 0, 0);

    // Mount the plugin memory in the process
    if (R_SUCCEEDED(MemoryBlock__MountInProcess()))
    // Install hook
    {
        u32  procStart = 0x00100000;
        u32 *game = (u32 *)procStart;

        extern u32  g_savedGameInstr[2];

        if (R_FAILED((res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, process, procStart, 0x1000))))
        {
            ctx->error.message = "Couldn't map process";
            ctx->error.code = res;
            goto exitFail;
        }

        g_savedGameInstr[0] = game[0];
        g_savedGameInstr[1] = game[1];

        game[0] = 0xE51FF004; // ldr pc, [pc, #-4]
        game[1] = (u32)PA_FROM_VA_PTR(gamePatchFunc);
        svcFlushEntireDataCache();
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, procStart, 0x1000);
    }
    else
        goto exitFail;


    IFile_Close(&plugin);
    return true;

exitFail:
    IFile_Close(&plugin);
    MemoryBlock__Free();

    return false;
}