#include <Async/File.hpp>
#include <Async/IoHandler.hpp>
#include <Async/ParIter.hpp>
#include <Async/Scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

int main() {
    using namespace SFT::Async;
    using namespace std::chrono_literals;

    if (IoHandler::configure(IoHandlerConfig{2, 0}) ||
        !IoHandler::configure(IoHandlerConfig{2, 2})) {
        std::cerr << "IO handler configuration validation failed\n";
        return 1;
    }

    Scheduler::initialize(1);
    const std::thread::id scheduler_thread = Scheduler::spawn([] { return std::this_thread::get_id(); }).wait();
    const std::thread::id io_thread = IoHandler::submit([] { return std::this_thread::get_id(); }).wait();
    const bool worker_shutdown_was_rejected = IoHandler::submit([] { return !IoHandler::shutdown(); }).wait();
    if (scheduler_thread == io_thread || !worker_shutdown_was_rejected ||
        IoHandler::backend() != IoBackend::BlockingThreadPool) {
        std::cerr << "IO handler backend or execution lane is incorrect\n";
        Scheduler::shutdown();
        return 1;
    }

    bool exception_propagated = false;
    try {
        IoHandler::submit([]() -> int { throw std::runtime_error("expected IO task error"); }).wait();
    } catch (const std::runtime_error &error) {
        exception_propagated = std::string_view(error.what()) == "expected IO task error";
    }
    if (!exception_propagated) {
        std::cerr << "IO task exception did not propagate through TaskHandle::wait\n";
        Scheduler::shutdown();
        return 1;
    }
    const int nested_result = IoHandler::submit([] {
        if (!IoHandler::is_worker_thread()) {
            return -1;
        }
        return IoHandler::submit([] { return 73; }).wait();
    }).wait();
    if (nested_result != 73) {
        std::cerr << "nested IO submission failed on an IO worker\n";
        Scheduler::shutdown();
        return 1;
    }

    CancellationSource running_cancellation;
    std::atomic<bool> running_callback_started{false};
    auto cooperative_operation = IoHandler::submit(
        [&running_callback_started](CancellationToken token) {
            running_callback_started.store(true, std::memory_order_release);
            running_callback_started.notify_all();
            while (!token.is_cancelled()) {
                std::this_thread::yield();
            }
            token.throw_if_cancelled();
        },
        running_cancellation.token());
    const auto cancellation_deadline = std::chrono::steady_clock::now() + 5s;
    while (!running_callback_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < cancellation_deadline) {
        std::this_thread::yield();
    }
    (void)running_cancellation.cancel();
    bool running_cancellation_propagated = false;
    try {
        cooperative_operation.wait();
    } catch (const IoOperationCancelled &) {
        running_cancellation_propagated = true;
    }
    if (!running_cancellation_propagated) {
        std::cerr << "cooperative cancellation did not reach a running IO callback\n";
        Scheduler::shutdown();
        return 1;
    }

    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    unsigned started = 0;
    bool release_io = false;
    const unsigned io_workers = IoHandler::worker_count();
    if (io_workers != 2 || IoHandler::pending_capacity() != 2) {
        std::cerr << "configured IO worker or queue limits were not applied\n";
        Scheduler::shutdown();
        return 1;
    }
    std::vector<TaskHandle<void>> blocked_io;
    blocked_io.reserve(io_workers);
    for (unsigned i = 0; i < io_workers; ++i) {
        blocked_io.push_back(IoHandler::submit([&] {
            std::unique_lock lock(gate_mutex);
            ++started;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return release_io; });
        }));
    }

    {
        std::unique_lock lock(gate_mutex);
        if (!gate_cv.wait_for(lock, 5s, [&] { return started == io_workers; })) {
            release_io = true;
            lock.unlock();
            gate_cv.notify_all();
            for (const auto &task : blocked_io) task.wait();
            Scheduler::shutdown();
            std::cerr << "IO fallback workers did not all become active\n";
            return 1;
        }
    }

    CancellationSource queued_cancellation;
    std::atomic<bool> cancelled_callback_ran{false};
    auto queued_io = IoHandler::submit([&] { cancelled_callback_ran.store(true, std::memory_order_release); },
                                       queued_cancellation.token());
    auto second_queued_io = IoHandler::submit([] {});
    bool rejected_overflow = false;
    try {
        (void)IoHandler::submit([] {});
    } catch (const IoQueueFull &) {
        rejected_overflow = true;
    }
    if (!rejected_overflow || IoHandler::pending_count() != 2) {
        {
            std::lock_guard lock(gate_mutex);
            release_io = true;
        }
        gate_cv.notify_all();
        for (const auto &task : blocked_io) task.wait();
        queued_io.wait();
        second_queued_io.wait();
        Scheduler::shutdown();
        std::cerr << "IO queue limit did not reject overflow cleanly\n";
        return 1;
    }

    std::mutex scheduler_mutex;
    std::condition_variable scheduler_cv;
    bool scheduler_ran = false;
    auto cpu_task = Scheduler::spawn([&] {
        {
            std::lock_guard lock(scheduler_mutex);
            scheduler_ran = true;
        }
        scheduler_cv.notify_one();
    });
    bool scheduler_progressed = false;
    {
        std::unique_lock lock(scheduler_mutex);
        scheduler_progressed = scheduler_cv.wait_for(lock, 5s, [&] { return scheduler_ran; });
    }

    (void)queued_cancellation.cancel();
    {
        std::lock_guard lock(gate_mutex);
        release_io = true;
    }
    gate_cv.notify_all();
    for (const auto &task : blocked_io) task.wait();
    bool cancellation_propagated = false;
    try {
        queued_io.wait();
    } catch (const IoOperationCancelled &) {
        cancellation_propagated = true;
    }
    second_queued_io.wait();
    cpu_task.wait();
    if (!cancellation_propagated || cancelled_callback_ran.load(std::memory_order_acquire)) {
        std::cerr << "queued cancellation did not skip the operation and notify its waiter\n";
        Scheduler::shutdown();
        return 1;
    }
    Scheduler::shutdown();
    if (!scheduler_progressed) {
        std::cerr << "blocked IO workers starved the CPU scheduler\n";
        return 1;
    }

    const auto file_path = std::filesystem::temp_directory_path() / "sturdy-async-io-handler-test.bin";
    const std::vector<std::byte> expected{std::byte{0x53}, std::byte{0x46}, std::byte{0x54}};
    auto write_result = write_file(file_path.string(), expected).wait();
    auto size_result = file_size(file_path.string()).wait();
    auto range_result = read_file_range(file_path.string(), 1, 2).wait();
    auto eof_range_result = read_file_range(file_path.string(), 1, 8).wait();
    auto read_result = read_file(file_path.string()).wait();
    auto limited_read = read_file(file_path.string(), expected.size() - 1).wait();
    const auto missing_size = file_size((file_path.string() + ".missing")).wait();
    const bool read_limit_enforced = !limited_read && limited_read.error().code == IoErrorCode::InvalidArgument;
    std::error_code ignored;
    std::filesystem::remove(file_path, ignored);
    const std::vector<std::byte> expected_range{expected[1], expected[2]};
    if (!write_result || !size_result || *size_result != expected.size() || !range_result || *range_result != expected_range ||
        !eof_range_result || *eof_range_result != expected_range || !read_result || *read_result != expected ||
        missing_size || !read_limit_enforced) {
        std::cerr << "file IO did not complete through the IO handler path\n";
        return 1;
    }
    const auto fs_root = std::filesystem::temp_directory_path() / "sturdy-async-fs-operations-test";
    const auto fs_source = fs_root / "source.bin";
    const auto fs_renamed = fs_root / "renamed.bin";
    const auto fs_nested = fs_root / "nested";
    const auto fs_copy = fs_nested / "copy.bin";
    const std::vector<std::byte> appended{std::byte{0x21}};
    auto expected_appended = expected;
    expected_appended.insert(expected_appended.end(), appended.begin(), appended.end());
    const bool directories_created = create_directories(fs_nested.string()).wait().has_value();
    const bool filesystem_write_ok = write_file(fs_source.string(), expected).wait().has_value();
    const bool append_ok = append_file(fs_source.string(), appended).wait().has_value();
    const auto source_exists = file_exists(fs_source.string()).wait();
    const auto root_entries = list_directory(fs_root.string(), 8).wait();
    const bool copy_ok = copy_file(fs_source.string(), fs_copy.string()).wait().has_value();
    const auto recursive_entries = list_directory(fs_root.string(), 8, true).wait();
    const auto copied = read_file(fs_copy.string()).wait();
    const auto nested_entries = list_directory(fs_nested.string(), 1).wait();
    std::size_t iterated_entries = 0;
    for (const auto &entry : list_dir_iter(fs_nested.string())) {
        if (std::filesystem::path(entry).filename() == "copy.bin") ++iterated_entries;
    }
    std::size_t recursive_iterated_entries = 0;
    for (const auto &entry : list_dir_iter(fs_root.string(), true)) {
        (void)entry;
        ++recursive_iterated_entries;
    }
    static_assert(std::ranges::input_range<DirectoryIterator>);
    const auto parallel_directory_count = par_iter(list_dir_iter(fs_root.string(), true)).count();
    auto method_iterator = list_dir_iter(fs_nested.string());
    const auto first_entry = method_iterator.next();
    const auto end_entry = method_iterator.next();
    const bool rename_ok = rename_file(fs_source.string(), fs_renamed.string()).wait().has_value();
    const auto renamed_exists = file_exists(fs_renamed.string()).wait();
    const auto removed_source = remove_file(fs_renamed.string()).wait();
    const auto source_exists_after_remove = file_exists(fs_renamed.string()).wait();
    const auto removed_again = remove_file(fs_renamed.string()).wait();
    const auto removed_copy = remove_file(fs_copy.string()).wait();
    const auto removed_nested = remove_directory(fs_nested.string()).wait();
    std::error_code cleanup_error;
    std::filesystem::remove_all(fs_root, cleanup_error);
    if (!directories_created || !filesystem_write_ok || !append_ok || !source_exists || !*source_exists || !root_entries || !recursive_entries ||
        root_entries->size() != 2 || recursive_entries->size() != 3 || recursive_iterated_entries != 3 ||
                parallel_directory_count != 3 || !copy_ok || !copied || *copied != expected_appended || !nested_entries ||
        nested_entries->size() != 1 || iterated_entries != 1 || !first_entry || !*first_entry ||
        std::filesystem::path(**first_entry).filename() != "copy.bin" || !end_entry || *end_entry ||
        !rename_ok || !renamed_exists || !*renamed_exists || !removed_source ||
        !*removed_source || !source_exists_after_remove || *source_exists_after_remove || !removed_again || *removed_again ||
        !removed_copy || !*removed_copy || !removed_nested || !*removed_nested) {
        std::cerr << "async filesystem operations returned unexpected results\n";
        return 1;
    }

    CancellationSource file_cancellation;
    (void)file_cancellation.cancel();
    bool file_cancellation_propagated = false;
    try {
        (void)read_file(file_path.string(), file_cancellation.token()).wait();
    } catch (const IoOperationCancelled &) {
        file_cancellation_propagated = true;
    }
    if (!file_cancellation_propagated) {
        std::cerr << "pre-cancelled file read was not cancelled before touching the file\n";
        return 1;
    }

    constexpr unsigned producer_count = 4;
    constexpr unsigned submissions_per_producer = 128;
    std::atomic<unsigned> completed_operations{0};
    std::atomic<unsigned> rejected_operations{0};
    std::mutex accepted_mutex;
    std::vector<TaskHandle<unsigned>> accepted_operations;
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([producer, &completed_operations, &rejected_operations, &accepted_mutex, &accepted_operations] {
            for (unsigned index = 0; index < submissions_per_producer; ++index) {
                try {
                    auto operation = IoHandler::submit([producer, index, &completed_operations] {
                        completed_operations.fetch_add(1, std::memory_order_relaxed);
                        return producer * submissions_per_producer + index;
                    });
                    std::lock_guard lock(accepted_mutex);
                    accepted_operations.push_back(std::move(operation));
                } catch (const IoQueueFull &) {
                    rejected_operations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &producer : producers) producer.join();
    for (const auto &operation : accepted_operations) (void)operation.wait();
    const unsigned total_submissions = producer_count * submissions_per_producer;
    if (accepted_operations.size() + rejected_operations.load(std::memory_order_relaxed) != total_submissions ||
        completed_operations.load(std::memory_order_relaxed) != accepted_operations.size()) {
        std::cerr << "concurrent IO submissions were lost or completed more than once\n";
        return 1;
    }

    std::mutex shutdown_mutex;
    std::condition_variable shutdown_cv;
    unsigned shutdown_workers_started = 0;
    bool release_shutdown_workers = false;
    std::vector<TaskHandle<void>> shutdown_workers;
    for (unsigned i = 0; i < io_workers; ++i) {
        shutdown_workers.push_back(IoHandler::submit([&] {
            std::unique_lock lock(shutdown_mutex);
            ++shutdown_workers_started;
            shutdown_cv.notify_all();
            shutdown_cv.wait(lock, [&] { return release_shutdown_workers; });
        }));
    }
    {
        std::unique_lock lock(shutdown_mutex);
        if (!shutdown_cv.wait_for(lock, 5s, [&] { return shutdown_workers_started == io_workers; })) {
            release_shutdown_workers = true;
            lock.unlock();
            shutdown_cv.notify_all();
            for (const auto &task : shutdown_workers) task.wait();
            std::cerr << "IO workers failed to start for shutdown drain test\n";
            return 1;
        }
    }

    std::atomic<bool> queued_operation_ran{false};
    auto queued_before_shutdown = IoHandler::submit([&] { queued_operation_ran.store(true, std::memory_order_release); });
    if (IoHandler::pending_count() != 1) {
        {
            std::lock_guard lock(shutdown_mutex);
            release_shutdown_workers = true;
        }
        shutdown_cv.notify_all();
        for (const auto &task : shutdown_workers) task.wait();
        queued_before_shutdown.wait();
        std::cerr << "queued operation was not pending before shutdown\n";
        return 1;
    }
    bool shutdown_succeeded = false;
    std::thread shutdown_thread([&] { shutdown_succeeded = IoHandler::shutdown(); });
    {
        std::lock_guard lock(shutdown_mutex);
        release_shutdown_workers = true;
    }
    shutdown_cv.notify_all();
    for (const auto &task : shutdown_workers) task.wait();
    queued_before_shutdown.wait();
    shutdown_thread.join();

    bool rejected_after_shutdown = false;
    try {
        (void)IoHandler::submit([] {});
    } catch (const std::runtime_error &) {
        rejected_after_shutdown = true;
    }
    if (!shutdown_succeeded || !queued_operation_ran.load(std::memory_order_acquire) ||
        !rejected_after_shutdown || !IoHandler::shutdown()) {
        std::cerr << "IO handler shutdown did not drain or reject work as specified\n";
        return 1;
    }
    return 0;
}
