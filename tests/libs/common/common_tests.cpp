// Copyright (c) eBPF for Windows contributors
// SPDX-License-Identifier: MIT

/**
 * @file
 * @brief Common test functions used by end to end and component tests.
 */

#include "catch_wrapper.hpp"
#include "common_tests.h"
#include "platform.h"
#include "sample_test_common.h"

#include <chrono>
#include <future>
#include <map>
using namespace std::chrono_literals;

bool use_ebpf_store = true;

void
ebpf_test_pinned_map_enum(bool verify_pin_path)
{
    int error;
    uint32_t return_value;
    ebpf_result_t result;
    const int pinned_map_count = 10;
    std::string pin_path_prefix = "\\ebpf\\map\\";
    uint16_t map_count = 0;
    ebpf_map_info_t* map_info = nullptr;
    std::map<std::string, std::string> results;

    fd_t map_fd = bpf_map_create(BPF_MAP_TYPE_ARRAY, nullptr, sizeof(uint32_t), sizeof(uint64_t), 1024, nullptr);
    REQUIRE(map_fd >= 0);

    if (map_fd < 0) {
        goto Exit;
    }

    for (int i = 0; i < pinned_map_count; i++) {
        std::string pin_path = pin_path_prefix + std::to_string(i);
        error = bpf_obj_pin(map_fd, pin_path.c_str());
        REQUIRE(error == 0);
        if (error != 0) {
            goto Exit;
        }
    }

    REQUIRE((result = ebpf_api_get_pinned_map_info(&map_count, &map_info)) == EBPF_SUCCESS);
    if (result != EBPF_SUCCESS) {
        goto Exit;
    }

    REQUIRE(map_count == pinned_map_count);
    REQUIRE(map_info != nullptr);
    if (map_info == nullptr) {
        goto Exit;
    }

    _Analysis_assume_(pinned_map_count == map_count);
    for (int i = 0; i < pinned_map_count; i++) {
        bool matched = false;
        std::string pin_path = pin_path_prefix + std::to_string(i);

        if (verify_pin_path) {
            // The canonical file path actually pinned should have the "BPF:" prefix,
            // so should be 4 longer than the non-canonical path.
            REQUIRE(
                (matched =
                     (static_cast<uint16_t>(pin_path.size() + 4) ==
                      strnlen_s(map_info[i].pin_path, EBPF_MAX_PIN_PATH_LENGTH))));
        }
        std::string temp(map_info[i].pin_path);
        results[pin_path] = temp;

        // Unpin the object.
        REQUIRE((return_value = ebpf_object_unpin(pin_path.c_str())) == EBPF_SUCCESS);
    }

    REQUIRE(results.size() == pinned_map_count);
    for (int i = 0; i < pinned_map_count; i++) {
        std::string pin_path = pin_path_prefix + std::to_string(i);
        REQUIRE(results.find(pin_path) != results.end());
    }

Exit:
    Platform::_close(map_fd);
    ebpf_api_map_info_free(map_count, map_info);
    map_count = 0;
    map_info = nullptr;
}

void
verify_utility_helper_results(_In_ const bpf_object* object, bool helper_override)
{
    fd_t utility_map_fd = bpf_object__find_map_fd_by_name(object, "utility_map");
    ebpf_utility_helpers_data_t test_data[UTILITY_MAP_SIZE];
    for (uint32_t key = 0; key < UTILITY_MAP_SIZE; key++) {
        REQUIRE(bpf_map_lookup_elem(utility_map_fd, &key, (void*)&test_data[key]) == EBPF_SUCCESS);
    }

    REQUIRE(test_data[0].random != test_data[1].random);
    REQUIRE(test_data[0].timestamp <= test_data[1].timestamp);
    REQUIRE(test_data[0].boot_timestamp <= test_data[1].boot_timestamp);
    REQUIRE(test_data[0].boot_timestamp_ms <= test_data[1].boot_timestamp_ms);
    REQUIRE(test_data[0].timestamp_ms <= test_data[1].timestamp_ms);
    REQUIRE(
        (test_data[1].boot_timestamp - test_data[0].boot_timestamp) >=
        (test_data[1].timestamp - test_data[0].timestamp));

    if (helper_override) {
        REQUIRE(test_data[0].pid_tgid == SAMPLE_EXT_PID_TGID);
        REQUIRE(test_data[1].pid_tgid == SAMPLE_EXT_PID_TGID);
    } else {
        REQUIRE(test_data[0].pid_tgid != SAMPLE_EXT_PID_TGID);
        REQUIRE(test_data[1].pid_tgid != SAMPLE_EXT_PID_TGID);
    }
}

ring_buffer_test_event_context_t::_ring_buffer_test_event_context()
    : ring_buffer(nullptr), records(nullptr), canceled(false), matched_entry_count(0), test_event_count(0)
{
}
ring_buffer_test_event_context_t::~_ring_buffer_test_event_context()
{
    if (ring_buffer != nullptr) {
        ring_buffer__free(ring_buffer);
    }
}
void
ring_buffer_test_event_context_t::unsubscribe()
{
    struct ring_buffer* temp = ring_buffer;
    ring_buffer = nullptr;
    // Unsubscribe.
    ring_buffer__free(temp);
}

int
ring_buffer_test_event_handler(_Inout_ void* ctx, _In_opt_ const void* data, size_t size)
{
    ring_buffer_test_event_context_t* event_context = reinterpret_cast<ring_buffer_test_event_context_t*>(ctx);

    if ((data == nullptr) || (size == 0)) {
        return 0;
    }

    if (event_context->canceled) {
        // Ignore the callback as the subscription is canceled.
        // Return error so that no further callback is made.
        return -1;
    }

    if (event_context->matched_entry_count == event_context->test_event_count) {
        // Required number of event notifications already received.
        return 0;
    }

    std::vector<char> event_record(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size);

    // Check if indicated event record matches an entry in the context records
    // that has not been matched yet.
    auto records = event_context->records;
    auto it = records->begin();
    for (;;) {
        // Find the next entry in the records vector.
        it = std::find(it, records->end(), event_record);
        if (it == records->end()) {
            // No more entries in the records vector.
            break;
        }

        // Check if the entry has already been matched.
        auto index = std::distance(records->begin(), it);
        if (event_context->event_received.find(index) != event_context->event_received.end()) {
            it++;
            // Entry already matched.
            continue;
        }

        // Mark the entry as matched.
        event_context->event_received.insert(index);
        event_context->matched_entry_count++;
        break;
    }

    if (event_context->matched_entry_count == event_context->test_event_count) {
        // If all the entries in the app ID list was found, fulfill the promise.
        event_context->ring_buffer_event_promise.set_value();
    }
    return 0;
}

void
ring_buffer_api_test_helper(
    fd_t ring_buffer_map, std::vector<std::vector<char>>& expected_records, std::function<void(int)> generate_event)
{
    std::vector<std::vector<char>> records = expected_records;

    // Add a couple of interleaved messages to the records vector.
    std::string message = "Interleaved message #1";
    std::vector<char> record(message.begin(), message.end());
    record.push_back('\0');
    records.push_back(record);
    record[record.size() - 2] = '2';
    records.push_back(record);

    // Ring buffer event callback context.
    std::unique_ptr<ring_buffer_test_event_context_t> context = std::make_unique<ring_buffer_test_event_context_t>();
    context->test_event_count = RING_BUFFER_TEST_EVENT_COUNT + 2;

    context->records = &records;

    // Generate events prior to subscribing for ring buffer events.
    for (int i = 0; i < RING_BUFFER_TEST_EVENT_COUNT / 2; i++) {
        generate_event(i);
    }

    // Write an interleaved message to the ring buffer map.
    REQUIRE(ebpf_ring_buffer_map_write(ring_buffer_map, message.c_str(), message.length() + 1) == EBPF_SUCCESS);

    // Get the std::future from the promise field in ring buffer event context, which should be in ready state
    // once notifications for all events are received.
    auto ring_buffer_event_callback = context->ring_buffer_event_promise.get_future();

    // Create a new ring buffer manager and subscribe to ring buffer events.
    // The notifications for the events that were generated before should occur after the subscribe call.
    context->ring_buffer = ring_buffer__new(
        ring_buffer_map, (ring_buffer_sample_fn)ring_buffer_test_event_handler, context.get(), nullptr);
    REQUIRE(context->ring_buffer != nullptr);

    // Generate more events, post-subscription.
    for (int i = RING_BUFFER_TEST_EVENT_COUNT / 2; i < RING_BUFFER_TEST_EVENT_COUNT; i++) {
        generate_event(i);
    }

    // Write another interleaved message to the ring buffer map.
    message[message.length() - 1] = '2';
    REQUIRE(ebpf_ring_buffer_map_write(ring_buffer_map, message.c_str(), message.length() + 1) == EBPF_SUCCESS);

    // Wait for event handler getting notifications for all RING_BUFFER_TEST_EVENT_COUNT events.
    REQUIRE(ring_buffer_event_callback.wait_for(1s) == std::future_status::ready);

    REQUIRE(context->matched_entry_count == context->test_event_count);

    // Mark the event context as canceled, such that the event callback stops processing events.
    context->canceled = true;
    // Unsubscribe.
    context->unsubscribe();
}

perf_buffer_test_context_t::_perf_buffer_test_context()
    : perf_buffer(nullptr), records(nullptr), canceled(false), matched_entry_count(0), test_event_count(0),
      lost_entry_count(0), doubled_data(false), bad_records(0)
{
}

perf_buffer_test_context_t::~_perf_buffer_test_context()
{
    if (perf_buffer != nullptr) {
        perf_buffer__free(perf_buffer);
    }
}
void
perf_buffer_test_context_t::unsubscribe()
{
    struct perf_buffer* temp = perf_buffer;
    perf_buffer = nullptr;
    // Unsubscribe.
    perf_buffer__free(temp);
}

void
perf_buffer_test_lost_event_handler(void* ctx, int cpu, __u64 cnt)
{
    UNREFERENCED_PARAMETER(cpu);
    perf_buffer_test_context_t* event_context = reinterpret_cast<perf_buffer_test_context_t*>(ctx);
    {
        std::unique_lock<std::mutex> lock(event_context->lock);
        event_context->lost_entry_count += (int)cnt;
    }
}

void
perf_buffer_test_event_handler(_Inout_ void* ctx, int cpu, _In_opt_ const void* data, size_t size)
{
    UNREFERENCED_PARAMETER(cpu);
    perf_buffer_test_context_t* event_context = reinterpret_cast<perf_buffer_test_context_t*>(ctx);

    if ((data == nullptr) || (size == 0)) {
        return;
    }

    if (event_context->canceled) {
        // Ignore the callback as the subscription is canceled.
        // Return error so that no further callback is made.
        return;
    }

    if (event_context->matched_entry_count == event_context->test_event_count) {
        // Required number of event notifications already received.
        return;
    }

    std::vector<char> event_record(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + size);
    {
        std::unique_lock<std::mutex> lock(event_context->lock);

        // If doubled_data is set, the event record should contain the same data twice. We only compare the first half.
        if (event_context->doubled_data) {
            // Check if the event record contains the same data twice.
            size_t half_size = size / 2;
            if (memcmp(data, (char*)data + half_size, half_size) == 0) {
                // The event record contains the same data twice.
                event_record.resize(half_size);
            }
        }

        // Check if indicated event record matches an entry in the context records
        // that has not been matched yet.
        auto records = event_context->records;
        auto it = records->begin();
        for (;;) {
            // Find the next entry in the records vector.
            it = std::find(it, records->end(), event_record);
            if (it == records->end()) {
                // No more entries in the records vector.
                break;
            }

            // Check if the entry has already been matched.
            auto index = std::distance(records->begin(), it);
            if (event_context->event_received.find(index) != event_context->event_received.end()) {
                it++;
                // Entry already matched.
                continue;
            }

            // Mark the entry as matched.
            event_context->event_received.insert(index);
            event_context->matched_entry_count++;
            break;
        }

        if (event_context->matched_entry_count == event_context->test_event_count) {
            // If all the entries in the app ID list was found, fulfill the promise.
            event_context->perf_buffer_event_promise.set_value();
        }
    }
}

void
perf_buffer_api_test_helper(
    fd_t perf_buffer_map,
    std::vector<std::vector<char>>& expected_records,
    std::function<void(int)> generate_event,
    bool doubled_data)
{
    std::vector<std::vector<char>> records = expected_records;

    // Add a couple of interleaved messages to the records vector.
    std::string message = "Interleaved message #1";
    std::vector<char> record(message.begin(), message.end());
    record.push_back('\0');
    records.push_back(record);
    record[record.size() - 2] = '2';
    records.push_back(record);

    // Perf buffer event callback context.
    std::unique_ptr<perf_buffer_test_context_t> context = std::make_unique<perf_buffer_test_context_t>();
    context->test_event_count = PERF_BUFFER_TEST_EVENT_COUNT + 2;
    context->doubled_data = doubled_data;

    context->records = &records;

    // Generate events prior to subscribing for ring buffer events.
    for (int i = 0; i < PERF_BUFFER_TEST_EVENT_COUNT / 2; i++) {
        generate_event(i);
    }

    // Write an interleaved message to the perf buffer map.
    REQUIRE(ebpf_perf_event_array_map_write(perf_buffer_map, message.c_str(), message.length() + 1) == EBPF_SUCCESS);

    // Get the std::future from the promise field in perf buffer event context, which should be in ready state
    // once notifications for all events are received.
    auto perf_buffer_event_callback = context->perf_buffer_event_promise.get_future();

    // Create a new perf buffer manager and subscribe to perf buffer events.
    // The notifications for the events that were generated before should occur after the subscribe call.
    context->perf_buffer = perf_buffer__new(
        perf_buffer_map,
        0,
        (perf_buffer_sample_fn)perf_buffer_test_event_handler,
        (perf_buffer_lost_fn)perf_buffer_test_lost_event_handler,
        context.get(),
        nullptr);
    REQUIRE(context->perf_buffer != nullptr);

    // Generate more events, post-subscription.
    for (int i = PERF_BUFFER_TEST_EVENT_COUNT / 2; i < PERF_BUFFER_TEST_EVENT_COUNT; i++) {
        generate_event(i);
    }

    // Write another interleaved message to the perf buffer map.
    message[message.length() - 1] = '2';
    REQUIRE(ebpf_perf_event_array_map_write(perf_buffer_map, message.c_str(), message.length() + 1) == EBPF_SUCCESS);
    // Wait for event handler getting notifications for all PERF_BUFFER_TEST_EVENT_COUNT events.
    bool test_completed = perf_buffer_event_callback.wait_for(10s) == std::future_status::ready;
    CAPTURE(context->matched_entry_count, context->lost_entry_count, context->test_event_count);
    REQUIRE(test_completed == true);
    REQUIRE((context->matched_entry_count + context->lost_entry_count) == context->test_event_count);

    // Mark the event context as canceled, such that the event callback stops processing events.
    context->canceled = true;

    // Unsubscribe.
    context->unsubscribe();
}