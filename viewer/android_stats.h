#pragma once
// Polls the emulator (or Waydroid) over adb about once a second: overall Android CPU use and
// the game's busiest threads (from /proc/stat and /proc/<pid>/task/*/stat). With a native process
// name (Refract Native, Windows only) it reads that process and its threads directly instead:
// refract_native names each host thread after its guest thread.
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct ThreadUse {
    std::string name;
    double percent = 0;  // Of one core.
};

struct AndroidSnapshot {
    bool valid = false;
    bool native = false;  // CPU use of the native runner process, not of the whole Android system
    int cores = 0;
    double cpuPercent = 0;  // Of all cores.
    std::vector<ThreadUse> threads;  // Busiest first.
};

class AndroidStats {
public:
    AndroidStats(std::string adb, std::string serial, std::string package, std::string nativeProcess = {})
        : adb_(std::move(adb)), serial_(std::move(serial)), package_(std::move(package)),
          nativeProcess_(std::move(nativeProcess)) {}

    void start()
    {
        std::thread([this] {
            for (;;) {
#ifdef _WIN32
                if (enabled) nativeProcess_.empty() ? poll() : pollNative();
#else
                if (enabled) poll();
#endif
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }).detach();
    }

    bool native() const { return !nativeProcess_.empty(); }

    AndroidSnapshot snapshot()
    {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    std::atomic<bool> enabled{true};

private:
#ifdef _WIN32
    // Runs a command without a console window and returns its standard output.
    static bool run(std::string command, std::string& output)
    {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &inherit, 0)) return false;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        // Inherit only the output pipe. If this adb call starts the adb server, that server lives on and
        // would otherwise keep every inheritable handle, including the viewer's port 38491 listener.
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeStorage(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &write, sizeof(write),
                                       nullptr, nullptr)) {
            CloseHandle(read);
            CloseHandle(write);
            return false;
        }
        STARTUPINFOEXA startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = write;
        startup.StartupInfo.hStdError = write;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                            &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(write);
        if (started) {
            char buffer[4096];
            DWORD n = 0;
            while (ReadFile(read, buffer, sizeof(buffer), &n, nullptr) && n) output.append(buffer, n);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
        CloseHandle(read);
        return started;
    }
#else
    // Runs a shell command and returns its standard output. The child keeps only its standard streams: an adb server
    // it starts would otherwise keep the viewer's descriptors, including the port 38491 listener (as on Windows).
    static bool run(const std::string& command, std::string& output)
    {
        int pipe[2];
        if (::pipe(pipe) != 0) return false;
        const pid_t child = fork();
        if (child == 0) {
            dup2(pipe[1], STDOUT_FILENO);
            close_range(3, ~0u, 0);
            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(pipe[1]);
        if (child < 0) {
            close(pipe[0]);
            return false;
        }
        char buffer[4096];
        ssize_t n = 0;
        while ((n = read(pipe[0], buffer, sizeof(buffer))) > 0) output.append(buffer, size_t(n));
        close(pipe[0]);
        int status = 0;
        waitpid(child, &status, 0);
        return true;
    }
    // Single-quoted for /bin/sh.
    static std::string quote(const std::string& text)
    {
        std::string quoted = "'";
        for (char c : text) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return quoted + "'";
    }
#endif

    void poll()
    {
        std::string output;
#ifdef _WIN32
        const std::string command = "\"" + adb_ + "\" -s " + serial_ + " shell \"grep '^cpu' /proc/stat; p=$(pidof " +
            package_ + "); [ -n \\\"$p\\\" ] && cat /proc/$p/task/*/stat\"";
#else
        const std::string command = quote(adb_) + " -s " + quote(serial_) + " shell " +
            quote("grep '^cpu' /proc/stat; p=$(pidof " + package_ + "); [ -n \"$p\" ] && cat /proc/$p/task/*/stat") +
            " 2>/dev/null";
#endif
        const auto now = std::chrono::steady_clock::now();
        if (!run(command, output)) return;

        AndroidSnapshot next;
        unsigned long long total = 0, idle = 0;
        std::map<int, std::pair<std::string, unsigned long long>> threads;  // tid -> (name, cpu ticks)
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("cpu", 0) == 0) {
                if (line.size() > 3 && line[3] != ' ') { ++next.cores; continue; }
                std::istringstream fields(line.substr(3));
                unsigned long long value[8]{};
                for (auto& v : value) fields >> v;
                for (auto v : value) total += v;
                idle = value[3] + value[4];  // idle + iowait
                continue;
            }
            const auto open = line.find('('), close = line.rfind(')');
            // close + 2 past the end (a line cut off after the name) would make substr throw on this detached thread.
            if (open == std::string::npos || close == std::string::npos || close < open || close + 2 > line.size()) continue;
            std::istringstream fields(line.substr(close + 2));
            std::string field[13];
            for (auto& f : field) fields >> f;  // state ... utime (index 11), stime (index 12)
            if (field[12].empty()) continue;
            threads[std::atoi(line.c_str())] = {line.substr(open + 1, close - open - 1),
                                                std::strtoull(field[11].c_str(), nullptr, 10) + std::strtoull(field[12].c_str(), nullptr, 10)};
        }
        if (!total) return;

        const double seconds = std::chrono::duration<double>(now - previousAt_).count();
        if (previousTotal_ && total > previousTotal_) {
            next.cpuPercent = 100.0 * (1.0 - double(idle - previousIdle_) / double(total - previousTotal_));
            for (const auto& [tid, thread] : threads) {
                const auto found = previousThreads_.find(tid);
                if (found == previousThreads_.end() || thread.second < found->second) continue;
                const double percent = double(thread.second - found->second) / seconds;  // 100 ticks per second.
                if (percent >= 1.0) next.threads.push_back({thread.first, percent});
            }
            std::sort(next.threads.begin(), next.threads.end(), [](const auto& a, const auto& b) { return a.percent > b.percent; });
            next.valid = true;
        }
        previousTotal_ = total;
        previousIdle_ = idle;
        previousAt_ = now;
        previousThreads_.clear();
        for (const auto& [tid, thread] : threads) previousThreads_[tid] = thread.second;

        std::lock_guard lock(mutex_);
        snapshot_ = std::move(next);
    }

#ifdef _WIN32
    static unsigned long long ticks(const FILETIME& a, const FILETIME& b)  // 100 ns units
    {
        return (ULARGE_INTEGER{{a.dwLowDateTime, a.dwHighDateTime}}.QuadPart) +
            (ULARGE_INTEGER{{b.dwLowDateTime, b.dwHighDateTime}}.QuadPart);
    }

    // The newest process named nativeProcess_ (a new game session starts a new one) and its threads.
    void pollNative()
    {
        const auto now = std::chrono::steady_clock::now();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        DWORD pid = 0;
        PROCESSENTRY32W pe{sizeof(pe)};
        const std::wstring want(nativeProcess_.begin(), nativeProcess_.end());
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) pid = (std::max)(pid, pe.th32ProcessID);  // windows.h may define max
        std::map<int, std::pair<std::string, unsigned long long>> threads;  // tid -> (name, cpu time)
        unsigned long long total = 0;
        HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (process) {
            FILETIME created, exited, kernel, user;
            if (GetProcessTimes(process, &created, &exited, &kernel, &user)) total = ticks(kernel, user);
            CloseHandle(process);
            THREADENTRY32 te{sizeof(te)};
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid) continue;
                HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (!thread) continue;
                std::string name;
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
                    for (const wchar_t* c = description; *c; ++c) name += *c < 128 ? char(*c) : '?';
                    LocalFree(description);
                }
                if (name.rfind("guest:", 0) == 0) name.erase(0, 6);
                if (name.empty()) name = "tid " + std::to_string(te.th32ThreadID);
                if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
                    threads[int(te.th32ThreadID)] = {name, ticks(kernel, user)};
                CloseHandle(thread);
            }
        }
        CloseHandle(snap);

        AndroidSnapshot next;
        next.native = true;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        next.cores = int(si.dwNumberOfProcessors);
        const double seconds = std::chrono::duration<double>(now - previousAt_).count();
        if (pid && pid == previousPid_ && seconds > 0 && total >= previousTotal_) {
            next.cpuPercent = double(total - previousTotal_) / 1e5 / seconds / next.cores;
            for (const auto& [tid, thread] : threads) {
                const auto found = previousThreads_.find(tid);
                if (found == previousThreads_.end() || thread.second < found->second) continue;
                const double percent = double(thread.second - found->second) / 1e5 / seconds;
                if (percent >= 1.0) next.threads.push_back({thread.first, percent});
            }
            std::sort(next.threads.begin(), next.threads.end(), [](const auto& a, const auto& b) { return a.percent > b.percent; });
            next.valid = true;
        }
        previousPid_ = pid;
        previousTotal_ = total;
        previousAt_ = now;
        previousThreads_.clear();
        for (const auto& [tid, thread] : threads) previousThreads_[tid] = thread.second;

        std::lock_guard lock(mutex_);
        snapshot_ = std::move(next);
    }
    DWORD previousPid_ = 0;
#endif

    std::string adb_, serial_, package_, nativeProcess_;
    std::mutex mutex_;
    AndroidSnapshot snapshot_;
    unsigned long long previousTotal_ = 0, previousIdle_ = 0;
    std::chrono::steady_clock::time_point previousAt_{};
    std::map<int, unsigned long long> previousThreads_;
};
