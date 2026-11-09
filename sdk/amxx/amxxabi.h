#pragma once

// The part of the AMX Mod X module ABI this plugin needs, copied from AMXX
// 1.10 (public/sdk/amxxmodule.h and amxmodx/amx.h). The full module SDK is
// 150 KB of macro configuration for natives, forwards and a Metamod half that
// this module has none of: it registers no natives and only ever asks for
// three function pointers.
//
// Everything here is ABI, not convenience. AMXX_INTERFACE_VERSION is checked
// against the core at load, and the structures below are read out of memory
// the core owns, so they must match its layout exactly. Update against the
// AMXX source if either changes.

#include <cstdint>

#define AMXX_INTERFACE_VERSION 4

#define AMXX_OK                0
#define AMXX_IFVERS            1
#define AMXX_PARAM             2
#define AMXX_FUNC_NOT_PRESENT  3

#define AMXX_GAME_OK           0
#define AMXX_GAME_BAD          1

struct amxx_module_info_s {
    const char* name;
    const char* author;
    const char* version;
    int reload;            // reload on mapchange when nonzero
    const char* logtag;    // added in interface version 2
    const char* library;   // added in interface version 4
    const char* libclass;  // added in interface version 4
};

// The core hands this to AMXX_Attach; it maps a name to one of the core's
// functions. "amx_Exec" resolves to the real amx_Exec, which is what makes
// detouring it possible without scanning for a signature.
typedef void* (*PFN_REQ_FNPTR)(const char* name);

// AMXX builds amx.cpp with neither STDECL nor CDECL defined, so AMXAPI is
// empty and these are cdecl on 32-bit MSVC. Getting this wrong corrupts the
// stack on every call, so it is spelled out rather than inherited.
typedef int (*PFN_AMX_EXEC)(struct tagAMX* amx, int32_t* retval, int index);
typedef int (*PFN_FIND_AMXSCRIPT_BYAMX)(const struct tagAMX* amx);
typedef const char* (*PFN_GET_AMXSCRIPTNAME)(int id);

typedef int32_t cell;
typedef uint32_t ucell;

#if defined(__GNUC__)
#define AMXX_PACKED __attribute__((packed))
#else
#define AMXX_PACKED
#endif

#if !defined(__GNUC__)
#pragma pack(push)
#pragma pack(1)
#endif

#define sEXPMAX  19
#define sNAMEMAX 63

typedef struct tagAMX_FUNCSTUB {
    ucell address AMXX_PACKED;
    char name[sEXPMAX + 1];
} AMXX_PACKED AMX_FUNCSTUB;

// Newer plugins keep names in a table at the end of the file instead of
// inline; defsize says which layout is in use.
typedef struct tagFUNCSTUBNT {
    ucell address AMXX_PACKED;
    ucell nameofs AMXX_PACKED;
} AMXX_PACKED AMX_FUNCSTUBNT;

#define AMX_USERNUM 4

typedef struct tagAMX {
    unsigned char* base AMXX_PACKED;  // points at the AMX_HEADER
    unsigned char* data AMXX_PACKED;
    void* callback AMXX_PACKED;
    void* debug AMXX_PACKED;
    cell cip AMXX_PACKED;
    cell frm AMXX_PACKED;
    cell hea AMXX_PACKED;
    cell hlw AMXX_PACKED;
    cell stk AMXX_PACKED;
    cell stp AMXX_PACKED;
    int flags AMXX_PACKED;
    long usertags[AMX_USERNUM] AMXX_PACKED;
    void* userdata[AMX_USERNUM] AMXX_PACKED;
    int error AMXX_PACKED;
    int paramcount;
    cell pri AMXX_PACKED;
    cell alt AMXX_PACKED;
    cell reset_stk AMXX_PACKED;
    cell reset_hea AMXX_PACKED;
    cell sysreq_d AMXX_PACKED;
    int reloc_size AMXX_PACKED;
    long code_size AMXX_PACKED;
} AMXX_PACKED AMX;

typedef struct tagAMX_HEADER {
    int32_t size AMXX_PACKED;
    uint16_t magic AMXX_PACKED;
    char file_version;
    char amx_version;
    int16_t flags AMXX_PACKED;
    int16_t defsize AMXX_PACKED;  // size of one entry in the tables below
    int32_t cod AMXX_PACKED;
    int32_t dat AMXX_PACKED;
    int32_t hea AMXX_PACKED;
    int32_t stp AMXX_PACKED;
    int32_t cip AMXX_PACKED;
    int32_t publics AMXX_PACKED;
    int32_t natives AMXX_PACKED;
    int32_t libraries AMXX_PACKED;
    int32_t pubvars AMXX_PACKED;
    int32_t tags AMXX_PACKED;
    int32_t nametable AMXX_PACKED;
} AMXX_PACKED AMX_HEADER;

#if !defined(__GNUC__)
#pragma pack(pop)
#endif

#define AMX_MAGIC 0xf1e0

// Set while the core is walking the opcode table rather than running a
// plugin. amx_Exec is called with a throwaway AMX in that state and must be
// left alone.
#define AMX_FLAG_BROWSE 0x4000

// The publics table, keyed the same way amx_GetPublic does it.
#define AMXX_USENAMETABLE(hdr) ((hdr)->defsize == sizeof(AMX_FUNCSTUBNT))
#define AMXX_NUMENTRIES(hdr, field, nextfield) \
    (unsigned)(((hdr)->nextfield - (hdr)->field) / (hdr)->defsize)
#define AMXX_GETENTRY(hdr, table, index) \
    (AMX_FUNCSTUB*)((unsigned char*)(hdr) + (unsigned)(hdr)->table + (unsigned)(index) * (hdr)->defsize)
#define AMXX_GETENTRYNAME(hdr, entry)                                                     \
    (AMXX_USENAMETABLE(hdr)                                                               \
            ? (char*)((unsigned char*)(hdr) + (unsigned)((AMX_FUNCSTUBNT*)(entry))->nameofs) \
            : ((AMX_FUNCSTUB*)(entry))->name)
