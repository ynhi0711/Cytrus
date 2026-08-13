// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/format-inl.h>

#ifdef _WIN32
#include <share.h>   // For _SH_DENYWR
#include <windows.h> // For OutputDebugStringW
#else
#define _SH_DENYWR 0
#endif

#ifdef CITRA_LINUX_GCC_BACKTRACE
#define BOOST_STACKTRACE_USE_BACKTRACE
#include <boost/stacktrace.hpp>
#undef BOOST_STACKTRACE_USE_BACKTRACE
#include <signal.h>
#endif

#include "common/bounded_threadsafe_queue.h"
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/literals.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/logging/log_entry.h"
#include "common/logging/text_formatter.h"
#include "common/polyfill_thread.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/thread.h"

namespace Common::Log {

namespace {

/**
 * Interface for logging backends.
 */
class Backend {
public:
    virtual ~Backend() = default;

    virtual void Write(const Entry& entry) = 0;

    virtual void EnableForStacktrace() = 0;

    virtual void Flush() = 0;

    virtual void Close() = 0;
};

#ifdef HAVE_LIBRETRO
/**
 * LibRetro backend
 */
class LibRetroBackend : public Backend {
public:
    explicit LibRetroBackend() {}
    explicit LibRetroBackend(retro_log_printf_t callback) : callback(callback) {}

    ~LibRetroBackend() override = default;

    void Write(const Entry& entry) override {
        if (callback == nullptr) {
            return;
        }
        retro_log_level log_level;

        switch (entry.log_level) {
        case Common::Log::Level::Trace:
            log_level = retro_log_level::RETRO_LOG_DEBUG;
            break;
        case Common::Log::Level::Debug:
            log_level = retro_log_level::RETRO_LOG_DEBUG;
            break;
        case Common::Log::Level::Info:
            log_level = retro_log_level::RETRO_LOG_INFO;
            break;
        case Common::Log::Level::Warning:
            log_level = retro_log_level::RETRO_LOG_WARN;
            break;
        case Common::Log::Level::Error:
            log_level = retro_log_level::RETRO_LOG_ERROR;
            break;
        case Common::Log::Level::Critical:
            log_level = retro_log_level::RETRO_LOG_ERROR;
            break;
        default:
            log_level = retro_log_level::RETRO_LOG_DUMMY;
        }

        auto str = FormatLogMessage(entry).append(1, '\n');
        callback(log_level, str.c_str());
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}

private:
    retro_log_printf_t callback = nullptr;
};
#endif

/**
 * Backend that writes to stderr and with color
 */
class ColorConsoleBackend final : public Backend {
public:
    explicit ColorConsoleBackend() = default;

    ~ColorConsoleBackend() override = default;

    void Write(const Entry& entry) override {
        if (enabled.load(std::memory_order_relaxed)) {
            PrintColoredMessage(entry);
        }
    }

    void Flush() override {
        std::fflush(stderr);
    }

    void Close() override {
        enabled = false;
    }

    void EnableForStacktrace() override {
        enabled = true;
    }

    void SetEnabled(bool enabled_) {
        enabled = enabled_;
    }

private:
    std::atomic_bool enabled{false};
};

/**
 * Backend that writes to a file passed into the constructor
 */
class FileBackend final : public Backend {
public:
    explicit FileBackend(const std::string& filename) {
        auto old_filename = filename;
        boost::replace_all(old_filename, ".txt", ".old.txt");

        // Existence checks are done within the functions themselves.
        // We don't particularly care if these succeed or not.
        static_cast<void>(FileUtil::Delete(old_filename));
        static_cast<void>(FileUtil::Rename(filename, old_filename));

        // _SH_DENYWR allows read only access to the file for other programs.
        // It is #defined to 0 on other platforms
        file = std::make_unique<FileUtil::IOFile>(filename, "w", _SH_DENYWR);
    }

    ~FileBackend() override = default;

    void Write(const Entry& entry) override {
        if (!enabled) {
            return;
        }

        bytes_written += file->WriteString(FormatLogMessage(entry).append(1, '\n'));

        using namespace Common::Literals;
        // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
        const auto write_limit = 100_MiB;
        const bool write_limit_exceeded = bytes_written > write_limit;
        if (entry.log_level >= Level::Error || write_limit_exceeded) {
            if (write_limit_exceeded) {
                // xappify fork: say so — a capped log used to just END mid-session, which reads
                // as "the process died here" and has misdirected a diagnosis before. Not added
                // to bytes_written (writes are disabled right after).
                file->WriteString(
                    "cytrus_log truncated at 100 MiB — logging disabled for the rest of the "
                    "session\n");
                // Stop writing after the write limit is exceeded.
                // Don't close the file so we can print a stacktrace if necessary
                enabled = false;
            }
            file->Flush();
        }
    }

    void Flush() override {
        file->Flush();
    }

    void Close() override {
        file->Close();
        enabled = false;
    }

    void EnableForStacktrace() override {
        enabled = true;
        bytes_written = 0;
    }

private:
    std::unique_ptr<FileUtil::IOFile> file;
    bool enabled = true;
    std::size_t bytes_written = 0;
};

/**
 * Backend that writes to Visual Studio's output window
 */
class DebuggerBackend final : public Backend {
public:
    explicit DebuggerBackend() = default;

    ~DebuggerBackend() override = default;

    void Write(const Entry& entry) override {
#ifdef _WIN32
        ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry).append(1, '\n')).c_str());
#endif
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}
};

#ifdef ANDROID
/**
 * Backend that writes to the Android logcat
 */
class LogcatBackend : public Backend {
public:
    explicit LogcatBackend() = default;

    ~LogcatBackend() override = default;

    void Write(const Entry& entry) override {
        PrintMessageToLogcat(entry);
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}
};
#endif

bool initialization_in_progress_suppress_logging = true;
bool logging_initialized = false;

#ifdef CITRA_LINUX_GCC_BACKTRACE
[[noreturn]] void SleepForever() {
    while (true) {
        pause();
    }
}
#endif

/**
 * Static state as a singleton.
 */
class Impl {
public:
    static Impl& Instance() {
        if (!instance) {
            throw std::runtime_error("Using Logging instance before its initialization");
        }
        return *instance;
    }
#ifdef HAVE_LIBRETRO
    static void Initialize(retro_log_printf_t callback) {
        if (instance) {
            LOG_WARNING(Log, "Reinitializing logging backend");
            return;
        }
        initialization_in_progress_suppress_logging = true;
        Filter filter;
        filter.ParseFilterString(Settings::values.log_filter.GetValue());
        instance = std::unique_ptr<Impl, decltype(&Deleter)>(new Impl(callback, filter), Deleter);
        initialization_in_progress_suppress_logging = false;
    }
#endif
    static void Initialize(std::string_view log_file) {
        if (instance) {
            LOG_WARNING(Log, "Reinitializing logging backend");
            return;
        }
        initialization_in_progress_suppress_logging = true;
        const auto& log_dir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
        void(FileUtil::CreateFullPath(log_dir));
        Filter filter;
        filter.ParseFilterString(Settings::values.log_filter.GetValue());
        instance = std::unique_ptr<Impl, decltype(&Deleter)>(
            new Impl(fmt::format("{}{}", log_dir, log_file), filter), Deleter);
        initialization_in_progress_suppress_logging = false;
        logging_initialized = true;
    }

    static void Start() {
        instance->StartBackendThread();
    }

    static void Stop() {
        instance->StopBackendThread();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void SetGlobalFilter(const Filter& f) {
        filter = f;
    }

    bool SetRegexFilter(const std::string& regex) {
        if (regex.empty()) {
            regex_filter = boost::regex();
            return true;
        }
        regex_filter = boost::regex(regex, boost::regex_constants::no_except);
        if (regex_filter.status() != 0) {
            regex_filter = boost::regex();
            return false;
        }
        return true;
    }

    const Filter& GetFilter() const {
        return filter;
    }

    void SetColorConsoleBackendEnabled(bool enabled) {
        color_console_backend.SetEnabled(enabled);
    }

    // xappify fork: guarantee everything logged BEFORE this call is on disk before returning, so
    // a diagnostic burst can be read back from the file immediately.
    //
    // Implemented as a sentinel HANDSHAKE through the backend thread — never by consuming
    // `message_queue` here. The queue is single-consumer (`MPSCQueue`); an earlier version of
    // this function TryPop'd it from caller threads, and two concurrent consumers double-pop
    // until `m_read_index` overruns `m_write_index`, after which the producer-side wait
    // predicate underflows (unsigned) to permanently false and EVERY thread that logs deadlocks
    // behind the queue's write mutex — a device-reproduced total emulator freeze.
    //
    // Bounded: one 2s budget covers both getting the sentinel into a (possibly full) queue and
    // waiting for the backend thread to process it. If the backend thread is stopped or wedged,
    // this returns without the on-disk guarantee instead of wedging the caller.
    //
    // Ticket order is load-bearing: the ticket is taken BEFORE the push, so `flush_completed >=
    // ticket` implies (pigeonhole over FIFO order) that a sentinel pushed no earlier than this
    // caller's log entries has been processed — i.e. those entries were written and a flush ran
    // after them.
    void FlushBackends() {
        const u64 ticket = flush_requested.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        // Ordinary entries may be dropped when the queue is full (see PushEntry), but the
        // sentinel must get IN for the handshake to mean anything — retry briefly. Never a
        // blocking Emplace: after Log::Stop() (reachable from any failed ASSERT) there is no
        // consumer, and a caller stuck here would never finish crashing.
        bool pushed = message_queue.TryEmplace(MakeFlushSentinel());
        while (!pushed && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pushed = message_queue.TryEmplace(MakeFlushSentinel());
        }
        if (!pushed) {
            return;
        }

        std::unique_lock lock{flush_mutex};
        flush_cv.wait_until(lock, deadline, [this, ticket] {
            return flush_completed.load(std::memory_order_relaxed) >= ticket;
        });
    }

    void PushEntry(Class log_class, Level log_level, const char* filename, unsigned int line_num,
                   const char* function, std::string message) {
        Entry new_entry = CreateEntry(log_class, log_level, filename, line_num, function,
                                      std::move(message), time_origin);
        if (!regex_filter.empty() &&
            !boost::regex_search(FormatLogMessage(new_entry), regex_filter)) {
            return;
        }
        if (Settings::values.instant_debug_log.GetValue()) {
            ForEachBackend([&new_entry](Backend& backend) {
                backend.Write(new_entry);
                backend.Flush();
            });
        } else {
            // xappify fork: a full queue DROPS the entry instead of blocking the producer. The
            // old EmplaceWait parked the calling thread until the writer made room — under a
            // log burst that was the EMULATION thread, and a parked producer holds the queue's
            // write mutex, so one saturated burst stalled every logging thread in the process.
            // A dropped log line is strictly better than a frozen emulator; drops are counted
            // and reported on the next successful push.
            if (message_queue.TryEmplace(std::move(new_entry))) {
                if (const u64 dropped = dropped_entries.exchange(0, std::memory_order_relaxed)) {
                    if (!message_queue.TryEmplace(CreateEntry(
                            Class::Log, Level::Warning, __FILE__, __LINE__, __func__,
                            fmt::format("logging dropped {} entries under pressure", dropped),
                            time_origin))) {
                        dropped_entries.fetch_add(dropped, std::memory_order_relaxed);
                    }
                }
            } else {
                dropped_entries.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // xappify fork: marks a FlushBackends handshake sentinel. Distinguishable from both real
    // entries (real line numbers + non-null filename) and a default-constructed Entry left by a
    // stop-token wake (line_num 0).
    static constexpr unsigned int flush_sentinel_line = std::numeric_limits<unsigned int>::max();

    // xappify fork: valid class/level on purpose — if a sentinel ever leaked into a formatter,
    // the name lookups' UNREACHABLE() paths would recurse into logging teardown.
    static Entry MakeFlushSentinel() {
        Entry sentinel{};
        sentinel.log_class = Class::Log;
        sentinel.log_level = Level::Critical;
        sentinel.filename = nullptr;
        sentinel.line_num = flush_sentinel_line;
        return sentinel;
    }

    static Entry CreateEntry(Class log_class, Level log_level, const char* filename,
                             unsigned int line_nr, const char* function, std::string&& message,
                             const std::chrono::steady_clock::time_point& time_origin) {
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        using std::chrono::steady_clock;

        return {
            .timestamp = duration_cast<microseconds>(steady_clock::now() - time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = filename,
            .line_num = line_nr,
            .function = function,
            .message = std::move(message),
        };
    }

private:
#ifdef HAVE_LIBRETRO
    Impl(retro_log_printf_t callback, const Filter& filter_)
        : filter{filter_}, file_backend{""}, libretro_backend{callback} {}
#endif
    Impl(const std::string& file_backend_filename, const Filter& filter_)
        : filter{filter_}, file_backend{file_backend_filename} {
#ifdef CITRA_LINUX_GCC_BACKTRACE
        int waker_pipefd[2];
        int done_printing_pipefd[2];
        if (pipe2(waker_pipefd, O_CLOEXEC) || pipe2(done_printing_pipefd, O_CLOEXEC)) {
            abort();
        }
        backtrace_thread_waker_fd = waker_pipefd[1];
        backtrace_done_printing_fd = done_printing_pipefd[0];
        std::thread([this, wait_fd = waker_pipefd[0], done_fd = done_printing_pipefd[1]] {
            Common::SetCurrentThreadName("citra:Crash");
            for (u8 ignore = 0; read(wait_fd, &ignore, 1) != 1;)
                ;
            const int sig = received_signal;
            if (sig <= 0) {
                abort();
            }
            backend_thread.request_stop();
            backend_thread.join();
            const auto signal_entry = CreateEntry(
                Class::Log, Level::Critical, "?", 0, "?",
                fmt::vformat("Received signal {}", fmt::make_format_args(sig)), time_origin);
            ForEachBackend([&signal_entry](Backend& backend) {
                backend.EnableForStacktrace();
                backend.Write(signal_entry);
            });
            const auto backtrace =
                boost::stacktrace::stacktrace::from_dump(backtrace_storage.data(), 4096);
            for (const auto& frame : backtrace.as_vector()) {
                auto line = boost::stacktrace::detail::to_string(&frame, 1);
                if (line.empty()) {
                    abort();
                }
                line.pop_back(); // Remove newline
                const auto frame_entry = CreateEntry(Class::Log, Level::Critical, "?", 0, "?",
                                                     std::move(line), time_origin);
                ForEachBackend([&frame_entry](Backend& backend) { backend.Write(frame_entry); });
            }
            using namespace std::literals;
            const auto rip_entry =
                CreateEntry(Class::Log, Level::Critical, "?", 0, "?", "RIP"s, time_origin);
            ForEachBackend([&rip_entry](Backend& backend) {
                backend.Write(rip_entry);
                backend.Flush();
            });
            for (const u8 anything = 0; write(done_fd, &anything, 1) != 1;)
                ;
            // Abort on original thread to help debugging
            SleepForever();
        }).detach();
        signal(SIGSEGV, &HandleSignal);
        signal(SIGABRT, &HandleSignal);
#endif
    }

    ~Impl() {
#ifdef CITRA_LINUX_GCC_BACKTRACE
        if (int zero_or_ignore = 0;
            !received_signal.compare_exchange_strong(zero_or_ignore, SIGKILL)) {
            SleepForever();
        }
#endif
    }

    void StartBackendThread() {
        backend_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("citra:Log");
            Entry entry;
            const auto write_logs = [this, &entry]() {
                ForEachBackend([&entry](Backend& backend) { backend.Write(entry); });
            };
            // xappify fork: services a FlushBackends handshake sentinel — flush HERE, on the
            // one thread that owns the backends, then release the waiter. The completion
            // increment happens under flush_mutex so a waiter that just evaluated its
            // predicate can't sleep through the notify.
            const auto complete_flush = [this]() {
                ForEachBackend([](Backend& backend) { backend.Flush(); });
                {
                    std::scoped_lock lk{flush_mutex};
                    flush_completed.fetch_add(1, std::memory_order_relaxed);
                }
                flush_cv.notify_all();
            };
            while (!stop_token.stop_requested()) {
                message_queue.PopWait(entry, stop_token);
                // Only write the log if something was actually popped (entry.filename != nullptr)
                // (for example, when the stop token is signaled).
                if (entry.filename != nullptr) {
                    write_logs();
                } else if (entry.line_num == flush_sentinel_line) {
                    complete_flush();
                }
                // xappify fork: a stop-token wake returns WITHOUT touching `entry`, leaving the
                // previous iteration's value — which used to re-write the last line once at
                // shutdown, and would double-count a stale sentinel and corrupt the flush
                // ticket accounting. Reset every iteration.
                entry = Entry{};
            }
            // Drain the logging queue. Only writes out up to MAX_LOGS_TO_WRITE to prevent a
            // case where a system is repeatedly spamming logs even on close.
            int max_logs_to_write = filter.IsDebug() ? INT_MAX : 100;
            while (max_logs_to_write-- && message_queue.TryPop(entry)) {
                // xappify fork: same guards as the live loop — a sentinel reaching the
                // formatter would pass a null filename into fmt (UB), and signalling drained
                // sentinels releases any waiter early instead of costing them the timeout.
                if (entry.filename != nullptr) {
                    write_logs();
                } else if (entry.line_num == flush_sentinel_line) {
                    complete_flush();
                }
            }
        });
    }

    void StopBackendThread() {
        backend_thread.request_stop();
        if (backend_thread.joinable()) {
            backend_thread.join();
        }

        ForEachBackend([](Backend& backend) {
            backend.Flush();
            backend.Close();
        });
    }

    void ForEachBackend(auto lambda) {
#ifdef HAVE_LIBRETRO
        lambda(static_cast<Backend&>(libretro_backend));
#else
        lambda(static_cast<Backend&>(debugger_backend));
        lambda(static_cast<Backend&>(color_console_backend));
        lambda(static_cast<Backend&>(file_backend));
#ifdef ANDROID
        lambda(static_cast<Backend&>(lc_backend));
#endif // ANDROID
#endif // HAVE_LIBRETRO
    }

    static void Deleter(Impl* ptr) {
        delete ptr;
    }

#ifdef CITRA_LINUX_GCC_BACKTRACE
    [[noreturn]] static void HandleSignal(int sig) {
        signal(SIGABRT, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        if (sig <= 0) {
            abort();
        }
        instance->InstanceHandleSignal(sig);
    }

    [[noreturn]] void InstanceHandleSignal(int sig) {
        if (int zero_or_ignore = 0; !received_signal.compare_exchange_strong(zero_or_ignore, sig)) {
            if (received_signal == SIGKILL) {
                abort();
            }
            SleepForever();
        }
        // Don't restart like boost suggests. We want to append to the log file and not lose dynamic
        // symbols. This may segfault if it unwinds outside C/C++ code but we'll just have to fall
        // back to core dumps.
        boost::stacktrace::safe_dump_to(backtrace_storage.data(), 4096);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (const int anything = 0; write(backtrace_thread_waker_fd, &anything, 1) != 1;)
            ;
        for (u8 ignore = 0; read(backtrace_done_printing_fd, &ignore, 1) != 1;)
            ;
        abort();
    }
#endif

    static inline std::unique_ptr<Impl, decltype(&Deleter)> instance{nullptr, Deleter};

    Filter filter;
    boost::regex regex_filter;
    DebuggerBackend debugger_backend{};
    ColorConsoleBackend color_console_backend{};
    FileBackend file_backend;
#ifdef ANDROID
    LogcatBackend lc_backend{};
#endif
#ifdef HAVE_LIBRETRO
    LibRetroBackend libretro_backend;
#endif

    MPSCQueue<Entry> message_queue{};
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};
    std::jthread backend_thread;

    // xappify fork: FlushBackends handshake + drop accounting — see FlushBackends()/PushEntry().
    std::atomic<u64> flush_requested{0};
    std::atomic<u64> flush_completed{0};
    std::mutex flush_mutex;
    std::condition_variable flush_cv;
    std::atomic<u64> dropped_entries{0};

#ifdef CITRA_LINUX_GCC_BACKTRACE
    std::atomic_int received_signal{0};
    std::array<u8, 4096> backtrace_storage{};
    int backtrace_thread_waker_fd;
    int backtrace_done_printing_fd;
#endif
};
} // namespace

#ifdef HAVE_LIBRETRO
void LibRetroStart(retro_log_printf_t callback) {
    Impl::Initialize(callback);
    Impl::Start();
}
#endif

void Initialize(std::string_view log_file) {
    Impl::Initialize(log_file.empty() ? LOG_FILE : log_file);
}

void Start() {
    Impl::Start();
}

void Stop() {
    Impl::Stop();
}

void DisableLoggingInTests() {
    initialization_in_progress_suppress_logging = true;
}

void SetGlobalFilter(const Filter& filter) {
    Impl::Instance().SetGlobalFilter(filter);
}

bool SetRegexFilter(const std::string& regex) {
    return Impl::Instance().SetRegexFilter(regex);
}

void SetColorConsoleBackendEnabled(bool enabled) {
    Impl::Instance().SetColorConsoleBackendEnabled(enabled);
}

void FlushBackends() {
    // xappify fork: no-op before Initialize — Instance() throws, and callers include core paths
    // that can run in harnesses without logging set up.
    if (!logging_initialized) {
        return;
    }
    Impl::Instance().FlushBackends();
}

void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename,
                       unsigned int line_num, const char* function, fmt::string_view format,
                       const fmt::format_args& args) {
    if (initialization_in_progress_suppress_logging) [[unlikely]] {
        return;
    }

    if (logging_initialized) [[likely]] {
        if (!Impl::Instance().GetFilter().CheckMessage(log_class, log_level)) {
            return;
        }
        Impl::Instance().PushEntry(log_class, log_level, filename, line_num, function,
                                   fmt::vformat(format, args));
    } else {
        // In the rare case that logging occurs before initialization, write the
        // message to stderr to preserve useful debug information.
        Entry new_entry = Impl::CreateEntry(log_class, log_level, filename, line_num, function,
                                            fmt::vformat(format, args), {});
        PrintMessage(new_entry);
    }
}
} // namespace Common::Log
