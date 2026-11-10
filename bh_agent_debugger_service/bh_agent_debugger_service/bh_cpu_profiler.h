#pragma once
// Owned, real-time ETW sampled CPU profiler (Windows 8+). No attachment,
// suspension, kernel-logger reuse, sample-interval changes or symbol servers.
// Commands must be serialized by the broker. Destruction stops ONLY our logger.
// Percentages and CPU milliseconds are sample estimates, not wall time. Timing
// assumes a constant ProfileTime interval verified at capture start and stop.
// Unknown/changed intervals suppress milliseconds. Inclusive counts de-duplicate
// recursion; trees preserve recursion. Kernel/user split stacks are partial, not
// stitched across address spaces. Missing stacks are excluded from aggregation.
// Toolhelp seeds target threads/modules; ETW thread/image events maintain safety.
// ANY image load/unload, malformed image event or ETW loss disables snapshot
// symbolication for the whole trace: raw addresses cannot acquire a false name.
// Local PDBs only (adjacent to image), GUID+age verified; no source retrieval.
// Other in-process DbgHelp callers must also use detail::symbol_mutex().
// Retained samples <=20000, frames/sample <=64, pending correlations <=2048,
// modules/threads <=2048, symbol identities <=8192, aggregation keys/table <=8192,
// report rows <=20/depth <=12. Capped aggregation reports excluded contributions.
// Normal shutdown drains ETW after STOP; cancellation/rollback may omit tail
// buffers (explicitly reported). Target exit deliberately discards post-exit
// delivered buffers to reject PID reuse. Reports require stopped collection.
// Snapshot symbols are also revalidated against a post-stop module snapshot.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <stdexcept>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ole32.lib")

namespace bh_cpu_profiler
{
namespace detail
{
inline const GUID& perf_guid()
{
    static const GUID id = {0xce1dbfb4,0x137e,0x4da6,{0x87,0xb0,0x3f,0x59,0xaa,0x10,0x2c,0xbc}};
    return id;
}
inline const GUID& stack_guid()
{
    static const GUID id = {0xdef2fe46,0x7bd6,0x4b80,{0xbd,0x94,0xf5,0x7f,0xe2,0x0d,0x0c,0xe3}};
    return id;
}
inline const GUID& thread_guid()
{
    static const GUID id = {0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}};
    return id;
}
inline const GUID& image_guid()
{
    static const GUID id = {0x2cb15d1d,0x5fc1,0x11d2,{0xab,0xe1,0x00,0xa0,0xc9,0x11,0xf5,0x18}};
    return id;
}
inline std::mutex& symbol_mutex()
{
    static std::mutex value;
    return value;
}
struct handle
{
    HANDLE value = nullptr;
    ~handle() { reset(); }
    void reset(HANDLE next = nullptr)
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
    handle() = default;
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
};
// Read the machine's interval without changing it or another profiler's settings.
inline ULONG profile_interval()
{
    TRACE_PROFILE_INTERVAL interval = {};
    interval.Source = 0; // ProfileTime: interval units are 100 ns.
    ULONG length = 0;
    return TraceQueryInformation(0, TraceSampledProfileIntervalInfo, &interval, sizeof(interval), &length) == ERROR_SUCCESS
        ? interval.Interval : 0;
}
// Enable only an already-assigned privilege and restore its previous state.
// This does not elevate the broker or grant a missing privilege.
struct profile_privilege
{
    handle token;
    TOKEN_PRIVILEGES previous = {};
    bool restore = false;
    profile_privilege()
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token.value)) return;
        TOKEN_PRIVILEGES wanted = {};
        wanted.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, L"SeSystemProfilePrivilege", &wanted.Privileges[0].Luid)) return;
        wanted.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD bytes = sizeof(previous);
        restore = AdjustTokenPrivileges(token.value, FALSE, &wanted, sizeof(previous), &previous, &bytes) && GetLastError() == ERROR_SUCCESS;
    }
    ~profile_privilege()
    {
        if (restore) AdjustTokenPrivileges(token.value, FALSE, &previous, 0, nullptr, nullptr);
    }
};
struct request
{
    std::string verb;
    DWORD pid = 0;
    DWORD seconds = 10;
};
inline bool decimal(const std::string& token, DWORD& value)
{
    if (token.empty() || token.size() > 10) return false;
    unsigned long long n = 0;
    for (char c : token)
    {
        if (c < '0' || c > '9') return false;
        n = n * 10 + static_cast<unsigned>(c - '0');
        if (n > MAXDWORD) return false;
    }
    value = static_cast<DWORD>(n);
    return value != 0;
}
inline bool parse(const std::string& text, request& result)
{
    if (text.size() > 128 || text.find_first_of("\r\n\t\v\f") != std::string::npos || text.find('\0') != std::string::npos) return false;
    std::istringstream in(text);
    std::string prefix, verb, pid, seconds, extra;
    if (!(in >> prefix >> verb) || prefix != "profile") return false;
    request next;
    next.verb = verb;
    if (verb == "start")
    {
        if (!(in >> pid) || !decimal(pid, next.pid)) return false;
        if (in >> seconds)
        {
            if (!decimal(seconds, next.seconds) || next.seconds > 60) return false;
        }
    }
    else if (verb != "stop" && verb != "status" && verb != "functions" && verb != "calltree" && verb != "hotpaths") return false;
    if (in >> extra) return false;
    result = next;
    return true;
}
template<class T> inline bool field(const EVENT_RECORD& e, size_t offset, T& value)
{
    if (!e.UserData || offset > e.UserDataLength || sizeof(T) > e.UserDataLength - offset) return false;
    std::memcpy(&value, static_cast<const unsigned char*>(e.UserData) + offset, sizeof(T));
    return true;
}
inline unsigned pointer_bytes(const EVENT_RECORD& e)
{
    const auto flags = e.EventHeader.Flags;
    if ((flags & EVENT_HEADER_FLAG_32_BIT_HEADER) && !(flags & EVENT_HEADER_FLAG_64_BIT_HEADER)) return 4;
    if ((flags & EVENT_HEADER_FLAG_64_BIT_HEADER) && !(flags & EVENT_HEADER_FLAG_32_BIT_HEADER)) return 8;
    return 0; // Never guess the producer's pointer size from the broker bitness.
}
inline bool address(const EVENT_RECORD& e, size_t offset, unsigned width, unsigned long long& value)
{
    value = 0;
    if (width == 8) return field(e, offset, value);
    DWORD narrow = 0;
    if (width != 4 || !field(e, offset, narrow)) return false;
    value = narrow;
    return true;
}
struct sample
{
    unsigned long long timestamp = 0, ip = 0;
    DWORD tid = 0;
    std::vector<unsigned long long> frames; // leaf first
};
inline bool decode_sample(const EVENT_RECORD& e, sample& result)
{
    const unsigned width = pointer_bytes(e);
    DWORD ignored = 0;
    if (e.EventHeader.EventDescriptor.Version > 2 || !address(e, 0, width, result.ip) ||
        !field(e, width, result.tid) || !field(e, width + 4, ignored) || !result.ip || !result.tid) return false;
    result.timestamp = static_cast<unsigned long long>(e.EventHeader.TimeStamp.QuadPart);
    return true;
}
inline bool decode_stack(const EVENT_RECORD& e, sample& result, DWORD& pid, size_t& clipped)
{
    const unsigned width = pointer_bytes(e);
    if (e.EventHeader.EventDescriptor.Version > 2 || !width || e.UserDataLength < 16 + width ||
        (e.UserDataLength - 16) % width != 0 || !field(e, 0, result.timestamp) ||
        !field(e, 8, pid) || !field(e, 12, result.tid) || !pid || !result.tid) return false;
    const size_t count = (e.UserDataLength - 16) / width;
    if (count > 192) return false;
    clipped = count > 64 ? count - 64 : 0;
    result.frames.clear();
    for (size_t i = 0; i < (std::min)(count, size_t(64)); ++i)
    {
        unsigned long long pc = 0;
        if (!address(e, 16 + i * width, width, pc)) return false;
        if (pc) result.frames.push_back(pc);
    }
    return !result.frames.empty();
}
inline std::string hex(unsigned long long value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}
inline std::string clean(const std::string& value, size_t limit = 100)
{
    std::string out;
    for (char c : value)
    {
        if (out.size() == limit) break;
        const auto ch = static_cast<unsigned char>(c);
        out.push_back(ch >= 32 && ch < 127 && c != ';' && c != '|' ? c : '_');
    }
    return out;
}
struct function_count { unsigned long long self = 0, inclusive = 0; };
struct aggregate
{
    std::map<std::string, function_count> functions;
    std::map<std::vector<std::string>, unsigned long long> paths;
    std::map<std::vector<std::string>, unsigned long long> tree;
    unsigned long long samples = 0, depth_clipped = 0, dropped_functions = 0, dropped_paths = 0, dropped_tree = 0;
    void add(const std::vector<std::string>& leaf_first, const std::string& sampled_self = {})
    {
        if (leaf_first.empty()) return;
        const std::string& self = sampled_self.empty() ? leaf_first.front() : sampled_self;
        ++samples;
        std::set<std::string> unique(leaf_first.begin(), leaf_first.end());
        unique.insert(self);
        for (const auto& name : unique)
        {
            if (functions.count(name) || functions.size() < 8192)
            {
                auto& count = functions[name];
                ++count.inclusive;
                if (name == self) ++count.self;
            }
            else ++dropped_functions;
        }
        std::vector<std::string> root_first(leaf_first.rbegin(), leaf_first.rend());
        if (root_first.size() > 12)
        {
            depth_clipped += root_first.size() - 12;
            root_first.resize(12);
        }
        if (paths.count(root_first) || paths.size() < 8192) ++paths[root_first];
        else ++dropped_paths;
        for (size_t n = 1; n <= root_first.size(); ++n)
        {
            std::vector<std::string> prefix(root_first.begin(), root_first.begin() + n);
            if (tree.count(prefix) || tree.size() < 8192) ++tree[prefix];
            else ++dropped_tree;
        }
    }
};
// bounded append never cuts a row or loses truncation metadata.
inline std::string report(const aggregate& data, const std::string& verb, const std::string& summary, ULONG interval_100ns = 0)
{
    struct row { unsigned long long count; std::string text; };
    std::vector<row> rows;
    if (verb == "functions")
    {
        for (const auto& item : data.functions)
        {
            std::ostringstream out;
            const double scale = data.samples ? 100.0 / static_cast<double>(data.samples) : 0;
            out << clean(item.first) << " self=" << item.second.self << "(" << std::fixed << std::setprecision(2)
                << item.second.self * scale << "%) incl=" << item.second.inclusive << "(" << item.second.inclusive * scale << "%)";
            if (interval_100ns)
                out << " self_cpu_ms_est=" << item.second.self * (interval_100ns / 10000.0)
                    << " incl_cpu_ms_est=" << item.second.inclusive * (interval_100ns / 10000.0);
            rows.push_back({item.second.inclusive, out.str()});
        }
    }
    else
    {
        const auto& entries = verb == "hotpaths" ? data.paths : data.tree;
        for (const auto& item : entries)
        {
            std::string text;
            for (const auto& name : item.first)
            {
                if (!text.empty()) text += ">";
                text += clean(name, 100);
            }
            std::ostringstream out;
            out << text << " samples=" << item.second << "(" << std::fixed << std::setprecision(2)
                << (data.samples ? 100.0 * item.second / data.samples : 0) << "%)";
            if (interval_100ns) out << " cpu_ms_est=" << item.second * (interval_100ns / 10000.0);
            rows.push_back({item.second, out.str()});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) { return a.count != b.count ? a.count > b.count : a.text < b.text; });
    std::string out = summary + "; " + verb + " estimates=sample_percent denominator=aggregated_stacks rows_limit=20 depth_limit=12";
    size_t shown = 0;
    for (const auto& item : rows)
    {
        if (shown == 20 || out.size() + item.text.size() + 384 >= 7400) break;
        out += "; " + item.text;
        ++shown;
    }
    out += "; shown=" + std::to_string(shown) + " truncated_rows=" + std::to_string(rows.size() - shown) +
        " truncated_depth_frames=" + std::to_string(data.depth_clipped) +
        " dropped_function_contributions=" + std::to_string(data.dropped_functions) +
        " dropped_path_contributions=" + std::to_string(data.dropped_paths) +
        " dropped_tree_contributions=" + std::to_string(data.dropped_tree);
    return out;
}
struct module
{
    unsigned long long base = 0;
    DWORD size = 0;
    std::wstring image, pdb;
    std::string name;
    GUID signature = {};
    DWORD age = 0, timestamp = 0;
    bool identity = false, loaded = false;
};
inline std::wstring local_file(const std::wstring& input)
{
    std::wstring candidate = input;
    if (candidate.compare(0, 4, L"\\\\?\\") == 0) candidate.erase(0, 4);
    if (candidate.size() < 3 || candidate[1] != L':' || candidate[2] != L'\\') return {};
    const std::wstring drive = candidate.substr(0, 3);
    if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED && GetDriveTypeW(drive.c_str()) != DRIVE_RAMDISK) return {};
    // Reject UNC, mapped drives and reparse-point directories BEFORE opening
    // the actual file; neither PDB discovery nor DbgHelp may follow a server path.
    for (size_t end = 3; end <= candidate.size(); ++end)
    {
        if (end != candidate.size() && candidate[end] != L'\\') continue;
        const std::wstring part = candidate.substr(0, end);
        handle component;
        component.reset(CreateFileW(part.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        FILE_ATTRIBUTE_TAG_INFO attributes = {};
        if (component.value == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandleEx(component.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    }
    handle file;
    file.reset(CreateFileW(candidate.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return {};
    wchar_t resolved[32768] = {};
    const DWORD n = GetFinalPathNameByHandleW(file.value, resolved, 32768, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= 32768) return {};
    std::wstring path(resolved, n);
    if (path.compare(0, 4, L"\\\\?\\") == 0) path.erase(0, 4);
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') return {};
    const std::wstring root = path.substr(0, 3);
    const auto type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_RAMDISK) return {};
    return path;
}
struct symbol_options
{
    DWORD previous;
    symbol_options() : previous(SymGetOptions())
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_DISABLE_SYMSRV_AUTODETECT);
    }
    ~symbol_options() { SymSetOptions(previous); }
};
} // namespace detail

class session
{
    struct properties
    {
        EVENT_TRACE_PROPERTIES value;
        wchar_t name[128];
    } props_ = {};
    struct pending
    {
        detail::sample value;
        bool profile = false, stack = false;
    };
    using key = std::pair<unsigned long long, DWORD>;
    std::mutex data_mutex_;
    std::map<key, pending> pending_;
    std::vector<detail::sample> samples_;
    std::set<DWORD> threads_;
    std::vector<detail::module> modules_;
    std::map<unsigned long long, std::string> names_;
    detail::handle target_, wake_, symbols_;
    TRACEHANDLE logger_ = 0, consumer_ = INVALID_PROCESSTRACE_HANDLE;
    EVENT_TRACE_LOGFILEW logfile_ = {};
    std::thread consumer_thread_, timer_thread_;
    std::atomic<bool> active_{false}, callback_failed_{false};
    DWORD pid_ = 0, seconds_ = 0;
    unsigned long long started_ = 0, ended_ = 0, samples_seen_ = 0, missing_ = 0, orphan_ = 0, dropped_ = 0;
    unsigned long long malformed_ = 0, clipped_ = 0, fragments_ = 0, matched_ = 0, ip_mismatch_ = 0;
    ULONG events_lost_ = 0, buffers_lost_ = 0, consume_error_ = 0, stop_error_ = 0;
    ULONG interval_begin_ = 0, interval_end_ = 0;
    bool changed_ = false, seeded_ = false, symbols_initialized_ = false, tail_omitted_ = false;
    std::string reason_ = "idle";

    void retire(const pending& item)
    {
        if (item.profile) ++samples_seen_;
        if (!item.profile) { ++orphan_; return; }
        if (!item.stack) { ++missing_; return; }
        ++matched_;
        if (!item.value.frames.empty() && item.value.frames.front() != item.value.ip) ++ip_mismatch_;
        if (samples_.size() >= 20000) { ++dropped_; return; }
        samples_.push_back(item.value);
    }
    void trim_pending()
    {
        while (pending_.size() > 2048)
        {
            retire(pending_.begin()->second);
            pending_.erase(pending_.begin());
        }
    }
    static void WINAPI event_callback(EVENT_RECORD* event) noexcept
    {
        if (!event || !event->UserContext) return;
        auto* self = static_cast<session*>(event->UserContext);
        try { self->event(*event); }
        catch (...) { self->callback_failed_ = true; SetEvent(self->wake_.value); }
    }
    void event(const EVENT_RECORD& e)
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        // The retained object handle, not a fresh PID lookup, is the exit guard.
        // Buffered events delivered after exit are deliberately omitted rather
        // than accepting events belonging to a reused PID/thread ID.
        if (target_.value && WaitForSingleObject(target_.value, 0) != WAIT_TIMEOUT) { changed_ = true; return; }
        const auto& guid = e.EventHeader.ProviderId;
        const BYTE opcode = e.EventHeader.EventDescriptor.Opcode;
        if (IsEqualGUID(guid, detail::thread_guid()) && opcode >= 1 && opcode <= 4)
        {
            DWORD pid = 0, tid = 0;
            if (e.EventHeader.EventDescriptor.Version < 1 || e.EventHeader.EventDescriptor.Version > 3 ||
                !detail::field(e, 0, pid) || !detail::field(e, 4, tid)) { ++malformed_; return; }
            if (opcode == 2 || opcode == 4 || pid != pid_) threads_.erase(tid);
            else if (threads_.size() < 2048) threads_.insert(tid);
            else ++dropped_;
            return;
        }
        if (IsEqualGUID(guid, detail::image_guid()) && (opcode == 10 || opcode == 2 || opcode == 3 || opcode == 4))
        {
            DWORD pid = 0;
            const unsigned width = detail::pointer_bytes(e);
            if (!width || e.EventHeader.EventDescriptor.Version < 2 || !detail::field(e, 2 * width, pid))
            { ++malformed_; changed_ = true; return; }
            if (pid == pid_ && (opcode == 10 || opcode == 2)) changed_ = true;
            return;
        }
        if (IsEqualGUID(guid, detail::perf_guid()) && opcode == 46)
        {
            detail::sample decoded;
            if (!detail::decode_sample(e, decoded)) { ++malformed_; return; }
            const key id(decoded.timestamp, decoded.tid);
            if (!threads_.count(decoded.tid) && !pending_.count(id)) return;
            auto& item = pending_[id];
            if (item.profile) { ++dropped_; return; }
            item.profile = true;
            item.value.timestamp = decoded.timestamp;
            item.value.ip = decoded.ip;
            item.value.tid = decoded.tid;
            trim_pending();
        }
        else if (IsEqualGUID(guid, detail::stack_guid()) && opcode == 32)
        {
            detail::sample decoded;
            DWORD pid = 0;
            size_t clipped = 0;
            if (!detail::decode_stack(e, decoded, pid, clipped)) { ++malformed_; return; }
            if (pid != pid_) return;
            clipped_ += clipped;
            auto& item = pending_[key(decoded.timestamp, decoded.tid)];
            // The second fragment is NOT safely stitchable without kernel boundary
            // metadata. Keep the first and report the discarded fragment explicitly.
            if (item.stack) { ++fragments_; return; }
            item.stack = true;
            item.value.timestamp = decoded.timestamp;
            item.value.tid = decoded.tid;
            item.value.frames.swap(decoded.frames);
            trim_pending();
        }
    }
    void seed()
    {
        detail::handle snapshot;
        snapshot.reset(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
        THREADENTRY32 thread = {};
        thread.dwSize = sizeof(thread);
        if (snapshot.value == INVALID_HANDLE_VALUE || !Thread32First(snapshot.value, &thread))
            throw std::runtime_error("thread snapshot unavailable");
        do
        {
            if (thread.th32OwnerProcessID == pid_)
            {
                if (threads_.size() == 2048) throw std::runtime_error("too many target threads");
                threads_.insert(thread.th32ThreadID);
            }
        } while (Thread32Next(snapshot.value, &thread));
        snapshot.reset(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_));
        MODULEENTRY32W module = {};
        module.dwSize = sizeof(module);
        if (snapshot.value == INVALID_HANDLE_VALUE || !Module32FirstW(snapshot.value, &module))
            throw std::runtime_error("module snapshot unavailable (use x64 broker for x64 targets)");
        std::lock_guard<std::mutex> symbol_lock(detail::symbol_mutex());
        detail::symbol_options options;
        do
        {
            if (modules_.size() == 2048) throw std::runtime_error("too many target modules");
            detail::module item;
            item.base = reinterpret_cast<ULONG_PTR>(module.modBaseAddr);
            item.size = module.modBaseSize;
            item.image = detail::local_file(module.szExePath);
            if (item.image.empty()) changed_ = true;
            for (wchar_t ch : std::wstring(module.szModule)) item.name.push_back(ch >= 32 && ch < 127 ? static_cast<char>(ch) : '_');
            if (!item.image.empty())
            {
                SYMSRV_INDEX_INFOW identity = {};
                identity.sizeofstruct = sizeof(identity);
                if (SymSrvGetFileIndexInfoW(item.image.c_str(), &identity, 0) && !IsEqualGUID(identity.guid, GUID{}))
                {
                    item.signature = identity.guid;
                    item.age = identity.age;
                    item.timestamp = identity.timestamp;
                    item.identity = true;
                    std::wstring pdb(identity.pdbfile);
                    const auto slash = pdb.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) pdb.erase(0, slash + 1);
                    // Ignore embedded PDB directories and environment search paths.
                    if (!pdb.empty() && pdb.find_first_of(L":\\/") == std::wstring::npos)
                        item.pdb = detail::local_file(item.image.substr(0, item.image.find_last_of(L'\\') + 1) + pdb);
                }
            }
            modules_.push_back(item);
        } while (Module32NextW(snapshot.value, &module));
        seeded_ = true;
    }
    void clear_symbols()
    {
        std::lock_guard<std::mutex> lock(detail::symbol_mutex());
        if (symbols_initialized_) SymCleanup(symbols_.value);
        symbols_initialized_ = false;
        symbols_.reset();
        names_.clear();
        for (auto& item : modules_) item.loaded = false;
    }
    std::string symbol(unsigned long long pc, bool unsafe)
    {
        if (unsafe) return detail::hex(pc);
        auto cached = names_.find(pc);
        if (cached != names_.end()) return cached->second;
        if (names_.size() >= 8192) return detail::hex(pc);
        std::string result = detail::hex(pc);
        for (auto& item : modules_)
        {
            if (pc < item.base || pc - item.base >= item.size) continue;
            // Snapshot module+RVA remains the stable fallback, never an address
            // cached against a later reload. Dynamic modules disable this entirely.
            const std::string module_id = detail::clean(item.name, 24) + "[" + detail::hex(item.base) + "]";
            result = module_id + "+" + detail::hex(pc - item.base);
            std::lock_guard<std::mutex> lock(detail::symbol_mutex());
            detail::symbol_options options;
            if (!symbols_initialized_)
            {
                symbols_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                symbols_initialized_ = symbols_.value && SymInitializeW(symbols_.value, L"", FALSE);
            }
            if (symbols_initialized_ && item.identity && !item.pdb.empty() && !item.loaded)
            {
                item.loaded = true; // At most one local attempt per module.
                SYMSRV_INDEX_INFOW image = {}, pdb = {};
                image.sizeofstruct = sizeof(image); pdb.sizeofstruct = sizeof(pdb);
                if (SymSrvGetFileIndexInfoW(item.image.c_str(), &image, 0) &&
                    SymSrvGetFileIndexInfoW(item.pdb.c_str(), &pdb, 0) &&
                    IsEqualGUID(image.guid, item.signature) && image.age == item.age && image.timestamp == item.timestamp &&
                    IsEqualGUID(pdb.guid, item.signature) && pdb.age == item.age)
                    SymLoadModuleExW(symbols_.value, nullptr, item.pdb.c_str(), nullptr, item.base, item.size, nullptr, 0);
            }
            if (symbols_initialized_)
            {
                alignas(SYMBOL_INFO) unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
                auto* info = reinterpret_cast<SYMBOL_INFO*>(storage);
                info->SizeOfStruct = sizeof(SYMBOL_INFO); info->MaxNameLen = MAX_SYM_NAME;
                DWORD64 displacement = 0;
                if (SymFromAddr(symbols_.value, pc, &displacement, info) && info->ModBase == item.base && info->Address <= pc &&
                    info->Address >= item.base && !(info->Flags & SYMFLAG_EXPORT) &&
                    (info->Tag == 5 /* SymTagFunction */ || info->Tag == 10 /* SymTagPublicSymbol */) && info->NameLen < MAX_SYM_NAME)
                    result = module_id + "!" + detail::clean(std::string(info->Name, info->NameLen), 32) +
                        "@" + detail::hex(info->Address - item.base);
            }
            break;
        }
        names_.emplace(pc, result);
        return result;
    }
    void verify_modules()
    {
        detail::handle snapshot;
        if (!target_.value || WaitForSingleObject(target_.value, 0) != WAIT_TIMEOUT) { changed_ = true; return; }
        snapshot.reset(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_));
        MODULEENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (snapshot.value == INVALID_HANDLE_VALUE || !Module32FirstW(snapshot.value, &entry)) { changed_ = true; return; }
        size_t count = 0;
        do
        {
            ++count;
            const auto found = std::find_if(modules_.begin(), modules_.end(), [&entry](const detail::module& item)
            {
                return item.base == reinterpret_cast<ULONG_PTR>(entry.modBaseAddr) && item.size == entry.modBaseSize;
            });
            if (found == modules_.end() || _wcsicmp(found->image.c_str(), detail::local_file(entry.szExePath).c_str()) != 0)
            { changed_ = true; return; }
            if (count > 2048) { changed_ = true; return; }
        } while (Module32NextW(snapshot.value, &entry));
        if (count != modules_.size()) changed_ = true;
    }
    void stop_logger(bool cancel = true) noexcept
    {
        detail::profile_privilege privilege;
        if (logger_)
        {
            properties final_properties = props_;
            // Stop by our UUID name, not a potentially recycled numeric handle.
            const ULONG result = ControlTraceW(0, props_.name, &final_properties.value, EVENT_TRACE_CONTROL_STOP);
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                stop_error_ = result == ERROR_WMI_INSTANCE_NOT_FOUND ? ERROR_SUCCESS : result;
                events_lost_ = final_properties.value.EventsLost;
                buffers_lost_ = final_properties.value.LogBuffersLost + final_properties.value.RealTimeBuffersLost;
                if (events_lost_ || buffers_lost_) changed_ = true;
            }
            // Keep the owned handle if STOP failed; destructor/next stop retries.
            if (result == ERROR_SUCCESS || result == ERROR_WMI_INSTANCE_NOT_FOUND) logger_ = 0;
        }
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE && (cancel || logger_))
        {
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                tail_omitted_ = true;
                changed_ = true; // Missing image events cannot be trusted for mapping.
            }
            CloseTrace(consumer_);
            consumer_ = INVALID_PROCESSTRACE_HANDLE;
        }
    }
    void finish()
    {
        interval_end_ = detail::profile_interval();
        stop_logger(false);
        if (consumer_thread_.joinable()) consumer_thread_.join();
        if (consumer_ != INVALID_PROCESSTRACE_HANDLE)
        {
            CloseTrace(consumer_);
            consumer_ = INVALID_PROCESSTRACE_HANDLE;
        }
        std::lock_guard<std::mutex> lock(data_mutex_);
        verify_modules();
        for (const auto& item : pending_) retire(item.second);
        pending_.clear();
        ended_ = GetTickCount64();
        active_ = false;
    }
    void join_finished()
    {
        if (!active_ && timer_thread_.joinable()) timer_thread_.join();
    }
    std::string summary_locked() const
    {
        std::ostringstream out;
        out << "ok: profile state=" << (active_ ? "running" : (pid_ ? "stopped" : "idle")) << " pid=" << pid_
            << " reason=" << reason_ << " seconds_limit=" << seconds_
            << " elapsed_ms=" << (pid_ ? (active_ ? GetTickCount64() : ended_) - started_ : 0)
            << " samples=" << samples_seen_ << " matched_stacks=" << matched_ << " retained=" << samples_.size()
            << " pending=" << pending_.size() << " missing_stacks_unconfirmed=" << missing_ << " orphan_stacks=" << orphan_
            << " dropped_samples=" << dropped_ << " dropped_stack_fragments=" << fragments_ << " clipped_stack_frames=" << clipped_
            << " stack_ip_mismatch=" << ip_mismatch_
            << " malformed_events=" << malformed_ << " etw_events_lost=" << events_lost_ << " etw_buffers_lost=" << buffers_lost_
            << " callback_error=" << callback_failed_ << " consumer_error=" << consume_error_ << " stop_error=" << stop_error_
            << " modules_changed_or_untrusted=" << changed_
            << " interval_begin_100ns=" << interval_begin_ << " interval_end_100ns=" << (active_ ? 0 : interval_end_)
            << " time_estimates=" << (!active_ && interval_begin_ && interval_begin_ == interval_end_ ? "sample_count_x_interval" : "unavailable")
            << " interval_assumption=constant_between_endpoint_queries"
            << " loss_counters=" << (active_ ? "final_on_stop" : "final")
            << " tail_buffers_may_be_omitted=" << tail_omitted_
            << " post_exit_buffers_omitted=1 partial_stacks_possible=1 coverage=matched_target_stacks";
        if (samples_.size() > 1)
        {
            LARGE_INTEGER frequency;
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            {
                auto range = std::minmax_element(samples_.begin(), samples_.end(), [](const detail::sample& a, const detail::sample& b)
                    { return a.timestamp < b.timestamp; });
                out << " observed_target_sample_spacing_ms=" << std::fixed << std::setprecision(4)
                    << 1000.0 * (range.second->timestamp - range.first->timestamp) / frequency.QuadPart / (samples_.size() - 1)
                    << " spacing_is_not_cpu_sampling_interval=1";
            }
        }
        return out.str();
    }
    std::string start(DWORD pid, DWORD seconds)
    {
        if (active_) return "error: profile already running; stop it first";
        join_finished();
        if (logger_)
        {
            stop_logger();
            if (logger_) return "error: owned ETW logger could not be stopped; refusing another session";
        }
        clear_symbols();
        samples_.clear(); pending_.clear(); threads_.clear(); modules_.clear();
        pid_ = 0; seconds_ = seconds; started_ = ended_ = 0;
        samples_seen_ = missing_ = orphan_ = dropped_ = malformed_ = clipped_ = fragments_ = matched_ = ip_mismatch_ = 0;
        events_lost_ = buffers_lost_ = consume_error_ = stop_error_ = 0;
        interval_begin_ = interval_end_ = 0;
        changed_ = seeded_ = tail_omitted_ = false; callback_failed_ = false; reason_ = "start_failed";
        target_.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!target_.value) return "error: profile OpenProcess win32=" + std::to_string(GetLastError());
        if (WaitForSingleObject(target_.value, 0) != WAIT_TIMEOUT) return "error: profile target already exited";
        wake_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!wake_.value) return "error: profile CreateEvent win32=" + std::to_string(GetLastError());
        GUID id;
        if (FAILED(CoCreateGuid(&id))) return "error: profile unique logger name unavailable";
        wchar_t text[40] = {};
        StringFromGUID2(id, text, 40);
        props_ = {};
        const std::wstring name = std::wstring(L"bh_cpu_") + text;
        wcscpy_s(props_.name, name.c_str());
        props_.value.Wnode.BufferSize = sizeof(props_);
        props_.value.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props_.value.Wnode.ClientContext = 1; // QPC + RAW_TIMESTAMP matching.
        // SystemTraceControlGuid is reserved for NT Kernel Logger. A separate
        // system logger uses its own GUID plus EVENT_TRACE_SYSTEM_LOGGER_MODE.
        props_.value.Wnode.Guid = id;
        props_.value.LoggerNameOffset = offsetof(properties, name);
        props_.value.BufferSize = 64;
        props_.value.MinimumBuffers = 64; props_.value.MaximumBuffers = 64;
        props_.value.FlushTimer = 1;
        props_.value.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
        props_.value.EnableFlags = EVENT_TRACE_FLAG_PROFILE | EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_IMAGE_LOAD;
        try
        {
            pid_ = pid;
            seed(); // Local metadata work occurs BEFORE an ETW logger is running.
        }
        catch (const std::exception& e)
        {
            pid_ = 0;
            return "error: profile snapshot: " + detail::clean(e.what());
        }
        if (WaitForSingleObject(target_.value, 0) != WAIT_TIMEOUT)
        { pid_ = 0; return "error: profile target exited before collection"; }
        started_ = GetTickCount64();
        detail::profile_privilege privilege;
        ULONG error = StartTraceW(&logger_, props_.name, &props_.value);
        if (error != ERROR_SUCCESS)
        {
            logger_ = 0; pid_ = 0; // Never STOP a logger we did not successfully create.
            return "error: profile StartTrace win32=" + std::to_string(error) +
                (error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD
                    ? " (requires elevated broker with ETW system-profile permission; no fallback)"
                    : " (ETW session unavailable; no fallback)");
        }
        interval_begin_ = detail::profile_interval();
        CLASSIC_EVENT_ID stack = {};
        stack.EventGuid = detail::perf_guid(); stack.Type = 46;
        error = TraceSetInformation(logger_, TraceStackTracingInfo, &stack, sizeof(stack));
        if (error != ERROR_SUCCESS)
        {
            stop_logger(); pid_ = 0;
            return "error: profile TraceStackTracingInfo win32=" + std::to_string(error);
        }
        try
        {
            logfile_ = {};
            logfile_.LoggerName = props_.name;
            logfile_.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
            logfile_.EventRecordCallback = &event_callback;
            logfile_.Context = this;
            consumer_ = OpenTraceW(&logfile_);
            if (consumer_ == INVALID_PROCESSTRACE_HANDLE)
            {
                error = GetLastError(); stop_logger(); pid_ = 0;
                return "error: profile OpenTrace win32=" + std::to_string(error);
            }
            reason_ = "collecting"; active_ = true;
            const TRACEHANDLE trace = consumer_;
            consumer_thread_ = std::thread([this, trace]() noexcept
            {
                TRACEHANDLE copy = trace;
                const ULONG result = ProcessTrace(&copy, 1, nullptr, nullptr);
                {
                    std::lock_guard<std::mutex> lock(data_mutex_);
                    consume_error_ = result == ERROR_CANCELLED ? ERROR_SUCCESS : result;
                    if (consume_error_) changed_ = true;
                }
                SetEvent(wake_.value);
            });
            timer_thread_ = std::thread([this]() noexcept
            {
                try
                {
                    HANDLE waits[] = {wake_.value, target_.value};
                    const unsigned long long elapsed = GetTickCount64() - started_;
                    const DWORD budget = seconds_ * 1000;
                    const DWORD result = WaitForMultipleObjects(2, waits, FALSE, elapsed >= budget ? 0 : budget - static_cast<DWORD>(elapsed));
                    {
                        std::lock_guard<std::mutex> lock(data_mutex_);
                        reason_ = callback_failed_ ? "callback_failure" : (result == WAIT_OBJECT_0 + 1 ? "target_exit" :
                            (result == WAIT_TIMEOUT ? "deadline" : (result == WAIT_OBJECT_0 ? "stop_or_consumer_end" : "wait_failure")));
                    }
                    finish();
                }
                catch (...)
                {
                    callback_failed_ = true;
                    stop_logger();
                    if (consumer_thread_.joinable()) consumer_thread_.join();
                    {
                        std::lock_guard<std::mutex> lock(data_mutex_);
                        ended_ = GetTickCount64();
                        changed_ = true;
                        active_ = false;
                    }
                }
            });
        }
        catch (const std::exception& e)
        {
            stop_logger();
            if (consumer_thread_.joinable()) consumer_thread_.join();
            active_ = false; pid_ = 0;
            return "error: profile start rollback: " + detail::clean(e.what());
        }
        return "ok: profile started pid=" + std::to_string(pid) + " seconds=" + std::to_string(seconds) +
            " backend=ETW samples_only=1 interval_begin_100ns=" + std::to_string(interval_begin_);
    }
public:
    session() = default;
    session(const session&) = delete;
    session& operator=(const session&) = delete;
    ~session()
    {
        if (wake_.value) SetEvent(wake_.value);
        if (timer_thread_.joinable()) timer_thread_.join();
        stop_logger();
        if (consumer_thread_.joinable()) consumer_thread_.join();
        clear_symbols();
    }
    std::string command(const std::string& message)
    {
        try
        {
            detail::request request;
            if (!detail::parse(message, request)) return "error: profile syntax: start <pid> [seconds 1..60] | stop | status | functions | calltree | hotpaths";
            if (request.verb == "start") return start(request.pid, request.seconds);
            if (request.verb == "stop")
            {
                if (wake_.value) SetEvent(wake_.value);
                if (timer_thread_.joinable()) timer_thread_.join();
                if (logger_) stop_logger();
            }
            join_finished();
            std::vector<detail::sample> snapshot;
            std::string summary;
            bool unsafe = false;
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                summary = summary_locked();
                if (request.verb == "status" || request.verb == "stop")
                    return stop_error_ ? "error: profile owned logger stop win32=" + std::to_string(stop_error_) : summary;
                if (!pid_ || !seeded_) return "error: profile no captured session";
                if (stop_error_) return "error: profile owned logger stop win32=" + std::to_string(stop_error_);
                // Symbol processing cannot block ETW collection: reports require stop.
                if (active_) return "error: profile reports require profile stop (collection still running)";
                snapshot = samples_; unsafe = changed_ || callback_failed_;
            }
            detail::aggregate data;
            for (const auto& item : snapshot)
            {
                std::vector<std::string> frames;
                // Sample IP is authoritative for self. Never invent an edge
                // between an IP and a mismatching (e.g. split user/kernel) stack.
                // The observed stack remains partial; self is counted separately.
                for (const auto pc : item.frames) frames.push_back(symbol(pc, unsafe));
                data.add(frames, symbol(item.ip, unsafe));
            }
            return detail::report(data, request.verb, summary, interval_begin_ == interval_end_ ? interval_begin_ : 0);
        }
        catch (...) { return "error: profile resource or symbol processing failure"; }
    }
};
} // namespace bh_cpu_profiler
