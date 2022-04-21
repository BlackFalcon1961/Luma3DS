#pragma once
#include <3ds/types.h>
#include "ifile.h"

#define _3GX_MAGIC (0x3230303024584733) /* "3GX$0002" */

typedef struct PACKED
{
    u32             authorLen;
    const char*     authorMsg;
    u32             titleLen;
    const char*     titleMsg;
    u32             summaryLen;
    const char*     summaryMsg;
    u32             descriptionLen;
    const char*     descriptionMsg;
    union {
        u32         flags;
        struct {
            u32     embeddedExeDecryptFunc : 1;
            u32     embeddedSwapEncDecFunc : 1;
            u32     unused : 30;
        };
    };
    u32             exeDecChecksum;
    u32             builtInDecExeArgs[4];
    u32             builtInSwapEncDecArgs[4];
} _3gx_Infos;

typedef struct PACKED
{
    u32             count;
    u32           * titles;
}   _3gx_Targets;

typedef struct PACKED
{
    u32             nbSymbols;
    u32             symbolsOffset;
    u32             nameTableOffset;
}   _3gx_Symtable;

typedef struct PACKED
{
    u32             codeOffset;
    u32             rodataOffset;
    u32             dataOffset;
    u32             codeSize;
    u32             rodataSize;
    u32             dataSize;
    u32             bssSize;
    u32             exeDecOffset; // NOP terminated
    u32             swapEncOffset; // NOP terminated
    u32             swapDecOffset; // NOP terminated
} _3gx_Executable;

typedef struct PACKED
{
    u64             magic;
    u32             version;
    u32             reserved;
    _3gx_Infos      infos;
    _3gx_Executable executable;
    _3gx_Targets    targets;
    _3gx_Symtable   symtable;
} _3gx_Header;


Result  Check_3gx_Magic(IFile *file);
Result  Read_3gx_Header(IFile *file, _3gx_Header *header);
Result  Read_3gx_LoadSegments(IFile *file, _3gx_Header *header, void *dst);
Result  Read_3gx_EmbeddedPayloads(IFile *file, _3gx_Header *header);
Result  Set_3gx_DecParams(u32* decFunc, u32* params);
void	Reset_3gx_DecParams(void);