// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT

#include "bpf/bpf.h"
#pragma warning(push)
#pragma warning(disable : 4200)
#include "bpf/libbpf.h"
#pragma warning(pop)
#include "capture_helper.hpp"
#include "catch_wrapper.hpp"
#include "common_tests.h"
#include "ebpf_platform.h"
#include "ebpf_tracelog.h"
#include "ebpf_vm_isa.hpp"
#include "helpers.h"
#include "platform.h"
#include "program_helper.h"
#include "test_helper.hpp"

#include <chrono>
#include <fstream>
#include <stop_token>
#include <thread>

// Pulling in the prevail namespace to get the definitions in ebpf_vm_isa.h.
// See: https://github.com/vbpf/prevail/issues/876
using namespace prevail;

// libbpf.h uses enum types and generates the
// following warning whenever an enum type is used below:
// "The enum type 'bpf_attach_type' is unscoped.
// Prefer 'enum class' over 'enum'"
#pragma warning(disable : 26812)

#define CONCAT(s1, s2) s1 s2
#define DECLARE_TEST_CASE(_name, _group, _function, _suffix, _execution_type) \
    TEST_CASE(CONCAT(_name, _suffix), _group) { _function(_execution_type); }
#define DECLARE_NATIVE_TEST(_name, _group, _function) \
    DECLARE_TEST_CASE(_name, _group, _function, "-native", EBPF_EXECUTION_NATIVE)
#if !defined(CONFIG_BPF_JIT_DISABLED)
#define DECLARE_JIT_TEST(_name, _group, _function) \
    DECLARE_TEST_CASE(_name, _group, _function, "-jit", EBPF_EXECUTION_JIT)
#else
#define DECLARE_JIT_TEST(_name, _group, _function)
#endif
#if !defined(CONFIG_BPF_INTERPRETER_DISABLED)
#define DECLARE_INTERPRET_TEST(_name, _group, _function) \
    DECLARE_TEST_CASE(_name, _group, _function, "-interpret", EBPF_EXECUTION_INTERPRET)
#else
#define DECLARE_INTERPRET_TEST(_name, _group, _function)
#endif

#define DECLARE_ALL_TEST_CASES(_name, _group, _function) \
    DECLARE_JIT_TEST(_name, _group, _function)           \
    DECLARE_NATIVE_TEST(_name, _group, _function)        \
    DECLARE_INTERPRET_TEST(_name, _group, _function)

#define DECLARE_JIT_TEST_CASES(_name, _group, _function) \
    DECLARE_JIT_TEST(_name, _group, _function)           \
    DECLARE_NATIVE_TEST(_name, _group, _function)

const int nonexistent_fd = 12345678;

// Set of all attach_types defined in ebpfcore. This must be updated any time a new bpf_attach_type is added.
static const std::set<bpf_attach_type> ebpf_core_attach_types = {
    BPF_ATTACH_TYPE_UNSPEC,
    BPF_ATTACH_TYPE_BIND,
    BPF_CGROUP_INET4_CONNECT,
    BPF_CGROUP_INET6_CONNECT,
    BPF_CGROUP_INET4_RECV_ACCEPT,
    BPF_CGROUP_INET6_RECV_ACCEPT,
    BPF_CGROUP_SOCK_OPS,
    BPF_ATTACH_TYPE_SAMPLE,
    BPF_XDP_TEST,
};

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("libbpf load program", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    struct bpf_object* object;
    int program_fd;
#pragma warning(suppress : 4996) // deprecated
    int result = bpf_prog_load_deprecated("test_sample_ebpf.o", BPF_PROG_TYPE_SAMPLE, &object, &program_fd);
    REQUIRE(result == 0);
    REQUIRE(object != nullptr);
    REQUIRE(program_fd != ebpf_fd_invalid);

    bpf_object__close(object);
}
#endif

std::vector<uint8_t>
prepare_udp_packet(uint16_t udp_length, uint16_t ethernet_type);

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("libbpf prog test run", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    struct bpf_object* object;
    int program_fd;
#pragma warning(suppress : 4996) // deprecated
    int result = bpf_prog_load_deprecated("test_sample_ebpf.o", BPF_PROG_TYPE_SAMPLE, &object, &program_fd);
    REQUIRE(result == 0);
    REQUIRE(object != nullptr);
    REQUIRE(program_fd != ebpf_fd_invalid);

    bpf_test_run_opts opts = {};
    sample_program_context_t in_ctx{0};
    sample_program_context_t out_ctx{0};
    opts.repeat = 10;
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = sizeof(in_ctx);
    opts.ctx_out = reinterpret_cast<uint8_t*>(&out_ctx);
    opts.ctx_size_out = sizeof(out_ctx);

    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == 0);

    REQUIRE(opts.duration > 0);

    // Negative tests.

    // Bad fd.
    REQUIRE(bpf_prog_test_run_opts(nonexistent_fd, &opts) == -EINVAL);

    // NULL context.
    opts.ctx_in = nullptr;
    opts.ctx_size_in = 0;
    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == -EINVAL);

    // Zero length context.
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = 0;
    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == -EINVAL);

    // Context out is too small.
    std::vector<uint8_t> small_context(1);
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = sizeof(in_ctx);
    opts.ctx_out = small_context.data();
    opts.ctx_size_out = static_cast<uint32_t>(small_context.size());
    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == -EOTHER);

    // Context in, null context out.
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = sizeof(in_ctx);
    opts.ctx_out = nullptr;
    opts.ctx_size_out = 0;
    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == -EOTHER);

    // No context in, Context out.
    std::vector<uint8_t> context_out(1024);
    opts.ctx_in = nullptr;
    opts.ctx_size_in = 0;
    opts.ctx_out = reinterpret_cast<uint8_t*>(&out_ctx);
    opts.ctx_size_out = sizeof(out_ctx);
    REQUIRE(bpf_prog_test_run_opts(program_fd, &opts) == -EINVAL);
    REQUIRE(opts.ctx_size_out == sizeof(sample_program_context_t));

    // With bpf syscall.
    bpf_attr attr = {};
    attr.test.prog_fd = program_fd;
    attr.test.repeat = 1000;
    attr.test.data_in = reinterpret_cast<uint64_t>(nullptr);
    attr.test.data_out = reinterpret_cast<uint64_t>(nullptr);
    attr.test.data_size_in = 0;
    attr.test.data_size_out = 0;
    attr.test.ctx_in = reinterpret_cast<uint64_t>(&in_ctx);
    attr.test.ctx_size_in = sizeof(in_ctx);
    attr.test.ctx_out = reinterpret_cast<uint64_t>(&out_ctx);
    attr.test.ctx_size_out = sizeof(out_ctx);
    REQUIRE(bpf(BPF_PROG_TEST_RUN, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.test.ctx_size_out == sizeof(sample_program_context_t));
    REQUIRE(attr.test.duration > 0);

    bpf_object__close(object);
}
#endif

TEST_CASE("empty bpf_load_program", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // An empty set of instructions is invalid.
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program(BPF_PROG_TYPE_SAMPLE, nullptr, 0, nullptr, 0, nullptr, 0);
    REQUIRE(program_fd < 0);
    REQUIRE(errno == EINVAL);
}

TEST_CASE("empty bpf_prog_load", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // An empty set of instructions is invalid.
    int program_fd = bpf_prog_load(BPF_PROG_TYPE_SAMPLE, "name", "license", nullptr, 0, nullptr);
    REQUIRE(program_fd < 0);
    REQUIRE(errno == EINVAL);
}

TEST_CASE("too big bpf_prog_load", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // An empty set of instructions is invalid.
    int program_fd = bpf_prog_load(BPF_PROG_TYPE_SAMPLE, "name", "license", nullptr, UINT32_MAX, nullptr);
    REQUIRE(program_fd < 0);
    REQUIRE(errno == EINVAL);
}

TEST_CASE("invalid bpf_load_program", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with an invalid set of instructions.
    prevail::EbpfInst instructions[] = {
        {INST_OP_EXIT}, // return r0
    };

    // Try to load and verify the eBPF program.
    char log_buffer[1024];
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program(
        BPF_PROG_TYPE_SAMPLE,
        (struct bpf_insn*)instructions,
        _countof(instructions),
        nullptr,
        0,
        log_buffer,
        sizeof(log_buffer));
    REQUIRE(program_fd < 0);
#if !defined(CONFIG_BPF_JIT_DISABLED)
    REQUIRE(errno == EACCES);
    REQUIRE(strcmp(log_buffer, "0: Invalid type (r0.type == number)\n\n") == 0);
#else
    REQUIRE(errno == ENOTSUP);
#endif
}

TEST_CASE("invalid bpf_prog_load", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with an invalid set of instructions.
    prevail::EbpfInst instructions[] = {
        {INST_OP_EXIT}, // return r0
    };

    // Try to load and verify the eBPF program.
    char log_buffer[1024] = "";
    struct bpf_prog_load_opts opts = {.sz = sizeof(opts), .log_size = sizeof(log_buffer), .log_buf = log_buffer};
    int program_fd = bpf_prog_load(
        BPF_PROG_TYPE_SAMPLE, "name", "license", (struct bpf_insn*)instructions, _countof(instructions), &opts);
    REQUIRE(program_fd < 0);
#if !defined(CONFIG_BPF_JIT_DISABLED)
    REQUIRE(errno == EACCES);
    REQUIRE(strcmp(log_buffer, "0: Invalid type (r0.type == number)\n\n") == 0);
#else
    REQUIRE(errno == ENOTSUP);
#endif
}

TEST_CASE("invalid bpf_load_program - wrong type", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program(
        (bpf_prog_type)-1, (struct bpf_insn*)instructions, _countof(instructions), nullptr, 0, nullptr, 0);
    REQUIRE(program_fd < 0);
    REQUIRE(errno == EINVAL);
}

TEST_CASE("invalid bpf_prog_load - wrong type", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
    int program_fd = bpf_prog_load(
        (bpf_prog_type)-1, "name", "license", (struct bpf_insn*)instructions, _countof(instructions), nullptr);
    REQUIRE(program_fd < 0);
    REQUIRE(errno == EINVAL);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("valid bpf_load_program", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program(
        BPF_PROG_TYPE_SAMPLE, (struct bpf_insn*)instructions, _countof(instructions), nullptr, 0, nullptr, 0);
    REQUIRE(program_fd >= 0);

    // Now query the program info and verify it matches what we set.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(program_info.nr_map_ids == 0);
    REQUIRE(program_info.map_ids == 0);

    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);

    Platform::_close(program_fd);
}

TEST_CASE("valid bpf_prog_load", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
    int program_fd = bpf_prog_load(
        BPF_PROG_TYPE_SAMPLE, "name", nullptr, (struct bpf_insn*)instructions, _countof(instructions), nullptr);
    REQUIRE(program_fd >= 0);

    // Now query the program info and verify it matches what we set.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(program_info.nr_map_ids == 0);
    REQUIRE(program_info.map_ids == 0);
    REQUIRE(strcmp(program_info.name, "name") == 0);

    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);

    Platform::_close(program_fd);
}

TEST_CASE("valid bpf_load_program_xattr", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
    struct bpf_load_program_attr attr = {
        .prog_type = BPF_PROG_TYPE_SAMPLE,
        .name = "name",
        .insns = (struct bpf_insn*)instructions,
        .insns_cnt = _countof(instructions)};
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program_xattr(&attr, nullptr, 0);
    REQUIRE(program_fd >= 0);

    // Now query the program info and verify it matches what we set.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(program_info.nr_map_ids == 0);
    REQUIRE(program_info.map_ids == 0);
    REQUIRE(strcmp(program_info.name, "name") == 0);

    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);

    Platform::_close(program_fd);
}
#endif

// Define macros that appear in the Linux man page to values in ebpf_vm_isa.h.
#define BPF_LD_MAP_FD(reg, fd) \
    {INST_OP_LDDW_IMM, (reg), 1, 0, (fd)}, { 0 }
#define BPF_ALU64_IMM(op, reg, imm) {INST_CLS_ALU64 | INST_SRC_IMM | ((op) << 4), (reg), 0, 0, (imm)}
#define BPF_MOV64_IMM(reg, imm) {INST_CLS_ALU64 | INST_SRC_IMM | 0xb0, (reg), 0, 0, (imm)}
#define BPF_MOV64_REG(dst, src) {INST_CLS_ALU64 | INST_SRC_REG | 0xb0, (dst), (src), 0, 0}
#define BPF_EXIT_INSN() {INST_OP_EXIT}
#define BPF_CALL_FUNC(imm) {INST_OP_CALL, 0, 0, 0, (imm)}
#define BPF_STX_MEM(sz, dst, src, off) {INST_CLS_STX | INST_MODE_MEM | (sz), (dst), (src), (off), 0}
#define BPF_W INST_SIZE_W
#define BPF_REG_1 R1_ARG
#define BPF_REG_2 R2_ARG
#define BPF_REG_3 R3_ARG
#define BPF_REG_4 R4_ARG
#define BPF_REG_10 R10_STACK_POINTER
#define BPF_ADD 0

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("valid bpf_load_program with map", "[libbpf][deprecated]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    int map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, nullptr, sizeof(uint32_t), sizeof(uint32_t), 2, nullptr);
    REQUIRE(map_fd >= 0);

    // Try with a valid set of instructions.
    prevail::EbpfInst instructions[] = {
        BPF_MOV64_IMM(BPF_REG_1, 0),                   // r1 = 0
        BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_1, -4), // *(u32 *)(r10 - 4) = r1
        BPF_MOV64_IMM(BPF_REG_1, 42),                  // r1 = 42
        BPF_STX_MEM(BPF_W, BPF_REG_10, BPF_REG_1, -8), // *(u32 *)(r10 - 8) = r1
        BPF_MOV64_REG(BPF_REG_2, BPF_REG_10),          // r2 = r10
        BPF_ALU64_IMM(BPF_ADD, BPF_REG_2, -4),         // r2 += -4
        BPF_MOV64_REG(BPF_REG_3, BPF_REG_10),          // r3 = r10
        BPF_ALU64_IMM(BPF_ADD, BPF_REG_3, -8),         // r3 += -8
        BPF_LD_MAP_FD(BPF_REG_1, map_fd),              // r1 = map_fd ll
        BPF_MOV64_IMM(BPF_REG_4, 0),                   // r4 = 0
        BPF_CALL_FUNC(BPF_FUNC_map_update_elem),       // call map_lookup_elem
        BPF_EXIT_INSN(),                               // return r0
    };

    // Load and verify the eBPF program.
#pragma warning(suppress : 4996) // deprecated
    int program_fd = bpf_load_program(
        BPF_PROG_TYPE_SAMPLE, (struct bpf_insn*)instructions, _countof(instructions), nullptr, 0, nullptr, 0);
    REQUIRE(program_fd >= 0);

    // Now query the program info and verify it matches what we set.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(program_info.nr_map_ids == 1);
    REQUIRE(program_info.map_ids == 0);

    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);

    Platform::_close(program_fd);
    Platform::_close(map_fd);
}

TEST_CASE("libbpf program", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);
    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    const char* name = bpf_object__name(object);
    REQUIRE(strcmp(name, "test_sample_ebpf.o") == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "test_program_entry");
    REQUIRE(program != nullptr);

    errno = 0;
    REQUIRE(bpf_object__find_program_by_name(object, "not_a_valid_name") == NULL);
    REQUIRE(errno == ENOENT);

    // Testing invalid map name.
    errno = 0;
    REQUIRE(bpf_object__find_map_by_name(object, "not_a_valid_map") == NULL);
    REQUIRE(errno == ENOENT);

    name = bpf_program__section_name(program);
    REQUIRE(strcmp(name, "sample_ext") == 0);

    name = bpf_program__name(program);
    REQUIRE(strcmp(name, "test_program_entry") == 0);

    int fd2 = bpf_program__fd(program);
    REQUIRE(fd2 != ebpf_fd_invalid);

    size_t size = bpf_program__insn_cnt(program);
    REQUIRE(size == 40);

#pragma warning(suppress : 4996) // deprecated
    size = bpf_program__size(program);
    REQUIRE(size == 320);

    REQUIRE(bpf_object__next_program(object, program) == nullptr);
    REQUIRE(bpf_object__prev_program(object, program) == nullptr);
    REQUIRE(bpf_object__next_program(object, nullptr) == program);
    REQUIRE(bpf_object__prev_program(object, nullptr) == program);

    bpf_object__close(object);
}

TEST_CASE("libbpf subprogram", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("bindmonitor_bpf2bpf.o");
    REQUIRE(object != nullptr);

    // Test bpf_object__find_program_by_name().
    struct bpf_program* program = bpf_object__find_program_by_name(object, "BindMonitor_Callee");
    REQUIRE(program == nullptr);
    program = bpf_object__find_program_by_name(object, "BindMonitor_Caller");
    REQUIRE(program != nullptr);

    // Test bpf_object__next_program().
    REQUIRE(bpf_object__next_program(object, program) == nullptr);
    REQUIRE(bpf_object__next_program(object, nullptr) == program);

    // Test bpf_object__next_program().
    REQUIRE(bpf_object__prev_program(object, program) == nullptr);
    REQUIRE(bpf_object__prev_program(object, nullptr) == program);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    bpf_object__close(object);
}

static void
_test_program_autoload(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name =
        (execution_type == EBPF_EXECUTION_NATIVE ? "tail_call_same_section_um.dll" : "tail_call_same_section.o");
    struct bpf_object* object = bpf_object__open(file_name);
    REQUIRE(object != nullptr);
    struct bpf_program* caller = bpf_object__find_program_by_name(object, "caller");
    REQUIRE(caller != nullptr);
    int caller_fd = bpf_program__fd(const_cast<const bpf_program*>(caller));
    REQUIRE(caller_fd == ebpf_fd_invalid);
    struct bpf_program* callee = bpf_object__find_program_by_name(object, "callee");
    REQUIRE(callee != nullptr);
    int callee_fd = bpf_program__fd(const_cast<const bpf_program*>(callee));
    REQUIRE(callee_fd == ebpf_fd_invalid);

    // Check initial autoload values.
    REQUIRE(bpf_program__autoload(caller) == true);
    REQUIRE(bpf_program__autoload(callee) == true);

    // Update an autoload value.
    REQUIRE(bpf_program__set_autoload(caller, false) == 0);
    REQUIRE(bpf_program__autoload(caller) == false);
    REQUIRE(bpf_program__autoload(callee) == true);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    // Verify what programs were loaded.
    caller_fd = bpf_program__fd(const_cast<const bpf_program*>(caller));
    REQUIRE(caller_fd == ebpf_fd_invalid);
    callee_fd = bpf_program__fd(const_cast<const bpf_program*>(callee));
    REQUIRE(callee_fd > 0);

    // Verify we cannot change autoload values after loading.
    int error = bpf_program__set_autoload(caller, false);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    error = bpf_program__set_autoload(caller, true);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    error = bpf_program__set_autoload(callee, false);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    error = bpf_program__set_autoload(callee, true);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);

    bpf_object__close(object);
}

DECLARE_ALL_TEST_CASES("libbpf program autoload", "[libbpf]", _test_program_autoload);

TEST_CASE("libbpf program pinning", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    const char* pin_path = "\\temp\\test";
    const char* bad_pin_path = "\\bad\\path";

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);
    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "test_program_entry");
    REQUIRE(program != nullptr);

    // Try to pin the program.
    int result = bpf_program__pin(program, pin_path);
    REQUIRE(result == 0);

    // Make sure a duplicate pin fails.
    result = bpf_program__pin(program, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == EEXIST);

    // Test bpf_obj_get() to return the fd and correctly set 'errno'.
    fd_t obj_fd = bpf_obj_get(pin_path);
    REQUIRE(obj_fd != ebpf_fd_invalid);
    REQUIRE(errno == EEXIST);
    obj_fd = bpf_obj_get(bad_pin_path);
    REQUIRE(obj_fd == -ENOENT);
    REQUIRE(errno == ENOENT);

    result = bpf_program__unpin(program, pin_path);
    REQUIRE(result == 0);

    // Make sure a duplicate unpin fails.
    result = bpf_program__unpin(program, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOENT);

    // Try to pin all (1) programs in the object.
    result = bpf_object__pin_programs(object, pin_path);
    REQUIRE(result == 0);

    // Make sure a duplicate pin fails.
    REQUIRE(bpf_object__pin_programs(object, pin_path) < 0);
    REQUIRE(errno == EEXIST);

    result = bpf_object__unpin_programs(object, pin_path);
    REQUIRE(result == 0);

    // Try to pin all programs and maps in the object.
    result = bpf_object__pin(object, pin_path);
    REQUIRE(result == 0);

    // Make sure a duplicate pin fails.
    REQUIRE(bpf_object__pin_programs(object, pin_path) < 0);
    REQUIRE(errno == EEXIST);

    // There is no bpf_object__unpin API, so
    // we have to unpin programs and maps separately.
    result = bpf_object__unpin_programs(object, pin_path);
    REQUIRE(result == 0);

    result = bpf_object__unpin_maps(object, pin_path);
    REQUIRE(result == 0);

    bpf_object__close(object);
}

TEST_CASE("libbpf program attach", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "test_program_entry");
    REQUIRE(program != nullptr);

    // Based on the program type, verify that the/ default attach type is set correctly.
    enum bpf_attach_type type = bpf_program__get_expected_attach_type(program);
    REQUIRE(type == BPF_ATTACH_TYPE_SAMPLE);

    REQUIRE(bpf_program__set_expected_attach_type(program, BPF_ATTACH_TYPE_SAMPLE) == 0);

    type = bpf_program__get_expected_attach_type(program);
    REQUIRE(type == BPF_ATTACH_TYPE_SAMPLE);

    int result = bpf_object__load(object);
    REQUIRE(result == 0);

    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    int link_fd = bpf_link__fd(link.get());
    REQUIRE(link_fd >= 0);

    result = bpf_link_detach(link_fd);
    REQUIRE(result == 0);

    // Second detach is idempotent.
    result = bpf_link_detach(link_fd);
    REQUIRE(result == 0);

    result = bpf_link_detach(ebpf_handle_invalid);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);

    bpf_object__close(object);
}
#endif

#define TEST_IFINDEX 17
// This is a set of tests which utilize the libbpf XDP APIs.
// The XDP extension is built outside of the ebpf-for-windows repo
// and therefore these APIs are expected to gracefully fail.
TEST_CASE("libbpf xdp negative", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    uint32_t program_id;
    REQUIRE(bpf_xdp_query_id(TEST_IFINDEX, 0, &program_id) < 0);

    REQUIRE(bpf_xdp_attach(TEST_IFINDEX, 0, 0, nullptr) < 0);

    REQUIRE(bpf_xdp_detach(TEST_IFINDEX, 0, nullptr) == 0);

#pragma warning(suppress : 4996) // deprecated
    REQUIRE(bpf_set_link_xdp_fd(TEST_IFINDEX, 0, 0) < 0);

#pragma warning(suppress : 4996) // deprecated
    REQUIRE(bpf_program__attach_xdp(nullptr, TEST_IFINDEX) == nullptr);
}

void
test_xdp_ifindex(uint32_t ifindex, int program_fd[2], bpf_prog_info program_info[2])
{
    // Verify there's no program attached to the specified ifindex.
    uint32_t program_id;
    REQUIRE(bpf_xdp_query_id(ifindex, 0, &program_id) < 0);
    REQUIRE(errno == ENOENT);

    // Attach the first program to the specified ifindex.
    REQUIRE(bpf_xdp_attach(ifindex, program_fd[0], 0, nullptr) == 0);
    REQUIRE(bpf_xdp_query_id(ifindex, 0, &program_id) == 0);
    REQUIRE(program_id == program_info[0].id);

    // Replace it with the second program.
    REQUIRE(bpf_xdp_attach(ifindex, program_fd[1], XDP_FLAGS_REPLACE, nullptr) == 0);
    REQUIRE(bpf_xdp_query_id(ifindex, 0, &program_id) == 0);
    REQUIRE(program_id == program_info[1].id);

    // Detach the second program.
    REQUIRE(bpf_xdp_detach(ifindex, XDP_FLAGS_REPLACE, nullptr) == 0);

    // Verify there's no program attached to this ifindex.
    REQUIRE(bpf_xdp_query_id(ifindex, 0, &program_id) < 0);
    REQUIRE(errno == ENOENT);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("bpf_set_link_xdp_fd", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object[2];
    struct bpf_program* program[2];
    int program_fd[2];
    bpf_prog_info program_info[2] = {};

    for (int i = 0; i < 2; i++) {
        object[i] = bpf_object__open("droppacket.o");
        REQUIRE(object[i] != nullptr);
        // Load the program(s).
        REQUIRE(bpf_object__load(object[i]) == 0);

        program[i] = bpf_object__find_program_by_name(object[i], "DropPacket");
        REQUIRE(program[i] != nullptr);
        program_fd[i] = bpf_program__fd(const_cast<const bpf_program*>(program[i]));

        uint32_t program_info_size = sizeof(program_info[i]);
        REQUIRE(bpf_obj_get_info_by_fd(program_fd[i], &program_info[i], &program_info_size) == 0);
    }

    test_xdp_ifindex(TEST_IFINDEX, program_fd, program_info);
    test_xdp_ifindex(0, program_fd, program_info);

    bpf_object__close(object[0]);
    bpf_object__close(object[1]);
}

TEST_CASE("libbpf map", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    std::vector<std::string> expected_map_names = {
        "HASH_map",
        "PERCPU_HASH_map",
        "ARRAY_map",
        "PERCPU_ARRAY_map",
        "LRU_HASH_map",
        "LRU_PERCPU_HASH_map",
        "QUEUE_map",
        "STACK_map"};
    std::vector<bpf_map_type> expected_map_types = {
        BPF_MAP_TYPE_HASH,
        BPF_MAP_TYPE_PERCPU_HASH,
        BPF_MAP_TYPE_ARRAY,
        BPF_MAP_TYPE_PERCPU_ARRAY,
        BPF_MAP_TYPE_LRU_HASH,
        BPF_MAP_TYPE_LRU_PERCPU_HASH,
        BPF_MAP_TYPE_QUEUE,
        BPF_MAP_TYPE_STACK};
    std::vector<struct bpf_map*> maps;

    struct bpf_object* object = bpf_object__open("map.o");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "test_maps");
    REQUIRE(program != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    // Find all maps.
    struct bpf_map* map = nullptr;
    bpf_object__for_each_map(map, object) { maps.push_back(map); };

    REQUIRE(maps.size() == expected_map_names.size());

    // Get the first map.
    map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);

    // Verify that there are no maps before this.
    REQUIRE(bpf_object__prev_map(object, map) == nullptr);

    // Get the next map.
    struct bpf_map* map2 = bpf_object__next_map(object, map);
    REQUIRE(map2 != nullptr);
    REQUIRE(bpf_object__prev_map(object, map2) == map);

    // Verify that there are no other maps after the last map.
    REQUIRE(bpf_object__next_map(object, maps.back()) == nullptr);

    // Verify the map names, types, key size, value size, max entries, and flags.
    for (size_t i = 0; i < maps.size(); i++) {
        REQUIRE(bpf_map__name(maps[i]) == expected_map_names[i]);
        REQUIRE(bpf_map__type(maps[i]) == expected_map_types[i]);

        if (expected_map_types[i] == BPF_MAP_TYPE_QUEUE || expected_map_types[i] == BPF_MAP_TYPE_STACK) {
            REQUIRE(bpf_map__key_size(maps[i]) == 0);
        } else {
            REQUIRE(bpf_map__key_size(maps[i]) == 4);
        }

        REQUIRE(bpf_map__value_size(maps[i]) == 4);
        REQUIRE(bpf_map__max_entries(maps[i]) == 10);
    }

    int map_fd = bpf_map__fd(maps[2]);
    REQUIRE(map_fd > 0);

    uint64_t value = 0;
    uint32_t index = bpf_map__max_entries(maps[2]) + 10; // Past end of array.

    int result = bpf_map_lookup_elem(map_fd, &index, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOENT);

    // Wrong fd type.
    int program_fd = bpf_program__fd(const_cast<const bpf_program*>(program));
    result = bpf_map_lookup_elem(program_fd, &index, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid fd.
    result = bpf_map_lookup_elem(nonexistent_fd, &index, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    result = bpf_map_lookup_elem(-1, &index, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    // NULL key.
    result = bpf_map_lookup_elem(map_fd, NULL, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid key.
    result = bpf_map_delete_elem(map_fd, &index);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Wrong fd type.
    result = bpf_map_delete_elem(program_fd, &index);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid fd.
    result = bpf_map_delete_elem(nonexistent_fd, &index);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    // NULL key.
    result = bpf_map_update_elem(map_fd, NULL, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid key.
    result = bpf_map_update_elem(map_fd, &index, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    index = 0;
    // Invalid fd.
    result = bpf_map_update_elem(nonexistent_fd, &index, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    result = bpf_map_update_elem(-1, &index, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    // Wrong fd type.
    result = bpf_map_update_elem(program_fd, &index, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &value) == 0);
    REQUIRE(value == 0);

    REQUIRE(bpf_map_delete_elem(map_fd, &index) == 0);

    value = 12345;
    REQUIRE(bpf_map_update_elem(map_fd, &index, &value, 0) == 0);

    // Wrong flags.
    result = bpf_map_update_elem(map_fd, &index, &value, UINT64_MAX);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    value = 0;
    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &value) == 0);
    REQUIRE(value == 12345);

    REQUIRE(bpf_map_delete_elem(map_fd, &index) == 0);

    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &value) == 0);
    REQUIRE(value == 0);

    // Query value from per-CPUs maps.
    for (size_t i = 0; i < expected_map_names.size(); i++) {
        if (expected_map_names[i].find("PERCPU") == std::string::npos) {
            continue;
        };
        std::vector<uint8_t> per_cpu_value(
            EBPF_PAD_8(bpf_map__value_size(maps[i])) * static_cast<size_t>(libbpf_num_possible_cpus()));
        REQUIRE(bpf_map_update_elem(bpf_map__fd(maps[i]), &index, per_cpu_value.data(), 0) == 0);
        REQUIRE(bpf_map_lookup_elem(bpf_map__fd(maps[i]), &index, per_cpu_value.data()) == 0);
    }

    // Invalid map type.
    result = bpf_map_create(BPF_MAP_TYPE_UNSPEC, "BPF_MAP_TYPE_UNSPEC", 1, 1, 1, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOTSUP);

    // Invalid key size.
    result = bpf_map_create(BPF_MAP_TYPE_HASH, "no_key", 0, 1, 1, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid value size.
    result = bpf_map_create(BPF_MAP_TYPE_HASH, "no_value", 1, 0, 1, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid entry count.
    result = bpf_map_create(BPF_MAP_TYPE_HASH, "no_entries", 1, 1, 0, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid options - bad inner_map_fd.
    bpf_map_create_opts opts{
        sizeof(opts),         // sz
        0,                    // btf_fd
        0,                    // btf_key_type_id
        0,                    // btf_value_type_id
        0,                    // btf_vmlinux_value_type_id
        (uint32_t)program_fd, // inner_map_fd
        0,                    // map_flags
        0,                    // map_extra
        0,                    // numa_node
        0,                    // map_ifindex
    };
    result = bpf_map_create(BPF_MAP_TYPE_HASH_OF_MAPS, "bad_opts", 1, 1, 1, &opts);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid options - bad flags.
    opts = {
        sizeof(opts), // sz
        0,            // btf_fd
        0,            // btf_key_type_id
        0,            // btf_value_type_id
        0,            // btf_vmlinux_value_type_id
        0,            // inner_map_fd
        UINT32_MAX,   // map_flags
        0,            // map_extra
        0,            // numa_node
        0,            // map_ifindex
    };
    result = bpf_map_create(BPF_MAP_TYPE_HASH_OF_MAPS, "bad_opts", 1, 1, 1, &opts);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Invalid fd.
    result = bpf_map_get_next_key(nonexistent_fd, NULL, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    // FD not a map.
    result = bpf_map_get_next_key(program_fd, NULL, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // next_key is NULL.
    result = bpf_map_get_next_key(map_fd, NULL, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    bpf_object__close(object);
}
#endif

TEST_CASE("libbpf create queue", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    bpf_map_create_opts opts = {0};
    const uint32_t max_entries = 2;
    const uint32_t value_size = sizeof(uint32_t);
    int map_fd = bpf_map_create(BPF_MAP_TYPE_QUEUE, "MapName", sizeof(uint32_t), value_size, max_entries, &opts);
    REQUIRE(map_fd < 0);

    map_fd = bpf_map_create(BPF_MAP_TYPE_QUEUE, "MapName", 0, value_size, max_entries, &opts);
    REQUIRE(map_fd > 0);

    bpf_map_info info;
    uint32_t info_size = sizeof(info);
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &info, &info_size) == 0);

    REQUIRE(info.type == BPF_MAP_TYPE_QUEUE);
    REQUIRE(info.key_size == 0);
    REQUIRE(info.value_size == value_size);
    REQUIRE(info.max_entries == max_entries);
    REQUIRE(info.map_flags == 0);
    REQUIRE(info.inner_map_id == EBPF_ID_NONE);
    REQUIRE(info.pinned_path_count == 0);
    REQUIRE(info.id > 0);
    REQUIRE(strcmp(info.name, "MapName") == 0);

    uint32_t next_key;
    int err = bpf_map_get_next_key(map_fd, NULL, &next_key);
    REQUIRE(err == -ENOTSUP);

    // Push 2 elements.
    uint32_t value = 1;
    REQUIRE(bpf_map_update_elem(map_fd, nullptr, &value, 0) == 0);
    value = 2;
    REQUIRE(bpf_map_update_elem(map_fd, nullptr, &value, 0) == 0);

    // Pop elements.
    REQUIRE(bpf_map_lookup_elem(map_fd, nullptr, &value) == 0);
    REQUIRE(value == 1);
    REQUIRE(bpf_map_lookup_and_delete_elem(map_fd, nullptr, &value) == 0);
    REQUIRE(value == 1);
    REQUIRE(bpf_map_lookup_elem(map_fd, nullptr, &value) == 0);
    REQUIRE(value == 2);
    REQUIRE(bpf_map_lookup_and_delete_elem(map_fd, nullptr, &value) == 0);
    REQUIRE(value == 2);
    REQUIRE(bpf_map_lookup_elem(map_fd, nullptr, &value) == -ENOENT);
    REQUIRE(bpf_map_lookup_and_delete_elem(map_fd, nullptr, &value) == -ENOENT);

    Platform::_close(map_fd);
}

TEST_CASE("libbpf create ringbuf", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    bpf_map_create_opts opts = {0};
    const uint32_t max_entries = 128 * 1024;
    const uint32_t value_size = sizeof(uint32_t);

    // Wrong key and value size.
    int map_fd = bpf_map_create(BPF_MAP_TYPE_RINGBUF, "MapName", sizeof(uint32_t), value_size, max_entries, &opts);
    REQUIRE(map_fd < 0);

    // Max_entries too small.
    map_fd = bpf_map_create(BPF_MAP_TYPE_RINGBUF, "MapName", 0, 0, 1024, &opts);
    REQUIRE(map_fd < 0);

    map_fd = bpf_map_create(BPF_MAP_TYPE_RINGBUF, "MapName", 0, 0, max_entries, &opts);
    REQUIRE(map_fd > 0);

    bpf_map_info info;
    uint32_t info_size = sizeof(info);
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &info, &info_size) == 0);

    REQUIRE(info.type == BPF_MAP_TYPE_RINGBUF);
    REQUIRE(info.key_size == 0);
    REQUIRE(info.value_size == 0);
    REQUIRE(info.max_entries == max_entries);
    REQUIRE(info.map_flags == 0);
    REQUIRE(info.inner_map_id == EBPF_ID_NONE);
    REQUIRE(info.pinned_path_count == 0);
    REQUIRE(info.id > 0);
    REQUIRE(strcmp(info.name, "MapName") == 0);

    int key;
    int value;
    int result = bpf_map_lookup_elem(map_fd, &key, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);
    result = bpf_map_update_elem(map_fd, &key, &value, 0);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);
    result = bpf_map_delete_elem(map_fd, &key);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    result = bpf_map_get_next_key(map_fd, &key, &value);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOTSUP);

    Platform::_close(map_fd);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("libbpf map binding", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_program* program = bpf_object__next_program(object, nullptr);
    REQUIRE(program != nullptr);
    int program_fd = bpf_program__fd(const_cast<const bpf_program*>(program));

    // Create a map.
    int map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, nullptr, sizeof(uint32_t), sizeof(uint32_t), 2, nullptr);
    REQUIRE(map_fd > 0);
    bpf_map_info info;
    uint32_t info_size = sizeof(info);
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &info, &info_size) == 0);
    ebpf_id_t map_id = info.id;

    // Try some invalid FDs.
    int error = bpf_prog_bind_map(ebpf_fd_invalid, map_fd, nullptr);
    REQUIRE(error < 0);
    REQUIRE(errno == EBADF);

    error = bpf_prog_bind_map(map_fd, map_fd, nullptr);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);

    error = bpf_prog_bind_map(program_fd, ebpf_fd_invalid, nullptr);
    REQUIRE(error < 0);
    REQUIRE(errno == EBADF);

    error = bpf_prog_bind_map(program_fd, program_fd, nullptr);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);

    // Bind it to the program.
    error = bpf_prog_bind_map(program_fd, map_fd, nullptr);
    REQUIRE(error == 0);

    // Release our own reference on the map.
    Platform::_close(map_fd);

    // Verify that the map still exists.
    map_fd = bpf_map_get_fd_by_id(map_id);
    REQUIRE(map_fd > 0);
    Platform::_close(map_fd);

    // Close the object, which should cause the map to be deleted.
    bpf_object__close(object);
    REQUIRE(bpf_map_get_fd_by_id(map_id) < 0);
}

TEST_CASE("libbpf map pinning", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    const char* pin_path = "\\temp\\test";

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);

    REQUIRE(bpf_map__is_pinned(map) == false);

    // Try to pin the map.
    int result = bpf_map__pin(map, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == true);

    // Make sure a duplicate pin fails.
    result = bpf_map__pin(map, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == EEXIST);

    result = bpf_map__unpin(map, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == false);

    // Make sure pinning with a different name fails.
    result = bpf_map__pin(map, "second_pin_path");
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    // Make sure an invalid path fails.
    result = bpf_map__unpin(map, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOENT);

    // Make sure a duplicate unpin fails.
    result = bpf_map__unpin(map, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == ENOENT);

    // Clear pin path for the map.
    result = bpf_map__set_pin_path(map, nullptr);
    REQUIRE(result == 0);

    // Set pin path for the map.
    result = bpf_map__set_pin_path(map, pin_path);
    REQUIRE(result == 0);

    // Clear pin path for the map.
    result = bpf_map__set_pin_path(map, nullptr);
    REQUIRE(result == 0);

    // Try to pin all (1) maps in the object.
    result = bpf_object__pin_maps(object, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == true);

    // Make sure a duplicate pin fails.
    result = bpf_object__pin_maps(object, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == EEXIST);

    result = bpf_object__unpin_maps(object, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == false);

    // Try to pin all programs and maps in the object.
    result = bpf_object__pin(object, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == true);

    // Make sure a duplicate pin fails.
    result = bpf_object__pin_maps(object, pin_path);
    REQUIRE(result < 0);
    REQUIRE(errno == EEXIST);

    // There is no bpf_object__unpin API, so
    // we have to unpin programs and maps separately.
    result = bpf_object__unpin_programs(object, pin_path);
    REQUIRE(result == 0);

    result = bpf_object__unpin_maps(object, pin_path);
    REQUIRE(result == 0);

    REQUIRE(bpf_map__is_pinned(map) == false);

    bpf_object__close(object);
}

TEST_CASE("libbpf obj pinning", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    const char* pin_path = "\\temp\\test";

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);

    int map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    int result = bpf_obj_pin(map_fd, pin_path);
    REQUIRE(result == 0);

    // Linux lacks a bpf_object_unpin, so call the ebpf_ variety.
    REQUIRE(ebpf_object_unpin(pin_path) == EBPF_SUCCESS);

    result = bpf_obj_pin(-1, "invalid_fd");
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    result = bpf_obj_pin(map_fd, NULL);
    REQUIRE(result < 0);
    REQUIRE(errno == EINVAL);

    result = bpf_obj_pin(nonexistent_fd, "not_a_real_fd");
    REQUIRE(result < 0);
    REQUIRE(errno == EBADF);

    bpf_object__close(object);
}
#endif

static void
_ebpf_test_tail_call(_In_z_ const char* filename, uint32_t expected_result)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    struct bpf_object* object = bpf_object__open(filename);
    REQUIRE(object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_program* caller = bpf_object__find_program_by_name(object, "caller");
    REQUIRE(caller != nullptr);

    struct bpf_program* callee = bpf_object__find_program_by_name(object, "callee");
    REQUIRE(callee != nullptr);

    struct bpf_map* map = bpf_object__find_map_by_name(object, "map");
    REQUIRE(map != nullptr);
    struct bpf_map* canary_map = bpf_object__find_map_by_name(object, "canary");
    REQUIRE(canary_map != nullptr);

    int callee_fd = bpf_program__fd(callee);
    REQUIRE(callee_fd >= 0);

    int map_fd = bpf_map__fd(map);
    REQUIRE(map_fd >= 0);

    int canary_map_fd = bpf_map__fd(canary_map);
    REQUIRE(canary_map_fd >= 0);

    // First do some negative tests.
    int index = 10;
    int error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&callee_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == -error);
    index = 0;
    int bad_fd = 0;
    error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&bad_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == -error);

    // Finally store the correct program fd.
    index = 9;
    error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&callee_fd, 0);
    REQUIRE(error == 0);

    // Verify that we can read it back.
    ebpf_id_t callee_id;
    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &callee_id) == 0);

    // Verify that we can convert the ID to a new fd, so we know it is actually
    // a valid program ID.
    int callee_fd2 = bpf_prog_get_fd_by_id(callee_id);
    REQUIRE(callee_fd2 > 0);
    Platform::_close(callee_fd2);

    bpf_link_ptr link(bpf_program__attach(caller));
    REQUIRE(link != nullptr);

    INITIALIZE_SAMPLE_CONTEXT
    uint32_t result;
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);
    REQUIRE(result == expected_result);

    uint32_t key = 0;
    uint32_t value = 0;
    error = bpf_map_lookup_elem(canary_map_fd, &key, &value);
    REQUIRE(error == 0);

    // Is bpf_tail_call expected to work?
    // Verify stack unwind occurred.
    if ((int)expected_result >= 0) {
        REQUIRE(value == 0);
    } else {
        REQUIRE(value != 0);
    }

    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);
    bpf_object__close(object);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("good_tail_call-jit", "[libbpf]")
{
    // Verify that 42 is returned, which is done by the callee.
    _ebpf_test_tail_call("tail_call.o", 42);
}
#endif

TEST_CASE("good_tail_call-native", "[libbpf]")
{
    // Verify that 42 is returned, which is done by the callee.
    _ebpf_test_tail_call("tail_call_um.dll", 42);
}

TEST_CASE("good_tail_call_same_section-native", "[libbpf]")
{
    // Verify that 42 is returned, which is done by the callee.
    _ebpf_test_tail_call("tail_call_same_section_um.dll", 42);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("bad_tail_call-jit", "[libbpf]")
{
    _ebpf_test_tail_call("tail_call_bad.o", (uint32_t)(-EBPF_INVALID_ARGUMENT));
}
#endif

TEST_CASE("bad_tail_call-native", "[libbpf]")
{
    _ebpf_test_tail_call("tail_call_bad_um.dll", (uint32_t)(-EBPF_INVALID_ARGUMENT));
}

static void
_multiple_tail_calls_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name =
        (execution_type == EBPF_EXECUTION_NATIVE ? "tail_call_multiple_um.dll" : "tail_call_multiple.o");

    struct bpf_object* object = bpf_object__open(file_name);
    REQUIRE(object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_program* caller = bpf_object__find_program_by_name(object, "caller");
    REQUIRE(caller != nullptr);

    struct bpf_program* callee0 = bpf_object__find_program_by_name(object, "callee0");
    REQUIRE(callee0 != nullptr);

    struct bpf_program* callee1 = bpf_object__find_program_by_name(object, "callee1");
    REQUIRE(callee1 != nullptr);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);

    int callee0_fd = bpf_program__fd(callee0);
    REQUIRE(callee0_fd >= 0);

    int callee1_fd = bpf_program__fd(callee1);
    REQUIRE(callee1_fd >= 0);

    int map_fd = bpf_map__fd(map);
    REQUIRE(map_fd >= 0);

    // Store callee0 at index 0.
    int index = 0;
    int error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&callee0_fd, 0);
    REQUIRE(error == 0);

    // Store callee1 at index 9.
    index = 9;
    error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&callee1_fd, 0);
    REQUIRE(error == 0);

    ebpf_id_t callee_id;
    // Verify that we can read the values back.
    index = 0;
    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &callee_id) == 0);

    // Verify that we can convert the ID to a new fd, so we know it is actually
    // a valid program ID.
    int callee0_fd2 = bpf_prog_get_fd_by_id(callee_id);
    REQUIRE(callee0_fd2 > 0);
    Platform::_close(callee0_fd2);

    index = 9;
    REQUIRE(bpf_map_lookup_elem(map_fd, &index, &callee_id) == 0);

    // Verify that we can convert the ID to a new fd, so we know it is actually
    // a valid program ID.
    int callee1_fd2 = bpf_prog_get_fd_by_id(callee_id);
    REQUIRE(callee1_fd2 > 0);
    Platform::_close(callee1_fd2);

    bpf_link_ptr link(bpf_program__attach(caller));
    REQUIRE(link != nullptr);

    auto packet = prepare_udp_packet(0, ETHERNET_TYPE_IPV4);
    INITIALIZE_SAMPLE_CONTEXT
    uint32_t result;
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);
    REQUIRE(result == 3);

    // Clear the prog array map entries. This is needed to release reference on the
    // programs which are inserted in the prog array.
    index = 0;
    REQUIRE(bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&ebpf_fd_invalid, 0) == 0);
    REQUIRE(error == 0);

    index = 9;
    REQUIRE(bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&ebpf_fd_invalid, 0) == 0);
    REQUIRE(error == 0);

    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);
    bpf_object__close(object);
}

DECLARE_JIT_TEST_CASES("multiple tail calls", "[libbpf]", _multiple_tail_calls_test);

static void
_test_bind_fd_to_prog_array(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t bind_program_info;
    REQUIRE(bind_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? "tail_call_um.dll" : "tail_call.o");
    struct bpf_object* sample_object = bpf_object__open(file_name);
    REQUIRE(sample_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);

    struct bpf_map* map = bpf_object__find_map_by_name(sample_object, "map");
    REQUIRE(map != nullptr);

    // Load a program of any other type.
    // Note: We are deliberately using "bindmonitor_um.dll" here as we want the programs to be loaded from
    // the individual dll, instead of the combined DLL. This helps in testing the DLL stub which is generated
    // bpf2c.exe tool.
    const char* another_file_name = (execution_type == EBPF_EXECUTION_NATIVE ? "bindmonitor_um.dll" : "bindmonitor.o");
    struct bpf_object* bind_object = bpf_object__open(another_file_name);
    REQUIRE(bind_object != nullptr);
    // Load the program(s).
    REQUIRE(bpf_object__load(bind_object) == 0);

    struct bpf_program* callee = bpf_object__find_program_by_name(bind_object, "BindMonitor");
    REQUIRE(callee != nullptr);

    int callee_fd = bpf_program__fd(callee);
    REQUIRE(callee_fd >= 0);

    int map_fd = bpf_map__fd(map);
    REQUIRE(map_fd >= 0);

    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(callee_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(strcmp(program_info.name, "BindMonitor") == 0);
    REQUIRE(program_info.type == BPF_PROG_TYPE_BIND);

    // Verify that we cannot add a BIND program fd to a prog_array map already
    // associated with a SAMPLE program.
    int index = 0;
    int error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&callee_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EBADF);

    bpf_object__close(bind_object);
    bpf_object__close(sample_object);
}

DECLARE_ALL_TEST_CASES("disallow setting bind fd in sample prog array", "[libbpf]", _test_bind_fd_to_prog_array);

static void
_load_inner_map(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? "inner_map_um.dll" : "inner_map.o");
    struct bpf_object* sample_object = bpf_object__open(file_name);
    REQUIRE(sample_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);

    struct bpf_map* map = bpf_object__find_map_by_name(sample_object, "outer_map");
    REQUIRE(map != nullptr);
    REQUIRE(bpf_map__type(map) == BPF_MAP_TYPE_HASH_OF_MAPS);

    bpf_object__close(sample_object);
}

DECLARE_ALL_TEST_CASES("Test loading BPF program with anonymous inner map", "[libbpf]", _load_inner_map);

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("disallow prog_array mixed program type values", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t bind_program_info;
    REQUIRE(bind_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    struct bpf_object* sample_object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(sample_object != nullptr);
    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);
    struct bpf_program* sample_program = bpf_object__find_program_by_name(sample_object, "test_program_entry");
    int sample_program_fd = bpf_program__fd(const_cast<const bpf_program*>(sample_program));

    struct bpf_object* bind_object = bpf_object__open("bindmonitor.o");
    REQUIRE(bind_object != nullptr);
    // Load the program(s).
    REQUIRE(bpf_object__load(bind_object) == 0);
    struct bpf_program* bind_program = bpf_object__find_program_by_name(bind_object, "BindMonitor");
    int bind_program_fd = bpf_program__fd(const_cast<const bpf_program*>(bind_program));

    // Create a map.
    int map_fd = bpf_map_create(BPF_MAP_TYPE_PROG_ARRAY, nullptr, sizeof(uint32_t), sizeof(uint32_t), 2, nullptr);
    REQUIRE(map_fd > 0);

    // Since the map is not yet associated with a program, the first program fd
    // we add will become the PROG_ARRAY's program type.
    int index = 0;
    int error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&sample_program_fd, 0);
    REQUIRE(error == 0);

    // Adding an entry with a different program type should fail.
    error = bpf_map_update_elem(map_fd, (uint8_t*)&index, (uint8_t*)&bind_program_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EBADF);

    Platform::_close(map_fd);
    bpf_object__close(bind_object);
    bpf_object__close(sample_object);
}
#endif

static void
_enumerate_program_ids_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    // Verify the enumeration is empty.
    uint32_t id1;
    REQUIRE(bpf_prog_get_next_id(0, &id1) < 0);
    REQUIRE(errno == ENOENT);

    REQUIRE(bpf_prog_get_next_id(EBPF_ID_NONE, &id1) < 0);
    REQUIRE(errno == ENOENT);

    // Load a file with multiple programs.
    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? "tail_call_um.dll" : "tail_call.o");
    struct bpf_object* sample_object = bpf_object__open(file_name);
    REQUIRE(sample_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);

    // Now enumerate the IDs.
    REQUIRE(bpf_prog_get_next_id(0, &id1) == 0);
    fd_t fd1 = bpf_prog_get_fd_by_id(id1);
    REQUIRE(fd1 >= 0);
    Platform::_close(fd1);

    uint32_t id2;
    REQUIRE(bpf_prog_get_next_id(id1, &id2) == 0);
    fd_t fd2 = bpf_prog_get_fd_by_id(id2);
    REQUIRE(fd2 >= 0);
    Platform::_close(fd2);

    uint32_t id3;
    REQUIRE(bpf_prog_get_next_id(id2, &id3) < 0);
    REQUIRE(errno == ENOENT);

    bpf_object__close(sample_object);
}

DECLARE_JIT_TEST_CASES("enumerate program IDs", "[libbpf]", _enumerate_program_ids_test);

static uint32_t
_ebpf_test_count_entries_map_in_map(int outer_map_fd)
{
    uint32_t entries_count = 0;
    uint32_t outer_key = 0;
    void* old_key = nullptr;
    void* key = &outer_key;

    while (bpf_map_get_next_key(outer_map_fd, old_key, key) == 0) {
        old_key = key;
        entries_count++;
    }
    return entries_count;
}

static void
_ebpf_test_map_in_map(ebpf_map_type_t type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    // Create an inner map that we'll use both as a template and as an actual entry.
    int inner_map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, "inner_map", sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(inner_map_fd > 0);

    // Verify that we cannot simply create an outer map without a template.
    REQUIRE(bpf_map_create(type, "array_map_of_maps", sizeof(__u32), sizeof(__u32), 2, nullptr) < 0);
    REQUIRE(errno == EBADF);

    // Verify that we cannot create an outer map with an invalid fd for the inner map.
    bpf_map_create_opts opts = {.inner_map_fd = (uint32_t)ebpf_fd_invalid};
    REQUIRE(bpf_map_create(type, "array_map_of_maps", sizeof(__u32), sizeof(fd_t), 2, &opts) < 0);
    REQUIRE(errno == EBADF);

    // Verify we can create an outer map with a template.
    opts.inner_map_fd = inner_map_fd;
    int outer_map_fd = bpf_map_create(type, "array_map_of_maps", sizeof(__u32), sizeof(fd_t), 2, &opts);
    REQUIRE(outer_map_fd > 0);

    // Verify that lookup of an empty slot in the map returns ENOENT.
    ebpf_id_t inner_map_id;
    __u32 outer_key = 0;
    REQUIRE(bpf_map_lookup_elem(outer_map_fd, &outer_key, &inner_map_id) == -ENOENT);
    REQUIRE(errno == ENOENT);

    // Verify we can insert the inner map into the outer map.
    int error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error == 0);

    uint32_t count = _ebpf_test_count_entries_map_in_map(outer_map_fd);
    // Verify the number of elements in the outer map.
    if (type == BPF_MAP_TYPE_HASH_OF_MAPS) {
        REQUIRE(count == 1);
    } else {
        // For ARRAY_OF_MAPS, the count is max_entries.
        REQUIRE(count == 2);
    }

    // Verify that we can read it back.
    REQUIRE(bpf_map_lookup_elem(outer_map_fd, &outer_key, &inner_map_id) == 0);

    // Verify that we can convert the ID to a new fd, so we know it is actually
    // a valid map ID.
    int inner_map_fd2 = bpf_map_get_fd_by_id(inner_map_id);
    REQUIRE(inner_map_fd2 > 0);
    Platform::_close(inner_map_fd2);

    // Verify we can't insert an integer into the outer map.
    __u32 bad_value = 12345678;
    outer_key = 1;
    error = bpf_map_update_elem(outer_map_fd, &outer_key, &bad_value, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EBADF);

    if (type == BPF_MAP_TYPE_HASH_OF_MAPS) {
        // Try deleting outer key that doesn't exist.
        error = bpf_map_delete_elem(outer_map_fd, &outer_key);
        REQUIRE(error < 0);
        REQUIRE(errno == ENOENT);
    }

    // Try deleting outer key that does exist.
    outer_key = 0;
    error = bpf_map_delete_elem(outer_map_fd, &outer_key);
    REQUIRE(error == 0);

    // Verify the number of elements in the outer map is 0.
    if (type == BPF_MAP_TYPE_HASH_OF_MAPS) {
        REQUIRE(_ebpf_test_count_entries_map_in_map(outer_map_fd) == 0);
    } else {
        // For ARRAY_OF_MAPS, the count is max_entries.
        REQUIRE(_ebpf_test_count_entries_map_in_map(outer_map_fd) == 2);
    }

    Platform::_close(inner_map_fd);
    Platform::_close(outer_map_fd);

    // Verify that all maps were successfully removed.
    uint32_t id;
    REQUIRE(bpf_map_get_next_id(0, &id) < 0);
    REQUIRE(errno == ENOENT);
}

// Verify libbpf can create and update arrays of maps.
TEST_CASE("simple array of maps", "[libbpf]") { _ebpf_test_map_in_map(BPF_MAP_TYPE_ARRAY_OF_MAPS); }

// Verify libbpf can create and update hash tables of maps.
TEST_CASE("simple hash of maps", "[libbpf]") { _ebpf_test_map_in_map(BPF_MAP_TYPE_HASH_OF_MAPS); }

// Verify an app can communicate with an eBPF program via an array of maps.
static void
_array_of_maps_test(ebpf_execution_type_t execution_type, _In_ PCSTR dll_name, _In_ PCSTR obj_name)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? dll_name : obj_name);
    struct bpf_object* sample_object = bpf_object__open(file_name);
    REQUIRE(sample_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);

    struct bpf_program* caller = bpf_object__find_program_by_name(sample_object, "lookup");
    REQUIRE(caller != nullptr);

    struct bpf_map* outer_map = bpf_object__find_map_by_name(sample_object, "outer_map");
    REQUIRE(outer_map != nullptr);

    int outer_map_fd = bpf_map__fd(outer_map);
    REQUIRE(outer_map_fd > 0);

    // Create an inner map.
    int inner_map_fd = bpf_map_create(BPF_MAP_TYPE_HASH, nullptr, sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(inner_map_fd > 0);

    // Add a value to the inner map.
    uint32_t inner_value = 42;
    uint32_t inner_key = 0;
    int error = bpf_map_update_elem(inner_map_fd, &inner_key, &inner_value, 0);
    REQUIRE(error == 0);

    // Add inner map to outer map.
    __u32 outer_key = 0;
    error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error == 0);

    bpf_link_ptr link(bpf_program__attach(caller));
    REQUIRE(link != nullptr);

    // Now run the ebpf program.
    INITIALIZE_SAMPLE_CONTEXT
    uint32_t result;
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);

    // Verify the return value is what we saved in the inner map.
    REQUIRE(result == inner_value);

    Platform::_close(inner_map_fd);
    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);
    bpf_object__close(sample_object);
}

// Create a map-in-map using BTF ids.
static void
_array_of_btf_maps_test(ebpf_execution_type_t execution_type)
{
    _array_of_maps_test(execution_type, "map_in_map_btf_um.dll", "map_in_map_btf.o");
}

DECLARE_JIT_TEST_CASES("array of btf maps", "[libbpf]", _array_of_btf_maps_test);

// Create a map-in-map using id and inner_id.
static void
_array_of_id_maps_test(ebpf_execution_type_t execution_type)
{
    _array_of_maps_test(execution_type, "map_in_map_legacy_id_um.dll", "map_in_map_legacy_id.o");
}

DECLARE_JIT_TEST_CASES("array of id maps", "[libbpf]", _array_of_id_maps_test);

// Create a map-in-map using map indices.
static void
_array_of_idx_maps_test(ebpf_execution_type_t execution_type)
{
    _array_of_maps_test(execution_type, "map_in_map_legacy_idx_um.dll", "map_in_map_legacy_idx.o");
}

DECLARE_JIT_TEST_CASES("array of idx maps", "[libbpf]", _array_of_idx_maps_test);

static void
_wrong_inner_map_types_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? "map_in_map_btf_um.dll" : "map_in_map_btf.o");
    struct bpf_object* sample_object = bpf_object__open(file_name);
    REQUIRE(sample_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(sample_object) == 0);

    struct bpf_map* outer_map = bpf_object__find_map_by_name(sample_object, "outer_map");
    REQUIRE(outer_map != nullptr);

    int outer_map_fd = bpf_map__fd(outer_map);
    REQUIRE(outer_map_fd > 0);

    // Create an inner map of the wrong type.
    int inner_map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, nullptr, sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(inner_map_fd > 0);

    // Try to add the array map to the outer map.
    __u32 outer_key = 0;
    int error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);

    Platform::_close(inner_map_fd);

    // Try an inner map with wrong key_size.
    inner_map_fd = bpf_map_create(BPF_MAP_TYPE_HASH, nullptr, sizeof(__u64), sizeof(__u32), 1, nullptr);
    REQUIRE(inner_map_fd > 0);
    error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    Platform::_close(inner_map_fd);

    // Try an inner map of the wrong value size.
    inner_map_fd = bpf_map_create(BPF_MAP_TYPE_HASH, nullptr, sizeof(__u32), sizeof(__u64), 1, nullptr);
    REQUIRE(inner_map_fd > 0);
    error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    Platform::_close(inner_map_fd);

    // Try an inner map with wrong max_entries.
    inner_map_fd = bpf_map_create(BPF_MAP_TYPE_HASH, nullptr, sizeof(__u32), sizeof(__u32), 2, nullptr);
    REQUIRE(inner_map_fd > 0);
    error = bpf_map_update_elem(outer_map_fd, &outer_key, &inner_map_fd, 0);
    REQUIRE(error < 0);
    REQUIRE(errno == EINVAL);
    Platform::_close(inner_map_fd);

    bpf_object__close(sample_object);
}

DECLARE_JIT_TEST_CASES("disallow wrong inner map types", "[libbpf]", _wrong_inner_map_types_test);

TEST_CASE("create map with name", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    // Create a map with a given name.
    PCSTR name = "mymapname";
    int map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, name, sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(map_fd > 0);

    // Make sure the name matches what we set.
    bpf_map_info info;
    uint32_t info_size = sizeof(info);
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &info, &info_size) == 0);
    REQUIRE(strcmp(info.name, name) == 0);

    Platform::_close(map_fd);
}

TEST_CASE("enumerate map IDs", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    // Verify the enumeration is empty.
    uint32_t id1;
    REQUIRE(bpf_map_get_next_id(0, &id1) < 0);
    REQUIRE(errno == ENOENT);

    REQUIRE(bpf_map_get_next_id(EBPF_ID_NONE, &id1) < 0);
    REQUIRE(errno == ENOENT);

    // Create two maps.
    int map1_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, nullptr, sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(map1_fd > 0);

    int map2_fd = bpf_map_create(BPF_MAP_TYPE_HASH, nullptr, sizeof(__u32), sizeof(__u32), 1, nullptr);
    REQUIRE(map2_fd > 0);

    // Now enumerate the IDs.
    REQUIRE(bpf_map_get_next_id(0, &id1) == 0);
    fd_t fd1 = bpf_map_get_fd_by_id(id1);
    REQUIRE(fd1 >= 0);
    Platform::_close(fd1);

    uint32_t id2;
    REQUIRE(bpf_map_get_next_id(id1, &id2) == 0);
    fd_t fd2 = bpf_map_get_fd_by_id(id2);
    REQUIRE(fd2 >= 0);
    Platform::_close(fd2);

    uint32_t id3;
    REQUIRE(bpf_map_get_next_id(id2, &id3) < 0);
    REQUIRE(errno == ENOENT);

    Platform::_close(map1_fd);
    Platform::_close(map2_fd);
    Platform::_close(fd1);
    Platform::_close(fd2);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("enumerate link IDs", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);
    program_info_provider_t bind_program_info;
    REQUIRE(bind_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);
    single_instance_hook_t sample_hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE);
    REQUIRE(sample_hook.initialize() == EBPF_SUCCESS);
    single_instance_hook_t bind_hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND);
    REQUIRE(bind_hook.initialize() == EBPF_SUCCESS);

    // Verify the enumeration is empty.
    uint32_t id1;
    REQUIRE(bpf_link_get_next_id(0, &id1) < 0);
    REQUIRE(errno == ENOENT);

    REQUIRE(bpf_link_get_next_id(EBPF_ID_NONE, &id1) < 0);
    REQUIRE(errno == ENOENT);

    // Load and attach some programs.
    program_load_attach_helper_t sample_helper;
    sample_helper.initialize(
        "test_sample_ebpf.o", BPF_PROG_TYPE_SAMPLE, "test_program_entry", EBPF_EXECUTION_JIT, nullptr, 0, sample_hook);
    program_load_attach_helper_t bind_helper;
    bind_helper.initialize(
        "bindmonitor.o", BPF_PROG_TYPE_BIND, "BindMonitor", EBPF_EXECUTION_JIT, nullptr, 0, bind_hook);

    // Now enumerate the IDs.
    REQUIRE(bpf_link_get_next_id(0, &id1) == 0);
    fd_t fd1 = bpf_link_get_fd_by_id(id1);
    REQUIRE(fd1 >= 0);
    Platform::_close(fd1);

    uint32_t id2;
    REQUIRE(bpf_link_get_next_id(id1, &id2) == 0);
    fd_t fd2 = bpf_link_get_fd_by_id(id2);
    REQUIRE(fd2 >= 0);
    Platform::_close(fd2);

    uint32_t id3;
    REQUIRE(bpf_link_get_next_id(id2, &id3) < 0);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("enumerate link IDs with bpf", "[libbpf][bpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);
    program_info_provider_t bind_program_info;
    REQUIRE(bind_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);
    single_instance_hook_t sample_hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE, BPF_LINK_TYPE_UNSPEC);
    REQUIRE(sample_hook.initialize() == EBPF_SUCCESS);
    single_instance_hook_t bind_hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND, BPF_LINK_TYPE_PLAIN);
    REQUIRE(bind_hook.initialize() == EBPF_SUCCESS);

    // Verify the enumeration is empty.
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    REQUIRE(bpf(BPF_LINK_GET_NEXT_ID, &attr, sizeof(attr)) == -ENOENT);

    memset(&attr, 0, sizeof(attr));
    attr.link_id = EBPF_ID_NONE;
    REQUIRE(bpf(BPF_LINK_GET_NEXT_ID, &attr, sizeof(attr)) == -ENOENT);

    // Load and attach some programs.
    program_load_attach_helper_t sample_helper;
    sample_helper.initialize(
        "test_sample_ebpf.o", BPF_PROG_TYPE_SAMPLE, "test_program_entry", EBPF_EXECUTION_JIT, nullptr, 0, sample_hook);
    program_load_attach_helper_t bind_helper;
    bind_helper.initialize(
        "bindmonitor.o", BPF_PROG_TYPE_BIND, "BindMonitor", EBPF_EXECUTION_JIT, nullptr, 0, bind_hook);

    // Now enumerate the IDs.
    memset(&attr, 0, sizeof(attr));
    REQUIRE(bpf(BPF_LINK_GET_NEXT_ID, &attr, sizeof(attr)) == 0);
    uint32_t id1 = attr.link_get_next_id.next_id;

    memset(&attr, 0, sizeof(attr));
    attr.link_id = id1;
    fd_t fd1 = bpf(BPF_LINK_GET_FD_BY_ID, &attr, sizeof(attr));
    REQUIRE(fd1 >= 0);

    REQUIRE(bpf(BPF_LINK_GET_NEXT_ID, &attr, sizeof(attr)) == 0);
    uint32_t id2 = attr.link_get_next_id.next_id;

    memset(&attr, 0, sizeof(attr));
    attr.link_id = id2;
    fd_t fd2 = bpf(BPF_LINK_GET_FD_BY_ID, &attr, sizeof(attr));
    REQUIRE(fd2 >= 0);

    REQUIRE(bpf(BPF_LINK_GET_NEXT_ID, &attr, sizeof(attr)) == -ENOENT);

    // Get info on the first link.
    memset(&attr, 0, sizeof(attr));
    sys_bpf_link_info_t info = {};
    attr.info.bpf_fd = fd1;
    attr.info.info = (uintptr_t)&info;
    attr.info.info_len = sizeof(info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(info.type == BPF_LINK_TYPE_UNSPEC);
    REQUIRE(info.id == id1);
    REQUIRE(info.prog_id != 0);

    // Detach the first link.
    memset(&attr, 0, sizeof(attr));
    attr.link_detach.link_fd = fd1;
    REQUIRE(bpf(BPF_LINK_DETACH, &attr, sizeof(attr)) == 0);

    // Get info on the detached link.
    memset(&attr, 0, sizeof(attr));
    attr.info.bpf_fd = fd1;
    attr.info.info = (uintptr_t)&info;
    attr.info.info_len = sizeof(info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(info.type == BPF_LINK_TYPE_UNSPEC);
    REQUIRE(info.id == id1);
    REQUIRE(info.prog_id == 0);

    // Pin the detached link.
    memset(&attr, 0, sizeof(attr));
    attr.obj_pin.bpf_fd = fd1;
    attr.obj_pin.pathname = (uintptr_t) "MyPath";
    REQUIRE(bpf(BPF_OBJ_PIN, &attr, sizeof(attr)) == 0);

    // Verify that bpf_fd must be 0 when calling BPF_OBJ_GET.
    REQUIRE(bpf(BPF_OBJ_GET, &attr, sizeof(attr)) == -EINVAL);

    // Retrieve a new fd from the pin path.
    attr.obj_pin.bpf_fd = 0;
    fd_t fd3 = bpf(BPF_OBJ_GET, &attr, sizeof(attr));
    REQUIRE(fd3 > 0);

    // Get info on the new fd.
    memset(&attr, 0, sizeof(attr));
    attr.info.bpf_fd = fd3;
    attr.info.info = (uintptr_t)&info;
    attr.info.info_len = sizeof(info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(info.id == id1);

    // Get info on the second link.
    memset(&attr, 0, sizeof(attr));
    info = {};
    attr.info.bpf_fd = fd2;
    attr.info.info = (uintptr_t)&info;
    attr.info.info_len = sizeof(info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(info.type == BPF_LINK_TYPE_PLAIN);
    REQUIRE(info.id == id2);
    REQUIRE(info.prog_id != 0);

    // And for completeness, try an invalid bpf() call.
    REQUIRE(bpf(-1, &attr, sizeof(attr)) == -EINVAL);

    // Unpin the link.
    REQUIRE(ebpf_object_unpin("MyPath") == EBPF_SUCCESS);

    Platform::_close(fd1);
    Platform::_close(fd2);
    Platform::_close(fd3);
}

TEST_CASE("bpf_prog_attach", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("cgroup_sock_addr.o");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "authorize_connect4");
    REQUIRE(program != nullptr);

    REQUIRE(bpf_object__load(object) == 0);

    int program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Verify we can't attach the program to an attach type that doesn't work with this API.
    REQUIRE(bpf_prog_attach(program_fd, 0, BPF_ATTACH_TYPE_SAMPLE, 0) == -ENOTSUP);

    // Verify we can't use an illegal program fd.
    REQUIRE(bpf_prog_attach(ebpf_fd_invalid, 0, BPF_CGROUP_INET4_CONNECT, 0) == -EBADF);

    // TODO (issue #1028): Currently one can pass an invalid attachable fd and bpf_prog_attach
    // will succeed because it's temporarily just treated as a compartment id. The following
    // should instead return errors.
    REQUIRE(bpf_prog_attach(program_fd, ebpf_fd_invalid, BPF_CGROUP_INET4_CONNECT, 0) == 0);
    uint32_t link_id;
    REQUIRE(bpf_link_get_next_id(0, &link_id) == 0);
    fd_t link_fd = bpf_link_get_fd_by_id(link_id);
    REQUIRE(link_fd >= 0);
    REQUIRE(bpf_link_detach(link_fd) == 0);
    REQUIRE(bpf_prog_attach(program_fd, program_fd, BPF_CGROUP_INET4_CONNECT, 0) == 0);

    bpf_object__close(object);
}

TEST_CASE("bpf_link__pin", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("test_sample_ebpf.o");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "test_program_entry");
    REQUIRE(program != nullptr);

    // Load and pin the program.
    REQUIRE(bpf_object__load(object) == 0);
    const char* program_pin_name = "ProgramPinName";
    REQUIRE(bpf_program__pin(program, program_pin_name) == 0);
    int program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Verify we can't attach the program to a different attach type.
    REQUIRE(bpf_prog_attach(program_fd, 0, BPF_CGROUP_INET4_CONNECT, 0) == -EINVAL);

    // Attach the program so we get a link object.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    // Verify that unpinning an unpinned link fails.
    REQUIRE(bpf_link__unpin(link.get()) == -ENOENT);

    // Verify that pinning a link to an already-in-use path fails.
    REQUIRE(bpf_link__pin(link.get(), program_pin_name) == -EEXIST);

    // Verify that pinning a link to a new path works.
    REQUIRE(bpf_link__pin(link.get(), "MyPath") == 0);

    // Verify that pinning an already-pinned link fails.
    REQUIRE(bpf_link__pin(link.get(), "MyPath2") == -EBUSY);

    REQUIRE(bpf_link__unpin(link.get()) == 0);

    REQUIRE(bpf_link__destroy(link.release()) == 0);
    REQUIRE(bpf_program__unpin(program, program_pin_name) == 0);

    bpf_program__unload(program);

    bpf_object__close(object);
}

TEST_CASE("bpf_obj_get_info_by_fd", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_SAMPLE) == EBPF_SUCCESS);
    single_instance_hook_t sample_hook(EBPF_PROGRAM_TYPE_SAMPLE, EBPF_ATTACH_TYPE_SAMPLE);
    REQUIRE(sample_hook.initialize() == EBPF_SUCCESS);
    program_load_attach_helper_t sample_helper;
    sample_helper.initialize(
        "map_reuse.o", BPF_PROG_TYPE_SAMPLE, "lookup_update", EBPF_EXECUTION_JIT, nullptr, 0, sample_hook);

    struct bpf_object* object = sample_helper.get_object();
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__next_program(object, nullptr);
    REQUIRE(program != nullptr);

    const char* program_name = bpf_program__name(program);
    REQUIRE(program_name != nullptr);

    int program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Fetch info about the maps and verify it matches what we'd expect.
    bpf_map_info map_info[3];
    uint32_t map_info_size = sizeof(bpf_map_info);

    // Find the inner map.
    struct bpf_map* map = bpf_object__find_map_by_name(object, "inner_map");
    REQUIRE(map != nullptr);

    int map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    bpf_map_info inner_map_info;
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &inner_map_info, &map_info_size) == 0);

    // Find the outer map.
    map = bpf_object__find_map_by_name(object, "outer_map");
    REQUIRE(map != nullptr);

    map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    const char* map_name = bpf_map__name(map);
    REQUIRE(map_name != nullptr);

    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &map_info[0], &map_info_size) == 0);

    bpf_map_info expected_map_info = {
        .type = BPF_MAP_TYPE_HASH_OF_MAPS,
        .key_size = sizeof(uint32_t),
        .value_size = sizeof(uint32_t),
        .max_entries = 1,
        .name = {0},
        .pinned_path_count = 1};
    expected_map_info.id = map_info[0].id;
    expected_map_info.inner_map_id = inner_map_info.id;
    strcpy_s(expected_map_info.name, sizeof(expected_map_info.name), map_name);
    if (strlen(map_name) < sizeof(expected_map_info.name)) {
        memset(expected_map_info.name + strlen(map_name), 0, sizeof(expected_map_info.name) - strlen(map_name));
    }

    // Verify the map info matches what we expect.
    REQUIRE(memcmp(&map_info[0], &expected_map_info, sizeof(expected_map_info)) == 0);

    // Find the second map.
    map = bpf_object__find_map_by_name(object, "port_map");
    REQUIRE(map != nullptr);

    map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    map_name = bpf_map__name(map);
    REQUIRE(map_name != nullptr);

    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &map_info[1], &map_info_size) == 0);

    expected_map_info.type = BPF_MAP_TYPE_ARRAY;
    expected_map_info.id = map_info[1].id;
    expected_map_info.inner_map_id = 0;
    strcpy_s(expected_map_info.name, sizeof(expected_map_info.name), map_name);
    if (strlen(map_name) < sizeof(expected_map_info.name)) {
        memset(expected_map_info.name + strlen(map_name), 0, sizeof(expected_map_info.name) - strlen(map_name));
    }

    // Verify the map info matches what we expect.
    REQUIRE(memcmp(&map_info[1], &expected_map_info, sizeof(expected_map_info)) == 0);

    // Get info but pass in a buffer that is too small.
    uint32_t reduced_map_info_size = sizeof(bpf_map_info) / 2;
    memset(&map_info[2], 0xa, sizeof(map_info[2]));
    REQUIRE(bpf_obj_get_info_by_fd(map_fd, &map_info[2], &reduced_map_info_size) == 0);
    // Verify the map info matches what we expect.
    REQUIRE(memcmp(&map_info[2], &expected_map_info, reduced_map_info_size) == 0);
    const std::vector<std::byte> buf(sizeof(bpf_map_info) - reduced_map_info_size, std::byte{0xa});
    REQUIRE(
        memcmp(
            reinterpret_cast<std::byte*>(&map_info[2]) + reduced_map_info_size,
            buf.data(),
            sizeof(bpf_map_info) - reduced_map_info_size) == 0);

    // Fetch info about the program and verify it matches what we'd expect.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(strcmp(program_info.name, program_name) == 0);
    REQUIRE(program_info.nr_map_ids == 2);
    REQUIRE(program_info.map_ids == 0);
    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);

    // Get info but pass in a buffer that smaller than the minimum required input size.
    bpf_prog_info reduced_program_info = {};
    uint32_t reduced_prog_info_size = EBPF_OFFSET_OF(bpf_prog_info, name) - 1;
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &reduced_program_info, &reduced_prog_info_size) == -EINVAL);

    // Get info but pass in a output buffer that is smaller than the full size of bpf_prog_info.
    reduced_prog_info_size = sizeof(bpf_prog_info) / 2;
    memset(&reduced_program_info, 0xa, sizeof(reduced_program_info));
    reduced_program_info.nr_map_ids = 0;
    reduced_program_info.map_ids = 0;
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &reduced_program_info, &reduced_prog_info_size) == 0);
    // Verify the program info matches what we expect.
    REQUIRE(memcmp(&reduced_program_info, &program_info, reduced_prog_info_size) == 0);
    const std::vector<std::byte> buf2(sizeof(bpf_prog_info) - reduced_prog_info_size, std::byte{0xa});
    REQUIRE(
        memcmp(
            reinterpret_cast<std::byte*>(&reduced_program_info) + reduced_prog_info_size,
            buf2.data(),
            sizeof(bpf_prog_info) - reduced_prog_info_size) == 0);

    // Fetch info about the maps and verify it matches what we'd expect.
    ebpf_id_t map_ids[2] = {0};
    program_info.map_ids = (uintptr_t)map_ids;
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(map_ids[0] == map_info[0].id);
    REQUIRE(map_ids[1] == map_info[1].id);

    // Try again with nr_map_ids set to get only partial.
    map_ids[0] = map_ids[1] = 0;
    program_info.nr_map_ids = 1;
    program_info.map_ids = (uintptr_t)map_ids;
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == -EFAULT);
    REQUIRE(map_ids[0] == map_info[0].id);

    // Try again with an invalid pointer.
    program_info.map_ids++;
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == -EFAULT);

    // Fetch info about the attachment and verify it matches what we'd expect.
    uint32_t link_id;
    REQUIRE(bpf_link_get_next_id(0, &link_id) == 0);
    fd_t link_fd = bpf_link_get_fd_by_id(link_id);
    REQUIRE(link_fd >= 0);

    bpf_link_info link_info;
    uint32_t link_info_size = sizeof(link_info);
    REQUIRE(bpf_obj_get_info_by_fd(link_fd, &link_info, &link_info_size) == 0);
    REQUIRE(link_info_size == sizeof(link_info));

    REQUIRE(link_info.prog_id == program_info.id);
    REQUIRE(link_info.attach_type == BPF_ATTACH_TYPE_SAMPLE);

    // Get info but pass in a buffer that is too small.
    bpf_link_info reduced_link_info = {};
    uint32_t reduced_link_info_size = sizeof(bpf_link_info) / 2;
    memset(&reduced_link_info, 0xa, sizeof(reduced_link_info));
    REQUIRE(bpf_obj_get_info_by_fd(link_fd, &reduced_link_info, &reduced_link_info_size) == 0);
    // Verify the link info matches what we expect.
    REQUIRE(memcmp(&reduced_link_info, &link_info, reduced_link_info_size) == 0);
    const std::vector<std::byte> buf3(sizeof(bpf_link_info) - reduced_link_info_size, std::byte{0xa});
    REQUIRE(
        memcmp(
            reinterpret_cast<std::byte*>(&reduced_link_info) + reduced_link_info_size,
            buf3.data(),
            sizeof(bpf_link_info) - reduced_link_info_size) == 0);

    // Verify we can detach using this link fd.
    // This is the flow used by bpftool to detach a link.
    REQUIRE(bpf_link_detach(link_fd) == 0);

    Platform::_close(link_fd);
}

TEST_CASE("bpf_obj_get_info_by_fd_2", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    program_info_provider_t sock_addr_program_info;
    REQUIRE(sock_addr_program_info.initialize(EBPF_PROGRAM_TYPE_CGROUP_SOCK_ADDR) == EBPF_SUCCESS);
    single_instance_hook_t v4_connect_hook(EBPF_PROGRAM_TYPE_CGROUP_SOCK_ADDR, EBPF_ATTACH_TYPE_CGROUP_INET4_CONNECT);
    REQUIRE(v4_connect_hook.initialize() == EBPF_SUCCESS);

    program_load_attach_helper_t sock_addr_helper;
    sock_addr_helper.initialize(
        "cgroup_sock_addr.o",
        BPF_PROG_TYPE_CGROUP_SOCK_ADDR,
        "authorize_connect4",
        EBPF_EXECUTION_JIT,
        nullptr,
        0,
        v4_connect_hook);

    struct bpf_object* object = sock_addr_helper.get_object();
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "authorize_connect4");
    REQUIRE(program != nullptr);

    int program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Fetch info about the program and verify it matches what we'd expect.
    bpf_prog_info program_info = {};
    uint32_t program_info_size = sizeof(program_info);
    REQUIRE(bpf_obj_get_info_by_fd(program_fd, &program_info, &program_info_size) == 0);
    REQUIRE(program_info_size == sizeof(program_info));
    REQUIRE(strcmp(program_info.name, "authorize_connect4") == 0);
    REQUIRE(program_info.nr_map_ids == 2);
    REQUIRE(program_info.map_ids == 0);
    REQUIRE(program_info.type == BPF_PROG_TYPE_CGROUP_SOCK_ADDR);

    // Fetch info about the attachment and verify it matches what we'd expect.
    uint32_t link_id;
    REQUIRE(bpf_link_get_next_id(0, &link_id) == 0);
    fd_t link_fd = bpf_link_get_fd_by_id(link_id);
    REQUIRE(link_fd >= 0);

    bpf_link_info link_info;
    uint32_t link_info_size = sizeof(link_info);
    REQUIRE(bpf_obj_get_info_by_fd(link_fd, &link_info, &link_info_size) == 0);
    REQUIRE(link_info_size == sizeof(link_info));

    REQUIRE(link_info.prog_id == program_info.id);
    REQUIRE(link_info.attach_type == BPF_CGROUP_INET4_CONNECT);

    // Verify we can detach using this link fd.
    // This is the flow used by bpftool to detach a link.
    REQUIRE(bpf_link_detach(link_fd) == 0);

    Platform::_close(link_fd);
}
#endif

TEST_CASE("libbpf_prog_type_by_name_test", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    bpf_prog_type prog_type;
    bpf_attach_type expected_attach_type;

    // Try a cross-platform type.
    REQUIRE(libbpf_prog_type_by_name("xdp", &prog_type, &expected_attach_type) == 0);
    REQUIRE(prog_type == BPF_PROG_TYPE_XDP);
    REQUIRE(expected_attach_type == BPF_XDP);

    // Try a Windows-specific type.
    REQUIRE(libbpf_prog_type_by_name("bind", &prog_type, &expected_attach_type) == 0);
    REQUIRE(prog_type == BPF_PROG_TYPE_BIND);
    REQUIRE(expected_attach_type == BPF_ATTACH_TYPE_BIND);

    // Try a random name. This should fail.
    REQUIRE(libbpf_prog_type_by_name("default", &prog_type, &expected_attach_type) == -ESRCH);
    REQUIRE(errno == ESRCH);

    REQUIRE(libbpf_prog_type_by_name(nullptr, &prog_type, &expected_attach_type) == -EINVAL);
    REQUIRE(errno == EINVAL);
}

TEST_CASE("libbpf_bpf_prog_type_str", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    const char* prog_type_str_sample = libbpf_bpf_prog_type_str(BPF_PROG_TYPE_SAMPLE);
    REQUIRE(prog_type_str_sample);
    REQUIRE(strcmp(prog_type_str_sample, "sample") == 0);
    const char* prog_type_str_unspec = libbpf_bpf_prog_type_str(BPF_PROG_TYPE_UNSPEC);
    REQUIRE(prog_type_str_unspec);
    REQUIRE(strcmp(prog_type_str_unspec, "unspec") == 0);
    REQUIRE(libbpf_bpf_prog_type_str((bpf_prog_type)123) == nullptr);
}

TEST_CASE("libbpf_get_error", "[libbpf]")
{
    errno = 123;
    REQUIRE(libbpf_get_error(nullptr) == -123);

    char buffer[80];
    REQUIRE(libbpf_strerror(errno, buffer, sizeof(buffer)) == -ENOENT);
    REQUIRE(strcmp(buffer, "Unknown libbpf error 123") == 0);
}

TEST_CASE("libbpf attach type names", "[libbpf]")
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    enum bpf_attach_type attach_type;
    for (int i = 1; i < __MAX_BPF_ATTACH_TYPE; i++) {
        // Skip types that are not defined in ebpfcore.
        if (ebpf_core_attach_types.find(static_cast<bpf_attach_type>(i)) == ebpf_core_attach_types.end())
            continue;
        const char* type_str = libbpf_bpf_attach_type_str((enum bpf_attach_type)i);

        REQUIRE(libbpf_attach_type_by_name(type_str, &attach_type) == 0);
        REQUIRE(attach_type == i);
    }
    REQUIRE(strcmp(libbpf_bpf_attach_type_str(BPF_ATTACH_TYPE_UNSPEC), "unspec") == 0);
    REQUIRE(libbpf_bpf_attach_type_str((bpf_attach_type)123) == nullptr);
    REQUIRE(libbpf_attach_type_by_name("other", &attach_type) == -ESRCH);
    REQUIRE(libbpf_attach_type_by_name(nullptr, &attach_type) == -EINVAL);
}

TEST_CASE("libbpf link type names", "[libbpf]")
{
    REQUIRE(strcmp(libbpf_bpf_link_type_str(BPF_LINK_TYPE_PLAIN), "plain") == 0);
    REQUIRE(strcmp(libbpf_bpf_link_type_str(BPF_LINK_TYPE_UNSPEC), "unspec") == 0);
    REQUIRE(strcmp(libbpf_bpf_link_type_str(BPF_LINK_TYPE_CGROUP), "cgroup") == 0);
    REQUIRE(strcmp(libbpf_bpf_link_type_str(BPF_LINK_TYPE_XDP), "xdp") == 0);
    REQUIRE(libbpf_bpf_link_type_str((bpf_link_type)123) == nullptr);
}

TEST_CASE("libbpf map type names", "[libbpf]")
{
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_UNSPEC), "unspec") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_HASH), "hash") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_ARRAY), "array") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_PROG_ARRAY), "prog_array") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_PERCPU_HASH), "percpu_hash") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_PERCPU_ARRAY), "percpu_array") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_HASH_OF_MAPS), "hash_of_maps") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_ARRAY_OF_MAPS), "array_of_maps") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_LRU_HASH), "lru_hash") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_LPM_TRIE), "lpm_trie") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_QUEUE), "queue") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_LRU_PERCPU_HASH), "lru_percpu_hash") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_STACK), "stack") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_RINGBUF), "ringbuf") == 0);
    REQUIRE(strcmp(libbpf_bpf_map_type_str(BPF_MAP_TYPE_PERF_EVENT_ARRAY), "perf_event_array") == 0);
    REQUIRE(libbpf_bpf_map_type_str((bpf_map_type)123) == nullptr);
}

TEST_CASE("bpf_object__open with .dll", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct bpf_object* object = bpf_object__open("test_sample_ebpf_um.dll");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__next_program(object, nullptr);
    REQUIRE(program != nullptr);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_SAMPLE);
    REQUIRE(bpf_program__get_expected_attach_type(program) == BPF_ATTACH_TYPE_SAMPLE);

    REQUIRE(bpf_object__next_program(object, program) == nullptr);

    // Trying to attach the program should fail since it's not loaded yet.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link == nullptr);
    REQUIRE(libbpf_get_error(link.get()) == -EINVAL);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    // Attach should now succeed.
    link.reset(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    REQUIRE(bpf_link__destroy(link.release()) == 0);

    bpf_object__close(object);
}

TEST_CASE("bpf_object__open_file with .dll", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    const char* my_object_name = "my_object_name";
    struct bpf_object_open_opts opts = {0};
    opts.object_name = my_object_name;
    struct bpf_object* object = bpf_object__open_file("test_sample_ebpf_um.dll", &opts);
    REQUIRE(object != nullptr);

    REQUIRE(strcmp(bpf_object__name(object), my_object_name) == 0);

    struct bpf_program* program = bpf_object__next_program(object, nullptr);
    REQUIRE(program != nullptr);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_SAMPLE);
    REQUIRE(bpf_program__get_expected_attach_type(program) == BPF_ATTACH_TYPE_SAMPLE);

    REQUIRE(bpf_object__next_program(object, program) == nullptr);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "test_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    // Trying to attach the program should fail since it's not loaded yet.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link == nullptr);
    REQUIRE(libbpf_get_error(link.get()) == -EINVAL);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    // The maps should now have FDs.
    map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "test_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    // Attach should now succeed.
    link.reset(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    REQUIRE(bpf_link__destroy(link.release()) == 0);

    bpf_object__close(object);
}

TEST_CASE("bpf_object__load with .dll", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    const char* my_object_name = "my_object_name";
    struct bpf_object_open_opts opts = {0};
    opts.object_name = my_object_name;
    struct bpf_object* object = bpf_object__open_file("droppacket_um.dll", &opts);
    REQUIRE(object != nullptr);

    REQUIRE(strcmp(bpf_object__name(object), my_object_name) == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "DropPacket");
    REQUIRE(program != nullptr);

    REQUIRE(bpf_program__fd(program) == ebpf_fd_invalid);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_XDP);

    // Make sure we cannot override the program type, since this is a native program.
    REQUIRE(bpf_program__set_type(program, BPF_PROG_TYPE_BIND) < 0);
    REQUIRE(errno == EINVAL);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_XDP);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    // Trying to attach the program should fail since it's not loaded yet.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link == nullptr);
    REQUIRE(libbpf_get_error(link.get()) == -EINVAL);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    // Attach should now succeed.
    link.reset(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    // The maps should now have FDs.
    map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    REQUIRE(bpf_link__destroy(link.release()) == 0);
    bpf_object__close(object);
}

#if !defined(CONFIG_BPF_JIT_DISABLED)
TEST_CASE("bpf_object__load with .o", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    const char* my_object_name = "my_object_name";
    struct bpf_object_open_opts opts = {0};
    opts.object_name = my_object_name;
    struct bpf_object* object = bpf_object__open_file("droppacket.o", &opts);
    REQUIRE(object != nullptr);

    REQUIRE(strcmp(bpf_object__name(object), my_object_name) == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "DropPacket");
    REQUIRE(program != nullptr);

    REQUIRE(bpf_program__fd(program) == ebpf_fd_invalid);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_XDP);

    // Make sure we can override the program type if desired.
    REQUIRE(bpf_program__set_type(program, BPF_PROG_TYPE_BIND) == 0);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_BIND);

    REQUIRE(bpf_program__set_type(program, BPF_PROG_TYPE_XDP) == 0);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    // Trying to attach the program should fail since it's not loaded yet.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link == nullptr);
    REQUIRE(libbpf_get_error(link.get()) == -EINVAL);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    // Attach should now succeed.
    link.reset(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    // The maps should now have FDs.
    map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    REQUIRE(bpf_link__destroy(link.release()) == 0);
    bpf_object__close(object);
}

TEST_CASE("bpf_object__load with .o from memory", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    const char* my_object_name = "my_object_name";
    struct bpf_object_open_opts opts = {0};
    opts.object_name = my_object_name;

    // Read droppacket.o into a std::vector.
    std::vector<uint8_t> object_data;
    std::fstream file("droppacket.o", std::ios::in | std::ios::binary);
    REQUIRE(file.is_open());
    file.seekg(0, std::ios::end);
    object_data.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read((char*)object_data.data(), object_data.size());
    file.close();

    struct bpf_object* object = bpf_object__open_mem(object_data.data(), object_data.size(), &opts);
    REQUIRE(object != nullptr);

    REQUIRE(strcmp(bpf_object__name(object), my_object_name) == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "DropPacket");
    REQUIRE(program != nullptr);

    REQUIRE(bpf_program__fd(program) == ebpf_fd_invalid);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_XDP);

    // Make sure we can override the program type if desired.
    REQUIRE(bpf_program__set_type(program, BPF_PROG_TYPE_BIND) == 0);
    REQUIRE(bpf_program__type(program) == BPF_PROG_TYPE_BIND);

    REQUIRE(bpf_program__set_type(program, BPF_PROG_TYPE_XDP) == 0);

    struct bpf_map* map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) == ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    // Trying to attach the program should fail since it's not loaded yet.
    bpf_link_ptr link(bpf_program__attach(program));
    REQUIRE(link == nullptr);
    REQUIRE(libbpf_get_error(link.get()) == -EINVAL);

    // Load the program.
    REQUIRE(bpf_object__load(object) == 0);

    // Attach should now succeed.
    link.reset(bpf_program__attach(program));
    REQUIRE(link != nullptr);

    // The maps should now have FDs.
    map = bpf_object__next_map(object, nullptr);
    REQUIRE(map != nullptr);
    REQUIRE(strcmp(bpf_map__name(map), "interface_index_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(strcmp(bpf_map__name(map), "dropped_packet_map") == 0);
    REQUIRE(bpf_map__fd(map) != ebpf_fd_invalid);
    map = bpf_object__next_map(object, map);
    REQUIRE(map == nullptr);

    REQUIRE(bpf_link__destroy(link.release()) == 0);
    bpf_object__close(object);
}

// Test that bpf() accepts a smaller and a larger bpf_attr.
TEST_CASE("bpf() backwards compatibility", "[libbpf][bpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    struct
    {
        union bpf_attr attr;
        char pad[3];
    } tmp = {};
    union bpf_attr* attr = &tmp.attr;

    attr->map_create.map_type = BPF_MAP_TYPE_ARRAY;
    attr->map_create.key_size = sizeof(uint32_t);
    attr->map_create.value_size = sizeof(uint32_t);
    attr->map_create.max_entries = 2;
    attr->map_create.map_flags = 0;

    // Truncate bpf_attr before map_flags.
    int map_fd = bpf(BPF_MAP_CREATE, attr, offsetof(union bpf_attr, map_create.map_flags));
    REQUIRE(map_fd > 0);
    Platform::_close(map_fd);

    // Pass extra trailing bytes.
    map_fd = bpf(BPF_MAP_CREATE, attr, sizeof(tmp));
    REQUIRE(map_fd > 0);
    Platform::_close(map_fd);

    // Ensure that non-zero trailing bytes are rejected.
    tmp.pad[0] = 1;
    map_fd = bpf(BPF_MAP_CREATE, attr, sizeof(tmp));
    REQUIRE(map_fd == -EINVAL);
}

// Test bpf() with the following command ids:
// BPF_PROG_LOAD, BPF_OBJ_GET_INFO_BY_FD, BPF_PROG_GET_NEXT_ID,
// BPF_MAP_CREATE, BPF_MAP_GET_NEXT_ID, BPF_PROG_BIND_MAP,
// BPF_PROG_GET_FD_BY_ID, BPF_MAP_GET_FD_BY_ID, and BPF_MAP_GET_FD_BY_ID.
TEST_CASE("BPF_PROG_BIND_MAP etc.", "[libbpf][bpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    prevail::EbpfInst instructions[] = {
        {0xb7, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},             // return r0
    };

    // Load and verify the eBPF program.
    union bpf_attr attr = {};
    attr.prog_load.prog_type = BPF_PROG_TYPE_SAMPLE;
    attr.prog_load.insns = (uintptr_t)instructions;
    attr.prog_load.insn_cnt = _countof(instructions);
    int program_fd = bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
    REQUIRE(program_fd >= 0);

    // Now query the program info and verify it matches what we set.
    sys_bpf_prog_info_t program_info = {};
    memset(&attr, 0, sizeof(attr));
    attr.info.bpf_fd = program_fd;
    attr.info.info = (uintptr_t)&program_info;
    attr.info.info_len = sizeof(program_info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.info.info_len == sizeof(program_info));
    REQUIRE(program_info.nr_map_ids == 0);
    REQUIRE(program_info.type == BPF_PROG_TYPE_SAMPLE);
    REQUIRE(strnlen(program_info.name, sizeof(program_info.name)) == 0);

    // Verify we can enumerate the program id.
    memset(&attr, 0, sizeof(attr));
    attr.prog_get_next_id.start_id = 0;
    REQUIRE(bpf(BPF_PROG_GET_NEXT_ID, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.prog_get_next_id.next_id == program_info.id);

    // Verify we can convert the program id to an fd.
    memset(&attr, 0, sizeof(attr));
    attr.prog_id = program_info.id;
    int prog_fd2 = bpf(BPF_PROG_GET_FD_BY_ID, &attr, sizeof(attr));
    REQUIRE(prog_fd2 > 0);
    Platform::_close(prog_fd2);

    // Create a map.
    memset(&attr, 0, sizeof(attr));
    attr.map_create.map_type = BPF_MAP_TYPE_ARRAY;
    attr.map_create.key_size = sizeof(uint32_t);
    attr.map_create.value_size = sizeof(uint32_t);
    attr.map_create.max_entries = 2;
    attr.map_create.map_flags = 0;
    int map_fd = bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
    REQUIRE(map_fd > 0);

    // Query the map id.
    sys_bpf_map_info_t map_info = {};
    memset(&attr, 0, sizeof(attr));
    attr.info.bpf_fd = map_fd;
    attr.info.info = (uintptr_t)&map_info;
    attr.info.info_len = sizeof(map_info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    ebpf_id_t map_id = map_info.id;

    // Verify we can enumerate the map id.
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_id.start_id = 0;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_ID, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.map_get_next_id.next_id == map_id);

    // Verify we can convert the map id to an fd.
    memset(&attr, 0, sizeof(attr));
    attr.map_id = map_id;
    int map_fd2 = bpf(BPF_MAP_GET_FD_BY_ID, &attr, sizeof(attr));
    REQUIRE(map_fd2 > 0);
    Platform::_close(map_fd2);

    // Bind it to the program.
    memset(&attr, 0, sizeof(attr));
    attr.prog_bind_map.prog_fd = program_fd;
    attr.prog_bind_map.map_fd = map_fd;
    attr.prog_bind_map.flags = 0;
    REQUIRE(bpf(BPF_PROG_BIND_MAP, &attr, sizeof(attr)) == 0);

    // Verify we can create a nested array map.
    uint32_t key = 0;
    fd_t value = map_fd;
    attr.map_create = {};
    attr.map_create.map_type = BPF_MAP_TYPE_ARRAY_OF_MAPS;
    attr.map_create.key_size = sizeof(key);
    attr.map_create.value_size = sizeof(value);
    attr.map_create.max_entries = 1;
    attr.map_create.inner_map_fd = map_fd;
    int nested_map_fd = bpf(BPF_MAP_CREATE, &attr, sizeof(attr.map_create));
    REQUIRE(nested_map_fd >= 0);

    // Ensure we can insert map_fd into the outer map.
    attr.map_update = {};
    attr.map_update.map_fd = nested_map_fd;
    attr.map_update.key = (uintptr_t)&key;
    attr.map_update.value = (uintptr_t)&value;
    REQUIRE(bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr.map_update)) == 0);

    // Ensure that looking up the same key gives the ID of the inner map.
    ebpf_id_t value_id = 0;
    attr.map_lookup = {};
    attr.map_lookup.map_fd = nested_map_fd;
    attr.map_lookup.key = (uintptr_t)&key;
    attr.map_lookup.value = (uintptr_t)&value_id;
    REQUIRE(bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr.map_lookup)) == 0);
    REQUIRE(value_id == map_id);

    Platform::_close(nested_map_fd);

    // Verify we can create a nested hash map.
    attr.map_create = {};
    attr.map_create.map_type = BPF_MAP_TYPE_HASH_OF_MAPS;
    attr.map_create.key_size = sizeof(key);
    attr.map_create.value_size = sizeof(value);
    attr.map_create.max_entries = 1;
    attr.map_create.inner_map_fd = map_fd;
    nested_map_fd = bpf(BPF_MAP_CREATE, &attr, sizeof(attr.map_create));
    REQUIRE(nested_map_fd >= 0);

    // Ensure we can insert map_fd into the outer map.
    attr.map_update = {};
    attr.map_update.map_fd = nested_map_fd;
    attr.map_update.key = (uintptr_t)&key;
    attr.map_update.value = (uintptr_t)&value;
    REQUIRE(bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr.map_update)) == 0);

    // Ensure that looking up the same key gives the ID of the inner map.
    value_id = 0;
    attr.map_lookup = {};
    attr.map_lookup.map_fd = nested_map_fd;
    attr.map_lookup.key = (uintptr_t)&key;
    attr.map_lookup.value = (uintptr_t)&value_id;
    REQUIRE(bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr.map_lookup)) == 0);
    REQUIRE(value_id == map_id);

    Platform::_close(nested_map_fd);

    // Release our own references on the map and program.
    Platform::_close(map_fd);
    Platform::_close(program_fd);
}

// Test bpf() with the following command ids:
//  BPF_PROG_ATTACH, BPF_PROG_DETACH
TEST_CASE("BPF_PROG_ATTACH", "[libbpf][bpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    // Load and verify the eBPF program.
    union bpf_attr attr = {};

    struct bpf_object* object = bpf_object__open("cgroup_sock_addr.o");
    REQUIRE(object != nullptr);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "authorize_connect4");
    REQUIRE(program != nullptr);

    REQUIRE(bpf_object__load(object) == 0);

    int program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Verify we can't attach the program using an attach type that doesn't work with this API.
    memset(&attr, 0, sizeof(attr));
    attr.prog_attach.attach_bpf_fd = program_fd;
    attr.prog_attach.target_fd = program_fd;
    attr.prog_attach.attach_flags = 0;
    attr.prog_attach.attach_type = BPF_ATTACH_TYPE_SAMPLE;
    REQUIRE(bpf(BPF_PROG_ATTACH, &attr, sizeof(attr)) == -ENOTSUP);

    // Verify we can attach the program.
    memset(&attr, 0, sizeof(attr));
    attr.prog_attach.attach_bpf_fd = program_fd;
    // TODO (issue #1028): Currently the target_fd is treated as a compartment id.
    attr.prog_attach.target_fd = program_fd;
    attr.prog_attach.attach_flags = 0;
    attr.prog_attach.attach_type = BPF_CGROUP_INET4_CONNECT;
    REQUIRE(bpf(BPF_PROG_ATTACH, &attr, sizeof(attr)) == 0);

    // Verify we can detach the program.
    memset(&attr, 0, sizeof(attr));
    attr.prog_detach.target_fd = program_fd;
    attr.prog_detach.attach_type = BPF_CGROUP_INET4_CONNECT;
    REQUIRE(bpf(BPF_PROG_DETACH, &attr, sizeof(attr)) == 0);

    // Verify we can't detach the program using a type that doesn't work with this API.
    memset(&attr, 0, sizeof(attr));
    attr.prog_detach.target_fd = program_fd;
    attr.prog_detach.attach_type = BPF_ATTACH_TYPE_SAMPLE;
    REQUIRE(bpf(BPF_PROG_DETACH, &attr, sizeof(attr)) == -ENOTSUP);
}
#endif

// Test bpf() with the following command ids:
// BPF_MAP_CREATE, BPF_MAP_UPDATE_ELEM, BPF_MAP_LOOKUP_ELEM,
// BPF_MAP_GET_NEXT_KEY, BPF_MAP_LOOKUP_AND_DELETE_ELEM, and BPF_MAP_DELETE_ELEM.
TEST_CASE("BPF_MAP_GET_NEXT_KEY etc.", "[libbpf][bpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Create a hash map.
    union bpf_attr attr = {};
    attr.map_create.map_type = BPF_MAP_TYPE_HASH;
    attr.map_create.key_size = sizeof(uint32_t);
    attr.map_create.value_size = sizeof(uint32_t);
    attr.map_create.max_entries = 3;
    attr.map_create.map_flags = 0;
    int map_fd = bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
    REQUIRE(map_fd > 0);

    // Add an entry.
    uint64_t value = 12345;
    uint32_t key = 42;
    memset(&attr, 0, sizeof(attr));
    attr.map_update.map_fd = map_fd;
    attr.map_update.key = (uintptr_t)&key;
    attr.map_update.value = (uintptr_t)&value;
    attr.map_update.flags = 0;
    REQUIRE(bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr)) == 0);

    // Look up the entry.
    value = 0;
    memset(&attr, 0, sizeof(attr));
    attr.map_lookup.map_fd = map_fd;
    attr.map_lookup.key = (uintptr_t)&key;
    attr.map_lookup.value = (uintptr_t)&value;
    REQUIRE(bpf(BPF_MAP_LOOKUP_ELEM, &attr, sizeof(attr)) == 0);
    REQUIRE(value == 12345);

    // Enumerate the entry.
    uint32_t next_key;
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_key.map_fd = map_fd;
    attr.map_get_next_key.key = 0;
    attr.map_get_next_key.next_key = (uintptr_t)&next_key;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr)) == 0);
    REQUIRE(next_key == key);

    // Verify the entry is the last entry.
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_key.map_fd = map_fd;
    attr.map_get_next_key.key = (uintptr_t)&key;
    attr.map_get_next_key.next_key = (uintptr_t)&next_key;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr)) < 0);
    REQUIRE(errno == ENOENT);

    // Delete the entry.
    memset(&attr, 0, sizeof(attr));
    attr.map_delete.map_fd = map_fd;
    attr.map_delete.key = (uintptr_t)&key;
    REQUIRE(bpf(BPF_MAP_DELETE_ELEM, &attr, sizeof(attr)) == 0);

    // Look up and delete the entry.
    memset(&attr, 0, sizeof(attr));
    value = 0;
    key = 42;
    attr.map_lookup.map_fd = map_fd;
    attr.map_lookup.key = (uintptr_t)&key;
    attr.map_lookup.value = (uintptr_t)&value;

    // Add the element back to the entry after the previous test entry deletion.
    bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr));
    REQUIRE(bpf(BPF_MAP_LOOKUP_AND_DELETE_ELEM, &attr, sizeof(attr)) == 0);

    // Test the API again and verify looking up and deleting fails.
    REQUIRE(bpf(BPF_MAP_LOOKUP_AND_DELETE_ELEM, &attr, sizeof(attr)) < 0);
    REQUIRE(errno == ENOENT);

    // Verify that no entries exist.
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_key.map_fd = map_fd;
    attr.map_get_next_key.key = 0;
    attr.map_get_next_key.next_key = (uintptr_t)&next_key;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr)) < 0);
    REQUIRE(errno == ENOENT);

    // Test that the bpf_map_get_next_key returns the first key of the map if the previous key is not found.
    // Add 3 entries into the now empty map.
    for (key = 100; key < 400; key += 100) {
        value = 0;
        memset(&attr, 0, sizeof(attr));
        attr.map_update.map_fd = map_fd;
        attr.map_update.key = (uintptr_t)&key;
        attr.map_update.value = (uintptr_t)&value;
        REQUIRE(bpf(BPF_MAP_UPDATE_ELEM, &attr, sizeof(attr)) == 0);
    }

    // Look up the first key in the map, so we can check that it's returned later.
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_key.map_fd = map_fd;
    attr.map_get_next_key.key = NULL;
    attr.map_get_next_key.next_key = (uintptr_t)&next_key;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr)) == 0);
    uint64_t first_key = next_key;

    // Look up a key that is not present in the map, and check that the first key is returned.
    key = 123;
    memset(&attr, 0, sizeof(attr));
    attr.map_get_next_key.map_fd = map_fd;
    attr.map_get_next_key.key = (uintptr_t)&key;
    attr.map_get_next_key.next_key = (uintptr_t)&next_key;
    REQUIRE(bpf(BPF_MAP_GET_NEXT_KEY, &attr, sizeof(attr)) == 0);
    REQUIRE(next_key == first_key);

    Platform::_close(map_fd);
}

TEST_CASE("Map and program information", "[libbpf][bpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    // Create a map.
    sys_bpf_map_create_attr_t map_create = {};
    map_create.map_type = BPF_MAP_TYPE_ARRAY;
    map_create.key_size = sizeof(uint32_t);
    map_create.value_size = sizeof(uint32_t);
    map_create.max_entries = 2;
    strncpy_s(map_create.map_name, "testing", sizeof(map_create.map_name));
    int map_fd = bpf(BPF_MAP_CREATE, (union bpf_attr*)&map_create, sizeof(map_create));
    REQUIRE(map_fd > 0);

    // Retrieve a prefix of map info.
    sys_bpf_map_info_t map_info = {};
    union bpf_attr attr = {};
    attr.info.bpf_fd = map_fd;
    attr.info.info = (uintptr_t)&map_info;
    attr.info.info_len = offsetof(map_info, key_size);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.info.info_len == offsetof(map_info, key_size));
    REQUIRE(map_info.type == map_create.map_type);
    REQUIRE(map_info.id != 0);
    REQUIRE(map_info.key_size == 0);

    // Retrieve the map info.
    attr = {};
    attr.info.bpf_fd = map_fd;
    attr.info.info = (uintptr_t)&map_info;
    attr.info.info_len = sizeof(map_info);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.info.info_len == sizeof(map_info));
    REQUIRE(map_info.type == map_create.map_type);
    REQUIRE(map_info.id != 0);
    REQUIRE(map_info.key_size == map_create.key_size);
    REQUIRE(map_info.value_size == map_create.value_size);
    REQUIRE(map_info.max_entries == map_create.max_entries);
    REQUIRE(map_info.map_flags == map_create.map_flags);
    REQUIRE(strncmp(map_info.name, map_create.map_name, sizeof(map_info.name)) == 0);

#if !defined(CONFIG_BPF_JIT_DISABLED)
    prevail::EbpfInst instructions[] = {
        {INST_ALU_OP_MOV | INST_CLS_ALU64, R0_RETURN_VALUE, 0}, // r0 = 0
        {INST_OP_EXIT},                                         // return r0
    };

    // Load and verify the eBPF program.
    attr = {};
    attr.prog_load.prog_type = BPF_PROG_TYPE_SAMPLE;
    attr.prog_load.insns = (uintptr_t)instructions;
    attr.prog_load.insn_cnt = _countof(instructions);
    strncpy_s(attr.prog_load.prog_name, "testing", sizeof(attr.prog_load.prog_name));
    int program_fd = bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
    REQUIRE(program_fd >= 0);

    // Bind map to the program so that the ID is returned in the info.
    attr = {};
    attr.prog_bind_map.prog_fd = program_fd;
    attr.prog_bind_map.map_fd = map_fd;
    REQUIRE(bpf(BPF_PROG_BIND_MAP, &attr, sizeof(attr)) == 0);

    // Verify that we can query a prefix of fields.
    sys_bpf_prog_info_t program_info = {};
    attr = {};
    attr.info.bpf_fd = program_fd;
    attr.info.info = (uintptr_t)&program_info;
    attr.info.info_len = offsetof(program_info, tag);
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.info.info_len == offsetof(program_info, tag));
    REQUIRE(program_info.id != 0);
    REQUIRE(program_info.nr_map_ids == 0);
    REQUIRE(strnlen(program_info.name, sizeof(program_info.name)) == 0);

    // Query the full program info.
    ebpf_id_t map_ids[2] = {};
    attr.info.info_len = sizeof(program_info);
    program_info.nr_map_ids = sizeof(map_ids) / sizeof(map_ids[0]);
    program_info.map_ids = (uintptr_t)map_ids;
    REQUIRE(bpf(BPF_OBJ_GET_INFO_BY_FD, &attr, sizeof(attr)) == 0);
    REQUIRE(attr.info.info_len == sizeof(program_info));
    REQUIRE(program_info.tag[0] == 0);
    REQUIRE(program_info.jited_prog_len == 0);
    REQUIRE(program_info.xlated_prog_len == 0);
    REQUIRE(program_info.jited_prog_insns == 0);
    REQUIRE(program_info.xlated_prog_insns == 0);
    REQUIRE(program_info.load_time == 0);
    REQUIRE(program_info.created_by_uid == 0);
    REQUIRE(program_info.nr_map_ids == 1);
    REQUIRE(map_ids[0] == map_info.id);
    REQUIRE(strncmp(program_info.name, "testing", sizeof(program_info.name)) == 0);
#endif
}

TEST_CASE("BPF_PROG_LOAD logging", "[libbpf][bpf]")
{
#if !defined(CONFIG_BPF_JIT_DISABLED) || !defined(CONFIG_BPF_INTERPRETER_DISABLED)
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    prevail::EbpfInst program[] = {
        {INST_OP_EXIT, 0, 0, 0} // Bare return instruction.
    };
    const size_t program_size = sizeof(program);
    char log_buf[256] = {};

    // log_true_size must reflect the minimum size of the log buffer.
    sys_bpf_prog_load_attr_t attr = {};
    attr.prog_type = BPF_PROG_TYPE_SAMPLE;
    attr.insns = reinterpret_cast<uint64_t>(program);
    attr.insn_cnt = program_size / sizeof(prevail::EbpfInst);
    attr.log_buf = reinterpret_cast<uint64_t>(log_buf);
    attr.log_size = sizeof(log_buf);
    attr.log_level = 1;

    int fd = bpf(BPF_PROG_LOAD, (union bpf_attr*)&attr, sizeof(attr));
    REQUIRE(fd < 0);
    REQUIRE(strlen(log_buf) + 1 == attr.log_true_size);

    // a smaller log buffer should fail with ENOSPC.
    attr.log_size = 1;
    fd = bpf(BPF_PROG_LOAD, (union bpf_attr*)&attr, sizeof(attr));
    REQUIRE(fd == -ENOSPC);
#else
    // If JIT or interpreter is disabled, ensure the error is ERROR_NOT_SUPPORTED.
    union bpf_attr attr = {};
    int fd = bpf(BPF_PROG_LOAD, &attr, sizeof(attr));
    REQUIRE(fd < 0);
    REQUIRE(GetLastError() == ERROR_NOT_SUPPORTED);
#endif
}

TEST_CASE("libbpf_num_possible_cpus", "[libbpf]")
{
    int cpu_count = libbpf_num_possible_cpus();
    REQUIRE(cpu_count > 0);
}

void
_test_nested_maps(bpf_map_type map_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    // First, create an inner map.
    fd_t inner_map_fd1 =
        bpf_map_create(BPF_MAP_TYPE_ARRAY, "inner_map1", sizeof(uint32_t), sizeof(uint32_t), 1, nullptr);
    REQUIRE(inner_map_fd1 > 0);

    // Create outer map with the inner map handle in options.
    bpf_map_create_opts opts = {.inner_map_fd = (uint32_t)inner_map_fd1};
    fd_t outer_map_fd = bpf_map_create(map_type, "outer_map", sizeof(uint32_t), sizeof(fd_t), 10, &opts);
    REQUIRE(outer_map_fd > 0);

    // Create second inner map.
    fd_t inner_map_fd2 =
        bpf_map_create(BPF_MAP_TYPE_ARRAY, "inner_map2", sizeof(uint32_t), sizeof(uint32_t), 1, nullptr);
    REQUIRE(inner_map_fd2 > 0);

    // Insert both inner maps in outer map.
    uint32_t key = 1;
    uint32_t result = bpf_map_update_elem(outer_map_fd, &key, &inner_map_fd1, 0);
    REQUIRE(result == ERROR_SUCCESS);

    key = 2;
    result = bpf_map_update_elem(outer_map_fd, &key, &inner_map_fd2, 0);
    REQUIRE(result == ERROR_SUCCESS);

    // Remove the inner maps from outer map.
    key = 1;
    result = bpf_map_delete_elem(outer_map_fd, &key);
    REQUIRE(result == ERROR_SUCCESS);

    key = 2;
    result = bpf_map_delete_elem(outer_map_fd, &key);
    REQUIRE(result == ERROR_SUCCESS);

    Platform::_close(inner_map_fd2);
    Platform::_close(inner_map_fd1);
    Platform::_close(outer_map_fd);
}

TEST_CASE("array_map_of_maps", "[libbpf]") { _test_nested_maps(BPF_MAP_TYPE_ARRAY_OF_MAPS); }
TEST_CASE("hash_map_of_maps", "[libbpf]") { _test_nested_maps(BPF_MAP_TYPE_HASH_OF_MAPS); }

TEST_CASE("libbpf_load_stress", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    std::vector<std::jthread> threads;
    // Schedule 4 threads per CPU to force contention.
    for (size_t i = 0; i < static_cast<size_t>(ebpf_get_cpu_count()) * 4; i++) {
        // Initialize thread object with lambda plus stop token
        threads.emplace_back([i](std::stop_token stop_token) {
            while (!stop_token.stop_requested()) {
                struct bpf_object* object = bpf_object__open("test_sample_ebpf_um.dll");
                if (!object) {
                    break;
                }
                // Enumerate maps and programs.
                bpf_program* program;
                bpf_object__for_each_program(program, object) {}
                bpf_map* map;
                bpf_object__for_each_map(map, object) {}
                bpf_object__close(object);
            }
        });
    }

    // Wait for 10 seconds.
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // Stop all threads.
    for (auto& thread : threads) {
        thread.request_stop();
    }

    // Wait for all threads to stop.
    for (auto& thread : threads) {
        thread.join();
    }
}

TEST_CASE("recursive_tail_call", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    struct bpf_object* object = bpf_object__open("tail_call_recursive_um.dll");
    REQUIRE(object != nullptr);

    // Load the BPF program.
    REQUIRE(bpf_object__load(object) == 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "recurse");
    REQUIRE(program != nullptr);

    // Get the map used to store the next program to call.
    struct bpf_map* map = bpf_object__find_map_by_name(object, "map");
    REQUIRE(map != nullptr);

    // Get the map used to record the number of times the program was called.
    struct bpf_map* canary_map = bpf_object__find_map_by_name(object, "canary");
    REQUIRE(canary_map != nullptr);

    // Get the fd for the program.
    fd_t program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Get the fd of the map.
    fd_t map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    // Get the fd of the canary map.
    fd_t canary_map_fd = bpf_map__fd(canary_map);
    REQUIRE(canary_map_fd > 0);

    uint32_t key = 0;
    uint32_t value = 0;
    REQUIRE(bpf_map_update_elem(canary_map_fd, &key, &value, 0) == 0);

    bpf_test_run_opts opts = {};
    sample_program_context_t in_ctx{0};
    sample_program_context_t out_ctx{0};
    opts.repeat = 1;
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = sizeof(in_ctx);
    opts.ctx_out = reinterpret_cast<uint8_t*>(&out_ctx);
    opts.ctx_size_out = sizeof(out_ctx);

    capture_helper_t capture;
    std::vector<std::string> output;
    errno_t error = capture.begin_capture();
    if (error == NO_ERROR) {
        // Run the program.
        usersim_trace_logging_set_enabled(true, EBPF_TRACELOG_LEVEL_INFO, EBPF_TRACELOG_KEYWORD_PRINTK);
        int result = bpf_prog_test_run_opts(program_fd, &opts);
        usersim_trace_logging_set_enabled(false, 0, 0);

        output = capture.buffer_to_printk_vector(capture.get_stdout_contents());
        REQUIRE(result == 0);
    }

    // Verify that the printk output is correct.
    // In 'recurse' ebpf program, the printk is added even to the caller.
    // Hence the printk count is called MAX_TAIL_CALL_CNT + 1 times.
    REQUIRE(output.size() == MAX_TAIL_CALL_CNT + 1);
    for (size_t i = 0; i < MAX_TAIL_CALL_CNT + 1; i++) {
        REQUIRE(output[i] == std::format("recurse: *value={}", i));
    }

    // The program should have returned -EBPF_NO_MORE_TAIL_CALLS.
    REQUIRE(opts.retval == -EBPF_NO_MORE_TAIL_CALLS);

    // Read the map to determine how many times the program was called.
    REQUIRE(bpf_map_lookup_elem(canary_map_fd, &key, &value) == 0);

    // The program should have been called MAX_TAIL_CALL_CNT + 1 times.
    REQUIRE(value == MAX_TAIL_CALL_CNT + 1);

    // Close the map and programs.
    bpf_object__close(object);
}

TEST_CASE("sequential_tail_call", "[libbpf]")
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();
    struct bpf_object* object = bpf_object__open("tail_call_sequential_um.dll");
    REQUIRE(object != nullptr);

    // Load the BPF program.
    REQUIRE(bpf_object__load(object) == 0);

    // Get the map used to store the next program to call.
    struct bpf_map* map = bpf_object__find_map_by_name(object, "map");
    REQUIRE(map != nullptr);

    // Get the map used to record the number of times the program was called.
    struct bpf_map* canary_map = bpf_object__find_map_by_name(object, "canary");
    REQUIRE(canary_map != nullptr);

    // Get the fd of the map.
    fd_t map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    // Get the fd of the canary map.
    fd_t canary_map_fd = bpf_map__fd(canary_map);
    REQUIRE(canary_map_fd > 0);

    struct bpf_program* program = bpf_object__find_program_by_name(object, "sequential0");
    REQUIRE(program);
    // Get the fd for the program.
    fd_t program_fd = bpf_program__fd(program);
    REQUIRE(program_fd > 0);

    // Invoke the first program in the chain.
    bpf_test_run_opts opts = {};
    sample_program_context_t in_ctx{0};
    sample_program_context_t out_ctx{0};
    opts.repeat = 1;
    opts.ctx_in = reinterpret_cast<uint8_t*>(&in_ctx);
    opts.ctx_size_in = sizeof(in_ctx);
    opts.ctx_out = reinterpret_cast<uint8_t*>(&out_ctx);
    opts.ctx_size_out = sizeof(out_ctx);

    capture_helper_t capture;
    std::vector<std::string> output;
    errno_t error = capture.begin_capture();
    if (error == NO_ERROR) {
        // Run the program.
        usersim_trace_logging_set_enabled(true, EBPF_TRACELOG_LEVEL_INFO, EBPF_TRACELOG_KEYWORD_PRINTK);
        int result = bpf_prog_test_run_opts(program_fd, &opts);
        usersim_trace_logging_set_enabled(false, 0, 0);

        output = capture.buffer_to_printk_vector(capture.get_stdout_contents());
        REQUIRE(result == 0);
    }

    // Verify that the printk output is correct.
    // In 'sequential' ebpf program, the printk is added even to the caller.
    // Hence the printk count is called MAX_TAIL_CALL_CNT + 1 times.
    REQUIRE(output.size() == MAX_TAIL_CALL_CNT + 1);
    for (size_t i = 0; i < MAX_TAIL_CALL_CNT + 1; i++) {
        REQUIRE(output[i] == std::format("sequential{}: *value={}", i, i));
    }

    // Read the map to determine how many times the program was called.
    uint32_t key = 0;
    uint32_t value = 0;
    REQUIRE(bpf_map_lookup_elem(canary_map_fd, &key, &value) == 0);

    // The program should have been called MAX_TAIL_CALL_CNT times.
    REQUIRE(value == MAX_TAIL_CALL_CNT + 1);

    // The last program should have returned -EBPF_NO_MORE_TAIL_CALLS.
    REQUIRE(opts.retval == -EBPF_NO_MORE_TAIL_CALLS);
}

bind_action_t
emulate_bind_tail_call(std::function<ebpf_result_t(void*, uint32_t*)>& invoke, uint64_t pid, const char* appid)
{
    uint32_t result;
    std::string app_id = appid;
    INITIALIZE_BIND_CONTEXT
    ctx->app_id_start = (uint8_t*)app_id.c_str();
    ctx->app_id_end = (uint8_t*)(app_id.c_str()) + app_id.size();
    ctx->process_id = pid;
    ctx->operation = BIND_OPERATION_BIND;

    REQUIRE(invoke(reinterpret_cast<void*>(ctx), &result) == EBPF_SUCCESS);

    return static_cast<bind_action_t>(result);
}

TEST_CASE("bind_tail_call_max_exceed", "[libbpf]")
{
    const int TOTAL_TAIL_CALL = MAX_TAIL_CALL_CNT + 2;

    _test_helper_end_to_end test_helper;
    test_helper.initialize();

    program_info_provider_t bind_program_info;
    REQUIRE(bind_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);

    struct bpf_object* object = bpf_object__open("tail_call_max_exceed_um.dll");
    REQUIRE(object != nullptr);

    // Load the BPF program.
    REQUIRE(bpf_object__load(object) == 0);

    // Get the map used to store the next program to call.
    struct bpf_map* map = bpf_object__find_map_by_name(object, "bind_tail_call_map");
    REQUIRE(map != nullptr);

    // Get the fd of the prog array map.
    fd_t map_fd = bpf_map__fd(map);
    REQUIRE(map_fd > 0);

    std::string first_program_name{"bind_test_caller"};
    struct bpf_program* first_program = bpf_object__find_program_by_name(object, first_program_name.c_str());
    REQUIRE(first_program != nullptr);

    fd_t first_program_fd = bpf_program__fd(first_program);
    REQUIRE(first_program_fd > 0);

    // Verify that the prog_array map contains the correct number of TOTAL_TAIL_CALL programs.
    uint32_t key = 0;
    uint32_t val = 0;
    bpf_map_lookup_elem(map_fd, &key, &val);
    for (int x = 0; x < TOTAL_TAIL_CALL - 1; x++) {
        REQUIRE(bpf_map_get_next_key(map_fd, &key, &key) == 0);
        uint32_t value = 0;
        bpf_map_lookup_elem(map_fd, &key, &value);
        REQUIRE(key != 0);
    }
    REQUIRE(bpf_map_get_next_key(map_fd, &key, &key) < 0);
    REQUIRE(errno == ENOENT);

    // Create a hook for the bind program.
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);

    // Attach the hook.
    bpf_link_ptr link;
    uint32_t ifindex = 0;
    uint64_t fake_pid = 123456;
    REQUIRE(hook.attach_link(first_program_fd, &ifindex, sizeof(ifindex), &link) == EBPF_SUCCESS);

    std::function<ebpf_result_t(void*, uint32_t*)> invoke =
        [&hook](_Inout_ void* context, _Out_ uint32_t* result) -> ebpf_result_t { return hook.fire(context, result); };

    // Binding to the port should be denied because the number of tail call programs exceeds MAX_TAIL_CALL_COUNT.
    REQUIRE(emulate_bind_tail_call(invoke, fake_pid, "fake_app_1") == BIND_DENY);

    hook.detach_and_close_link(&link);
}

void
_test_batch_iteration_maps(
    fd_t& map_fd, uint32_t batch_size, bpf_map_batch_opts* opts, size_t value_size, int num_of_cpus)
{
    // Retrieve in batches.
    const uint8_t batch_size_divisor = 10;
    uint32_t* in_batch = nullptr;
    uint32_t out_batch = 0;
    std::vector<uint32_t> returned_keys(0);
    std::vector<uint64_t> returned_values(0);
    int32_t requested_batch_size = (batch_size / batch_size_divisor) - 2;
    uint32_t batch_size_count = 0;
    int result = 0;

    if (requested_batch_size <= 0) {
        requested_batch_size = 1;
    }

    for (;;) {
        std::vector<uint32_t> batch_keys(requested_batch_size, 0);
        std::vector<uint64_t> batch_values(requested_batch_size * value_size, 0);

        batch_size_count = static_cast<uint32_t>(batch_keys.size());
        result = bpf_map_lookup_batch(
            map_fd, in_batch, &out_batch, batch_keys.data(), batch_values.data(), &batch_size_count, opts);
        if (result == -ENOENT) {
            printf("No more entries. End of map reached.\n");
            break;
        }

        REQUIRE(result == 0);
        // Number of entries retrieved (batch_size_count) should be less than or equal to requested_batch_size.
        REQUIRE(batch_size_count <= static_cast<uint32_t>(batch_keys.size()));
        REQUIRE(batch_size_count <= static_cast<uint32_t>(batch_values.size()) / value_size);

        batch_keys.resize(batch_size_count);
        batch_values.resize(batch_size_count * value_size);
        returned_keys.insert(returned_keys.end(), batch_keys.begin(), batch_keys.end());
        returned_values.insert(returned_values.end(), batch_values.begin(), batch_values.end());

        in_batch = &out_batch;
    }

    REQUIRE(returned_keys.size() == batch_size);
    REQUIRE(returned_values.size() == batch_size * value_size);

    // Verify the returned keys and values.
    uint32_t key = 0;
    for (uint32_t i = 0; i < batch_size; i++) {
        key = returned_keys[i];
        for (int cpu = 0; cpu < num_of_cpus; cpu++) {
            REQUIRE(returned_values[(i * value_size) + cpu] == static_cast<uint64_t>(key) * 2ul);
        }
    }
    // Hash maps do not guarantee order of keys.
    // The keys in the returned_keys vector should be in the range [0, batch_size-1].
    std::sort(returned_keys.begin(), returned_keys.end());
    for (uint32_t i = 0; i < batch_size; i++) {
        REQUIRE(returned_keys[i] == i);
    }
}

void
_test_maps_batch(bpf_map_type map_type)
{
    _test_helper_end_to_end test_helper;
    test_helper.initialize();
    int num_of_cpus = 1;
    size_t value_size = 1;

    if (BPF_MAP_TYPE_PER_CPU(map_type)) {
        // Get the number of possible CPUs that the host kernel supports and expects.
        num_of_cpus = libbpf_num_possible_cpus();
        REQUIRE(num_of_cpus > 0);

        // The value size is the size of the value multiplied by the number of CPUs.
        // Also, the value size should be closest 8-byte aligned.
        //       value_size = EBPF_PAD_8(sizeof(uint8_t)) * num_of_cpus
        //
        // If you use vector<uint8_t> to initialize the values, then use
        //       value_size = EBPF_PAD_8(sizeof(uint8_t)) * num_of_cpus
        //       vector<uint8_t> values(batch_size * value_size);
        //
        // In this test case, we use vector<uint64_t> to initialize the values, which is already 8-byte aligned.
        value_size = num_of_cpus;
    }

    // Create a hash map.
    union bpf_attr attr = {};
    attr.map_create.map_type = map_type;
    attr.map_create.key_size = sizeof(uint32_t);
    attr.map_create.value_size = sizeof(uint64_t);
    attr.map_create.max_entries = 1024 * 1024;

    fd_t map_fd = bpf(BPF_MAP_CREATE, &attr, sizeof(attr));
    REQUIRE(map_fd > 0);

    uint32_t batch_size = 20000;
    std::vector<uint32_t> keys(batch_size);
    std::vector<uint64_t> values(batch_size * value_size);
    for (uint32_t i = 0; i < batch_size; i++) {
        keys[i] = i;
        // Populate the value.
        for (int cpu = 0; cpu < num_of_cpus; cpu++) {
            values[(i * value_size) + cpu] = static_cast<uint64_t>(i) * 2ul;
        }
    }

    // Update the map with the batch.
    bpf_map_batch_opts opts = {.elem_flags = BPF_NOEXIST};

    uint32_t update_batch_size = batch_size;

    // Insert keys in batch.
    REQUIRE(bpf_map_update_batch(map_fd, keys.data(), values.data(), &update_batch_size, &opts) == 0);
    REQUIRE(update_batch_size == batch_size);

    // Fetch the batch.
    uint32_t fetched_batch_size = batch_size;
    std::vector<uint32_t> fetched_keys(batch_size);
    std::vector<uint64_t> fetched_values(batch_size * value_size);
    uint32_t next_key = 0;
    opts.elem_flags = 0;

    // Fetch all keys in one batch.
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) == 0);
    REQUIRE(fetched_batch_size == batch_size);
    _test_batch_iteration_maps(map_fd, batch_size, &opts, value_size, num_of_cpus);

    // Request more keys than present.
    uint32_t large_fetched_batch_size = fetched_batch_size * 2;
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &large_fetched_batch_size, &opts) ==
        0);
    REQUIRE(large_fetched_batch_size == batch_size);

    // Search at end of map.
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd,
            &next_key,
            &next_key,
            fetched_keys.data(),
            fetched_values.data(),
            &large_fetched_batch_size,
            &opts) == -ENOENT);

    // Verify all keys and values in batches.
    _test_batch_iteration_maps(map_fd, batch_size, &opts, value_size, num_of_cpus);

    // Delete all keys in one batch.
    uint32_t delete_batch_size = batch_size;
    opts.elem_flags = 0;
    REQUIRE(bpf_map_delete_batch(map_fd, keys.data(), &delete_batch_size, &opts) == 0);
    REQUIRE(delete_batch_size == batch_size);

    // Verify there are no entries, after deletion.
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) ==
        -ENOENT);

    // Lookup and Delete batch operation.
    opts.elem_flags = BPF_NOEXIST;
    update_batch_size = batch_size;

    // Populate the map with the keys and values.
    REQUIRE(bpf_map_update_batch(map_fd, keys.data(), values.data(), &update_batch_size, &opts) == 0);
    REQUIRE(update_batch_size == batch_size);

    next_key = 0;
    opts.elem_flags = BPF_ANY;
    fetched_batch_size = batch_size;
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) == 0);
    REQUIRE(fetched_batch_size == batch_size);
    _test_batch_iteration_maps(map_fd, batch_size, &opts, value_size, num_of_cpus);
    // Verify the fetched_keys and fetched_values are returned correctly.
    std::sort(fetched_keys.begin(), fetched_keys.end());
    REQUIRE(fetched_keys == keys);
    std::sort(fetched_values.begin(), fetched_values.end());
    REQUIRE(fetched_values == values);

    next_key = 0;
    REQUIRE(
        bpf_map_lookup_and_delete_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) == 0);
    REQUIRE(fetched_batch_size == batch_size);
    REQUIRE(next_key == 0);
    // Verify the fetched_keys and fetched_values are returned correctly.
    std::sort(fetched_keys.begin(), fetched_keys.end());
    REQUIRE(fetched_keys == keys);
    std::sort(fetched_values.begin(), fetched_values.end());
    REQUIRE(fetched_values == values);

    // Verify there are no entries, after deletion.
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) ==
        -ENOENT);

    // Negative tests.
    // Batch size 0

    update_batch_size = 0;
    REQUIRE(bpf_map_update_batch(map_fd, keys.data(), values.data(), &update_batch_size, &opts) == -EINVAL);

    fetched_batch_size = 0;
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) ==
        -EINVAL);

    delete_batch_size = 0;
    REQUIRE(bpf_map_delete_batch(map_fd, keys.data(), &delete_batch_size, &opts) == -EINVAL);

    // opts.flags has invalid value.
    opts.flags = 0x100;
    update_batch_size = batch_size;
    REQUIRE(bpf_map_update_batch(map_fd, keys.data(), values.data(), &update_batch_size, &opts) == -EINVAL);

    fetched_batch_size = batch_size;
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) ==
        -EINVAL);

    delete_batch_size = batch_size;
    REQUIRE(bpf_map_delete_batch(map_fd, keys.data(), &delete_batch_size, &opts) == -EINVAL);

    // opts.elem_flags has invalid value.
    opts.elem_flags = 0x100;
    opts.flags = 0;
    update_batch_size = batch_size;
    REQUIRE(bpf_map_update_batch(map_fd, keys.data(), values.data(), &update_batch_size, &opts) == -EINVAL);

    fetched_batch_size = batch_size;
    REQUIRE(
        bpf_map_lookup_batch(
            map_fd, nullptr, &next_key, fetched_keys.data(), fetched_values.data(), &fetched_batch_size, &opts) ==
        -EINVAL);

    delete_batch_size = batch_size;
    REQUIRE(bpf_map_delete_batch(map_fd, keys.data(), &delete_batch_size, &opts) == -EINVAL);

    // invalid map fd.
    fd_t invalid_map_fd = 0x10000000;

    opts.flags = 0;
    opts.elem_flags = 0;
    update_batch_size = batch_size;

    REQUIRE(bpf_map_update_batch(invalid_map_fd, keys.data(), values.data(), &update_batch_size, &opts) == -EBADF);

    fetched_batch_size = batch_size;
    REQUIRE(
        bpf_map_lookup_batch(
            invalid_map_fd,
            nullptr,
            &next_key,
            fetched_keys.data(),
            fetched_values.data(),
            &fetched_batch_size,
            &opts) == -EBADF);

    delete_batch_size = batch_size;
    REQUIRE(bpf_map_delete_batch(invalid_map_fd, keys.data(), &delete_batch_size, &opts) == -EBADF);
}

TEST_CASE("libbpf hash map batch", "[libbpf]") { _test_maps_batch(BPF_MAP_TYPE_HASH); }

TEST_CASE("libbpf lru hash map batch", "[libbpf]") { _test_maps_batch(BPF_MAP_TYPE_LRU_HASH); }

TEST_CASE("libbpf percpu hash map batch", "[libbpf]") { _test_maps_batch(BPF_MAP_TYPE_PERCPU_HASH); }

TEST_CASE("libbpf lru percpu hash map batch", "[libbpf]") { _test_maps_batch(BPF_MAP_TYPE_LRU_PERCPU_HASH); }

void
_hash_of_map_initial_value_test(ebpf_execution_type_t execution_type)
{
    _test_helper_libbpf test_helper;
    test_helper.initialize();

    std::string file_name = std::format("hash_of_map{}", execution_type == EBPF_EXECUTION_NATIVE ? "_um.dll" : ".o");

    bpf_object_ptr object(bpf_object__open(file_name.c_str()));
    REQUIRE(object != nullptr);

    REQUIRE(ebpf_object_set_execution_type(object.get(), execution_type) == EBPF_SUCCESS);

    // Load the BPF program.
    REQUIRE(bpf_object__load(object.get()) == 0);

    // Get the outer map.
    bpf_map* outer_map = bpf_object__find_map_by_name(object.get(), "outer_map");
    REQUIRE(outer_map != nullptr);

    bpf_map* inner_map = bpf_object__find_map_by_name(object.get(), "inner_map");
    REQUIRE(inner_map != nullptr);

    fd_t outer_map_fd = bpf_map__fd(outer_map);
    REQUIRE(outer_map_fd > 0);

    fd_t inner_map_fd = bpf_map__fd(inner_map);
    REQUIRE(inner_map_fd > 0);

    // Issue: https://github.com/microsoft/ebpf-for-windows/issues/3210
    // Only native execution supports map of maps with static initializers.
    if (execution_type != EBPF_EXECUTION_NATIVE) {
        return;
    }

    uint32_t key = 0;
    uint32_t inner_map_id = 0;

    // Get the map at index 0.
    REQUIRE(bpf_map_lookup_elem(outer_map_fd, &key, &inner_map_id) == 0);

    // Get id of the inner map.
    bpf_map_info info;
    uint32_t info_length = sizeof(info);
    memset(&info, 0, sizeof(info));
    REQUIRE(bpf_obj_get_info_by_fd(inner_map_fd, &info, &info_length) == 0);

    // Verify that the id of the inner map matches the id in the outer map.
    REQUIRE(inner_map_id == info.id);

    // Lookup in the inner map with a non-zero key should fail.
    uint32_t non_zero_key = 1;
    uint32_t value = 0;
    int result = bpf_map_lookup_elem(inner_map_fd, &non_zero_key, &value);
    REQUIRE(result != 0);
    REQUIRE(errno == ENOENT);

    // Lookup in the inner map with a clearly nonexistent key should also fail.
    uint32_t nonexistent_key = 12345;
    result = bpf_map_lookup_elem(inner_map_fd, &nonexistent_key, &value);
    REQUIRE(result != 0);
    REQUIRE(errno == ENOENT);
}

TEST_CASE("hash_of_map", "[libbpf]")
{
#if !defined(CONFIG_BPF_JIT_DISABLED)
    _hash_of_map_initial_value_test(EBPF_EXECUTION_JIT);

#endif
    _hash_of_map_initial_value_test(EBPF_EXECUTION_NATIVE);
}

static void
_utility_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    const char dll_name[] = "utility_um.dll";
    const char obj_name[] = "utility.o";
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? dll_name : obj_name);
    struct bpf_object* process_object = bpf_object__open(file_name);
    REQUIRE(process_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(process_object) == 0);

    struct bpf_program* caller = bpf_object__find_program_by_name(process_object, "UtilityTest");
    REQUIRE(caller != nullptr);

    bpf_link_ptr link(bpf_program__attach(caller));
    REQUIRE(link != nullptr);

    // Now run the ebpf program.
    INITIALIZE_BIND_CONTEXT
    ctx->operation = BIND_OPERATION_BIND;

    uint32_t result;
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);

    // Verify the result.
    REQUIRE(result == 0);

    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);

    bpf_object__close(process_object);
}

DECLARE_ALL_TEST_CASES("utility_test", "[libbpf]", _utility_test);

static void
_strings_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    const char dll_name[] = "strings_um.dll";
    const char obj_name[] = "strings.o";
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? dll_name : obj_name);
    struct bpf_object* process_object = bpf_object__open(file_name);
    REQUIRE(process_object != nullptr);

    // Load the program(s).
    REQUIRE(bpf_object__load(process_object) == 0);

    struct bpf_program* string_caller = bpf_object__find_program_by_name(process_object, "StringOpsTest");
    REQUIRE(string_caller != nullptr);

    bpf_link_ptr string_link(bpf_program__attach(string_caller));
    REQUIRE(string_link != nullptr);

    // Now run the ebpf program.
    INITIALIZE_BIND_CONTEXT
    ctx->operation = BIND_OPERATION_BIND;

    uint32_t result{};
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);

    REQUIRE(result == 0);

    result = bpf_link__destroy(string_link.release());
    REQUIRE(result == 0);

    bpf_object__close(process_object);
}
DECLARE_ALL_TEST_CASES("strings_test", "[libbpf]", _strings_test);

static void
_program_flags_test(ebpf_execution_type_t execution_type)
{
    _test_helper_end_to_end test_helper;
    const char dll_name[] = "utility_um.dll";
    const char obj_name[] = "utility.o";
    test_helper.initialize();
    single_instance_hook_t hook(EBPF_PROGRAM_TYPE_BIND, EBPF_ATTACH_TYPE_BIND);
    REQUIRE(hook.initialize() == EBPF_SUCCESS);
    program_info_provider_t sample_program_info;
    REQUIRE(sample_program_info.initialize(EBPF_PROGRAM_TYPE_BIND) == EBPF_SUCCESS);

    const char* file_name = (execution_type == EBPF_EXECUTION_NATIVE ? dll_name : obj_name);
    struct bpf_object* process_object = bpf_object__open(file_name);
    REQUIRE(process_object != nullptr);

    // Set the flag on each program in the object.
    struct bpf_program* program;
    bpf_object__for_each_program(program, process_object)
    {
        // Set some flags on the program. The test attach provider does not use these flags.
        REQUIRE(bpf_program__set_flags(program, 0xCCCCCCCC) == 0);

        // Get the flags and verify they are correct.
        REQUIRE(bpf_program__flags(program) == 0xCCCCCCCC);
    }

    // Load the program(s).
    REQUIRE(bpf_object__load(process_object) == 0);

    // Verify that setting the flag after load fails.
    bpf_object__for_each_program(program, process_object)
    {
        REQUIRE(bpf_program__set_flags(program, 0xCCCCCCCC) == -EBUSY);
    }

    struct bpf_program* caller = bpf_object__find_program_by_name(process_object, "UtilityTest");
    REQUIRE(caller != nullptr);

    bpf_link_ptr link(bpf_program__attach(caller));
    REQUIRE(link != nullptr);

    auto client_data = hook.get_client_data();

    REQUIRE(client_data != nullptr);
    REQUIRE(client_data->header.version == EBPF_ATTACH_CLIENT_DATA_CURRENT_VERSION);
    REQUIRE(client_data->prog_attach_flags == 0xCCCCCCCC);

    // Now run the ebpf program.
    INITIALIZE_BIND_CONTEXT
    ctx->operation = BIND_OPERATION_BIND;

    uint32_t result;
    REQUIRE(hook.fire(ctx, &result) == EBPF_SUCCESS);

    // Verify the result.
    REQUIRE(result == 0);

    result = bpf_link__destroy(link.release());
    REQUIRE(result == 0);
    bpf_object__close(process_object);
}

DECLARE_ALL_TEST_CASES("program_flag_test", "[libbpf]", _program_flags_test);