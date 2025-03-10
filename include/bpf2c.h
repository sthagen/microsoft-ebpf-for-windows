// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "ebpf_structs.h"

#if defined(NO_CRT)
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#define bool _Bool
#define false 0
#define true 1
#define UINT32_MAX ((uint32_t)0xFFFFFFFF)

#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define UBPF_STACK_SIZE 512

#define IMMEDIATE(X) (int32_t) X
#define OFFSET(X) (int16_t) X
#define POINTER(X) (uint64_t)(X)

#if !defined(htobe16)
#define htobe16(X) swap16(X)
#define htobe32(X) swap32(X)
#define htobe64(X) swap64(X)

#define htole16(X) (X)
#define htole32(X) (X)
#define htole64(X) (X)
#endif

#if !defined(UNREFERENCED_PARAMETER)
#define UNREFERENCED_PARAMETER(P) (P)
#endif

    typedef uint64_t (*helper_function_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, void*);

    /**
     * @brief Helper function entry.
     * This structure defines a helper function entry in the metadata table. The address of the helper function is
     * written into the entry during load time. The helper_id and name are used to identify the helper function
     * to bind to when the program is loaded.
     */
    typedef struct _helper_function_entry
    {
        uint32_t helper_id;
        const char* name;
    } helper_function_entry_t;

    typedef struct _helper_function_data
    {
        helper_function_t address;
        bool tail_call;
    } helper_function_data_t;

    /**
     * @brief Map entry.
     * This structure contains the address of the map and the map definition. The address is written into the entry
     * during load time. The map definition is used to initialize the map when the program is loaded.
     */
    typedef struct _map_entry
    {
        // DLLs put the strings into the same section, so add a marker
        // at the start of a map entry to make it easy to find
        // entries in the maps section.
        uint64_t zero_marker;

        ebpf_map_definition_in_file_t definition;
        const char* name;
    } map_entry_t;

    typedef struct _map_data
    {
        uintptr_t address;
    } map_data_t;

    /**
     * @brief Map initial values.
     * This structure contains the initial values for a map. The values are used to initialize the map when the
     * program is loaded. Values are specified as strings and are converted to the appropriate type based on the
     * map definition. Only BPF_MAP_TYPE_ARRAY_OF_MAPS and BPF_MAP_TYPE_PROG_ARRAY map types are supported.
     */
    typedef struct _map_initial_values
    {
        const char* name;    // Name of the map.
        size_t count;        // Number of values in the map.
        const char** values; // Array of strings containing the initial values.
    } map_initial_values_t;

    typedef struct _global_variable_section_info
    {
        const char* name;
        size_t size;
        const void* initial_data;
    } global_variable_section_info_t;

    typedef struct _global_variable_section_data
    {
        unsigned char* address_of_map_value;
    } global_variable_section_data_t;

    typedef struct _program_runtime_context
    {
        helper_function_data_t* helper_data;
        map_data_t* map_data;
        global_variable_section_data_t* global_variable_section_data;
    } program_runtime_context_t;

    /**
     * @brief Program entry.
     * This structure contains the address of the program and additional information about the program.
     */
    typedef struct _program_entry
    {
        // DLLs put the strings into the same section, so add a marker
        // at the start of a program entry to make it easy to find
        // entries in the programs section.
        uint64_t zero;

        uint64_t (*function)(void*, const program_runtime_context_t*); ///< Address of the program.
        const char* pe_section_name;              ///< Name of the PE section containing the program.
        const char* section_name;                 ///< Name of the section containing the program.
        const char* program_name;                 ///< Name of the program.
        uint16_t* referenced_map_indices;         ///< List of map indices referenced by the program.
        uint16_t referenced_map_count;            ///< Number of maps referenced by the program.
        helper_function_entry_t* helpers;         ///< List of helper functions used by the program.
        uint16_t helper_count;                    ///< Number of helper functions used by the program.
        size_t bpf_instruction_count;             ///< Number of BPF instructions in the program.
        ebpf_program_type_t* program_type;        ///< Type of the program.
        ebpf_attach_type_t* expected_attach_type; ///< Expected attach type of the program.
        const uint8_t* program_info_hash;         ///< Hash of the program info.
        size_t program_info_hash_length;          ///< Length of the program info hash.
        const char* program_info_hash_type;       ///< Type of the program info hash
    } program_entry_t;

    /**
     * @brief Version information for the bpf2c compiler.
     * This structure contains the version information for the bpf2c compiler that generated the module. It can be
     * used to determine if the module is compatible with the current version of the eBPF for Windows runtime.
     */
    typedef struct _bpf2c_version
    {
        uint32_t major;
        uint32_t minor;
        uint32_t revision;
    } bpf2c_version_t;

    /**
     * @brief Metadata table for a module.
     * This structure is returned by the module's metadata function, get_metadata_table and contains
     * information about the module including the list of programs and maps.
     */
    typedef struct _metadata_table
    {
        size_t size; ///< Size of this structure. Used for versioning.
        void (*programs)(
            _Outptr_result_buffer_maybenull_(*count) program_entry_t** programs,
            _Out_ size_t* count); ///< Returns the list of programs in this module.
        void (*maps)(
            _Outptr_result_buffer_maybenull_(*count) map_entry_t** maps,
            _Out_ size_t* count); ///< Returns the list of maps in this module.
        void (*hash)(
            _Outptr_result_buffer_maybenull_(*size) const uint8_t** hash,
            _Out_ size_t* size); ///< Returns the hash of the ELF file used to generate this module.
        void (*version)(_Out_ bpf2c_version_t* version);
        void (*map_initial_values)(
            _Outptr_result_buffer_maybenull_(*count) map_initial_values_t** map_initial_values,
            _Out_ size_t* count); ///< Returns the list of initial values for maps in this module.
        void (*global_variable_sections)(
            _Outptr_result_buffer_maybenull_(*count) global_variable_section_info_t** global_variable_sections,
            _Out_ size_t* count); ///< Returns the list of global variables in this module.
    } metadata_table_t;

    /**
     * @brief Inline function used to implement the 16 bit EBPF_OP_LE/EBPF_OP_BE instruction.
     *
     * @param[in] value The value to swap.
     * @return The swapped value.
     */
    inline uint16_t
    swap16(uint16_t value)
    {
        return value << 8 | value >> 8;
    }

    /**
     * @brief Inline function used to implement the 32 bit EBPF_OP_LE/EBPF_OP_BE instruction.
     *
     * @param[in] value The value to swap.
     * @return The swapped value.
     */
    inline uint32_t
    swap32(uint32_t value)
    {
        return swap16(value >> 16) | ((uint32_t)swap16(value & ((1 << 16) - 1))) << 16;
    }

    /**
     * @brief Inline function used to implement the 64 bit EBPF_OP_LE/EBPF_OP_BE instruction.
     *
     * @param[in] value The value to swap.
     * @return The swapped value.
     */
    inline uint64_t
    swap64(uint64_t value)
    {
        return swap32(value >> 32) | ((uint64_t)swap32(value & ((1ull << 32ull) - 1))) << 32;
    }

#ifdef __cplusplus
}
#endif
