// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT

#include "ebpf_api.h"
#include "elf.h"
#include "links.h"
#include "maps.h"
#include "pins.h"
#include "processes.h"
#include "programs.h"
#include "resource.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <netsh.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static const GUID g_EbpfHelperGuid = {/* 634d21b8-13f9-46a3-945f-885cbd661c13 */
                                      0x634d21b8,
                                      0x13f9,
                                      0x46a3,
                                      {0x94, 0x5f, 0x88, 0x5c, 0xbd, 0x66, 0x1c, 0x13}};

bool WINAPI
DllMain(HMODULE moduleHandle, unsigned long reasonForCall, void* reserved)
{
    UNREFERENCED_PARAMETER(moduleHandle);
    UNREFERENCED_PARAMETER(reasonForCall);
    UNREFERENCED_PARAMETER(reserved);
    return TRUE;
}

// Verbs
#define CMD_GROUP_ADD L"add"
#define CMD_GROUP_DELETE L"delete"
#define CMD_GROUP_PIN L"pin"
#define CMD_GROUP_SET L"set"
#define CMD_GROUP_SHOW L"show"
#define CMD_GROUP_UNPIN L"unpin"

// Nouns
#define CMD_EBPF_SHOW_DISASSEMBLY L"disassembly"
#define CMD_EBPF_SHOW_LINKS L"links"
#define CMD_EBPF_SHOW_MAPS L"maps"
#define CMD_EBPF_SHOW_PINS L"pins"
#define CMD_EBPF_SHOW_PROCESSES L"processes"

#define CMD_EBPF_ADD_PROGRAM L"program"
#define CMD_EBPF_DELETE_PROGRAM L"program"
#define CMD_EBPF_SET_PROGRAM L"program"
#define CMD_EBPF_SHOW_PROGRAMS L"programs"

#define CMD_EBPF_SHOW_SECTIONS L"sections"
#define CMD_EBPF_SHOW_VERIFICATION L"verification"

#define CMD_EBPF_PIN_MAP L"map"
#define CMD_EBPF_PIN_PROGRAM L"program"
#define CMD_EBPF_UNPIN_MAP L"map"
#define CMD_EBPF_UNPIN_PROGRAM L"program"

// Define this to work around a recent regression introduced in Windows
// until it is fixed.
#define WINDOWS_NETSH_BUG_WORKAROUND 1

#ifndef WINDOWS_NETSH_BUG_WORKAROUND
CMD_ENTRY g_EbpfAddCommandTable[] = {
    CREATE_CMD_ENTRY(EBPF_ADD_PROGRAM, handle_ebpf_add_program),
};
CMD_ENTRY g_EbpfDeleteCommandTable[] = {
    CREATE_CMD_ENTRY(EBPF_DELETE_PROGRAM, handle_ebpf_delete_program),
};
CMD_ENTRY g_EbpfSetCommandTable[] = {
    CREATE_CMD_ENTRY(EBPF_SET_PROGRAM, handle_ebpf_set_program),
};
CMD_ENTRY g_EbpfShowCommandTable[] = {
    CREATE_CMD_ENTRY(EBPF_SHOW_DISASSEMBLY, handle_ebpf_show_disassembly),
    CREATE_CMD_ENTRY(EBPF_SHOW_LINKS, handle_ebpf_show_links),
    CREATE_CMD_ENTRY(EBPF_SHOW_MAPS, handle_ebpf_show_maps),
    CREATE_CMD_ENTRY(EBPF_SHOW_PINS, handle_ebpf_show_pins),
    CREATE_CMD_ENTRY(EBPF_SHOW_PROCESSES, handle_ebpf_show_processes),
    CREATE_CMD_ENTRY(EBPF_SHOW_PROGRAMS, handle_ebpf_show_programs),
    CREATE_CMD_ENTRY(EBPF_SHOW_SECTIONS, handle_ebpf_show_sections),
    CREATE_CMD_ENTRY(EBPF_SHOW_VERIFICATION, handle_ebpf_show_verification),
};
#else
typedef struct _CMD_ENTRY_ORIGINAL
{
    _Field_z_ const wchar_t* pwszCmdToken; // The token for the command
    PFN_HANDLE_CMD pfnCmdHandler;          // The function which handles this command
    unsigned long dwShortCmdHelpToken;     // The short help message
    unsigned long
        dwCmdHlpToken; // The message to display if the only thing after the command is a help token (HELP, /?, -?, ?)
    unsigned long dwFlags;              // Flags (see CMD_FLAGS_xxx above)
    PNS_OSVERSIONCHECK pOsVersionCheck; // Check for the version of the OS this command can run against
} CMD_ENTRY_ORIGINAL, *PCMD_ENTRY_ORIGINAL;
#define CREATE_CMD_ENTRY_ORIGINAL(t, f) {CMD_##t, f, HLP_##t, HLP_##t##_EX, CMD_FLAG_PRIVATE, NULL}

typedef struct _CMD_ENTRY_LONG
{
    _Field_z_ const wchar_t* pwszCmdToken; // The token for the command
    PFN_HANDLE_CMD pfnCmdHandler;          // The function which handles this command
    unsigned long dwShortCmdHelpToken;     // The short help message
    unsigned long
        dwCmdHlpToken; // The message to display if the only thing after the command is a help token (HELP, /?, -?, ?)
    unsigned long dwFlags;              // Flags (see CMD_FLAGS_xxx above)
    PNS_OSVERSIONCHECK pOsVersionCheck; // Check for the version of the OS this command can run against
    void* pfnCustomHelpFn;
} CMD_ENTRY_LONG, *PCMD_ENTRY_LONG;
#define CREATE_CMD_ENTRY_LONG(t, f) {CMD_##t, f, HLP_##t, HLP_##t##_EX, CMD_FLAG_PRIVATE, NULL, NULL}

CMD_ENTRY_ORIGINAL g_EbpfAddCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_ADD_PROGRAM, handle_ebpf_add_program),
};
CMD_ENTRY_LONG g_EbpfAddCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_ADD_PROGRAM, handle_ebpf_add_program),
};
CMD_ENTRY_ORIGINAL g_EbpfDeleteCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_DELETE_PROGRAM, handle_ebpf_delete_program),
};
CMD_ENTRY_LONG g_EbpfDeleteCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_DELETE_PROGRAM, handle_ebpf_delete_program),
};
CMD_ENTRY_ORIGINAL g_EbpfSetCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SET_PROGRAM, handle_ebpf_set_program),
};
CMD_ENTRY_LONG g_EbpfSetCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_SET_PROGRAM, handle_ebpf_set_program),
};
CMD_ENTRY_ORIGINAL g_EbpfShowCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_DISASSEMBLY, handle_ebpf_show_disassembly),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_LINKS, handle_ebpf_show_links),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_MAPS, handle_ebpf_show_maps),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_PINS, handle_ebpf_show_pins),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_PROCESSES, handle_ebpf_show_processes),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_PROGRAMS, handle_ebpf_show_programs),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_SECTIONS, handle_ebpf_show_sections),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_SHOW_VERIFICATION, handle_ebpf_show_verification),
};
CMD_ENTRY_LONG g_EbpfShowCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_DISASSEMBLY, handle_ebpf_show_disassembly),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_LINKS, handle_ebpf_show_links),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_MAPS, handle_ebpf_show_maps),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_PINS, handle_ebpf_show_pins),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_PROCESSES, handle_ebpf_show_processes),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_PROGRAMS, handle_ebpf_show_programs),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_SECTIONS, handle_ebpf_show_sections),
    CREATE_CMD_ENTRY_LONG(EBPF_SHOW_VERIFICATION, handle_ebpf_show_verification),
};

CMD_ENTRY_ORIGINAL g_EbpfPinCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_PIN_MAP, handle_ebpf_pin_map),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_PIN_PROGRAM, handle_ebpf_pin_program),
};
CMD_ENTRY_LONG g_EbpfPinCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_PIN_MAP, handle_ebpf_pin_map),
    CREATE_CMD_ENTRY_LONG(EBPF_PIN_PROGRAM, handle_ebpf_pin_program),
};

CMD_ENTRY_ORIGINAL g_EbpfUnpinCommandTableOriginal[] = {
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_UNPIN_MAP, handle_ebpf_unpin_map),
    CREATE_CMD_ENTRY_ORIGINAL(EBPF_UNPIN_PROGRAM, handle_ebpf_unpin_program),
};
CMD_ENTRY_LONG g_EbpfUnpinCommandTableLong[] = {
    CREATE_CMD_ENTRY_LONG(EBPF_UNPIN_MAP, handle_ebpf_unpin_map),
    CREATE_CMD_ENTRY_LONG(EBPF_UNPIN_PROGRAM, handle_ebpf_unpin_program),
};

#endif // WINDOWS_NETSH_BUG_WORKAROUND

#define HLP_GROUP_ADD 1100
#define HLP_GROUP_ADD_EX 1101
#define HLP_GROUP_DELETE 1102
#define HLP_GROUP_DELETE_EX 1103
#define HLP_GROUP_SET 1104
#define HLP_GROUP_SET_EX 1105
#define HLP_GROUP_SHOW 1106
#define HLP_GROUP_SHOW_EX 1107
#define HLP_GROUP_PIN 1108
#define HLP_GROUP_PIN_EX 1109
#define HLP_GROUP_UNPIN 1110
#define HLP_GROUP_UNPIN_EX 1111

#ifndef WINDOWS_NETSH_BUG_WORKAROUND
static CMD_GROUP_ENTRY g_EbpfGroupCommands[] = {
    CREATE_CMD_GROUP_ENTRY(GROUP_ADD, g_EbpfAddCommandTable),
    CREATE_CMD_GROUP_ENTRY(GROUP_DELETE, g_EbpfDeleteCommandTable),
    CREATE_CMD_GROUP_ENTRY(GROUP_PIN, g_EbpfPinCommandTable),
    CREATE_CMD_GROUP_ENTRY(GROUP_SET, g_EbpfSetCommandTable),
    CREATE_CMD_GROUP_ENTRY(GROUP_SHOW, g_EbpfShowCommandTable),
    CREATE_CMD_GROUP_ENTRY(GROUP_UNPIN, g_EbpfUnpinCommandTable),
};
#else
#define CREATE_CMD_GROUP_ENTRY_ORIGINAL(t, s) \
    {CMD_##t, HLP_##t, sizeof(s) / sizeof(CMD_ENTRY_ORIGINAL), 0, (PCMD_ENTRY)s, NULL}
#define CREATE_CMD_GROUP_ENTRY_LONG(t, s) {CMD_##t, HLP_##t, sizeof(s) / sizeof(CMD_ENTRY_LONG), 0, (PCMD_ENTRY)s, NULL}
static CMD_GROUP_ENTRY g_EbpfGroupCommandsOriginal[] = {
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_ADD, g_EbpfAddCommandTableOriginal),
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_DELETE, g_EbpfDeleteCommandTableOriginal),
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_PIN, g_EbpfPinCommandTableOriginal),
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_SET, g_EbpfSetCommandTableOriginal),
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_SHOW, g_EbpfShowCommandTableOriginal),
    CREATE_CMD_GROUP_ENTRY_ORIGINAL(GROUP_UNPIN, g_EbpfUnpinCommandTableOriginal),
};
static CMD_GROUP_ENTRY g_EbpfGroupCommandsLong[] = {
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_ADD, g_EbpfAddCommandTableLong),
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_DELETE, g_EbpfDeleteCommandTableLong),
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_PIN, g_EbpfPinCommandTableLong),
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_SET, g_EbpfSetCommandTableLong),
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_SHOW, g_EbpfShowCommandTableLong),
    CREATE_CMD_GROUP_ENTRY_LONG(GROUP_UNPIN, g_EbpfUnpinCommandTableLong),
};
#endif // WINDOWS_NETSH_BUG_WORKAROUND

unsigned long WINAPI
EbpfStartHelper(const GUID* parentGuid, unsigned long version)
{
    NS_CONTEXT_ATTRIBUTES attributes = {0};
    UNREFERENCED_PARAMETER(parentGuid);
    UNREFERENCED_PARAMETER(version);

    attributes.pwszContext = L"ebpf";
    attributes.guidHelper = g_EbpfHelperGuid;
    attributes.dwVersion = 1;
    attributes.dwFlags = CMD_FLAG_LOCAL | CMD_FLAG_ONLINE;

#ifndef WINDOWS_NETSH_BUG_WORKAROUND
    attributes.ulNumGroups = _countof(g_EbpfGroupCommands);
    attributes.pCmdGroups = (CMD_GROUP_ENTRY(*)[]) & g_EbpfGroupCommands;
    unsigned long status = RegisterContext(&attributes);
#else
    unsigned long status;
    __try {
        attributes.ulNumGroups = _countof(g_EbpfGroupCommandsOriginal);
        attributes.pCmdGroups = (CMD_GROUP_ENTRY(*)[]) & g_EbpfGroupCommandsOriginal;
        status = RegisterContext(&attributes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_PARAMETER;
    }

    if (status == ERROR_INVALID_PARAMETER) {
        attributes.ulNumGroups = _countof(g_EbpfGroupCommandsLong);
        attributes.pCmdGroups = (CMD_GROUP_ENTRY(*)[]) & g_EbpfGroupCommandsLong;
        status = RegisterContext(&attributes);
    }
#endif // WINDOWS_NETSH_BUG_WORKAROUND

    return status;
}

__declspec(dllexport) unsigned long
InitHelperDll(unsigned long netshVersion, void* reserved)
{
    NS_HELPER_ATTRIBUTES attributes = {0};
    UNREFERENCED_PARAMETER(netshVersion);
    UNREFERENCED_PARAMETER(reserved);
    attributes.guidHelper = g_EbpfHelperGuid;
    attributes.dwVersion = 1;
    attributes.pfnStart = EbpfStartHelper;

    unsigned long status = RegisterHelper(NULL, &attributes);
    if (status != NO_ERROR) {
        // Print the error message here since just returning
        // it would instead print a generic message with the error
        // number instead of the correct netsh specific message.
        PrintError(NULL, status);
        if (status == ERROR_HELPER_ALREADY_REGISTERED) {
            // We must return NO_ERROR or netsh will unregister
            // the instance already registered.  This will, however,
            // cause netsh to add an extra "Ok." after the message,
            // but removing that would require netsh to change.
            status = NO_ERROR;
        } else {
            // We've already shown the message for this error,
            // so tell netsh to skip adding another one.
            status = ERROR_SUPPRESS_OUTPUT;
        }
    }

    return status;
}
