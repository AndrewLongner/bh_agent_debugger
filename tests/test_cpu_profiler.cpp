#define NOMINMAX
#include "../bh_agent_debugger_service/bh_agent_debugger_service/bh_cpu_profiler.h"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* name)
{
    if (!value) throw std::runtime_error(name);
}
static void check_parser()
{
    using namespace bh_cpu_profiler::detail;
    request r;
    check(parse("profile start 123", r) && r.pid == 123 && r.seconds == 10, "default deadline");
    check(parse("profile start 123 1", r) && r.seconds == 1, "minimum deadline");
    check(parse("profile start 4294967295 60", r) && r.pid == MAXDWORD && r.seconds == 60, "maximum bounds");
    for (const char* verb : {"stop", "status", "functions", "calltree", "hotpaths"})
        check(parse(std::string("profile ") + verb, r), "controls accepted");
    for (const char* bad : {"", "start 1", "profile", "profile start", "profile start 0", "profile start -1",
        "profile start +1", "profile start 1.0", "profile start 4294967296", "profile start 1 0",
        "profile start 1 61", "profile start 1 -2", "profile start 1 2 junk", "profile stop junk",
        "profile status 1", "profile Start 1", "profile nope", "profile\tstatus", "profile status\n"})
        check(!parse(bad, r), "malformed command rejected");
    check(!parse(std::string("profile status\0junk", 19), r), "embedded nul rejected");
    check(!parse(std::string(129, 'x'), r), "input bound");
}
template<class T> static void put(std::vector<unsigned char>& data, size_t at, T value)
{
    std::memcpy(data.data() + at, &value, sizeof(value));
}
static void check_events()
{
    using namespace bh_cpu_profiler::detail;
    std::vector<unsigned char> data(16);
    put<unsigned long long>(data, 0, 0x123456781234ULL);
    put<DWORD>(data, 8, 42); put<DWORD>(data, 12, 0);
    EVENT_RECORD e = {};
    e.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
    e.EventHeader.EventDescriptor.Version = 2;
    e.EventHeader.TimeStamp.QuadPart = 456;
    e.UserData = data.data(); e.UserDataLength = static_cast<USHORT>(data.size());
    sample decoded;
    check(decode_sample(e, decoded) && decoded.ip == 0x123456781234ULL && decoded.tid == 42 && decoded.timestamp == 456, "sample x64");
    e.UserDataLength = 15;
    check(!decode_sample(e, decoded), "sample short");
    e.UserDataLength = 16; e.UserData = nullptr;
    check(!decode_sample(e, decoded), "sample null");
    e.UserData = data.data(); e.EventHeader.Flags = 0;
    check(!decode_sample(e, decoded), "unknown pointer width rejected");
    e.EventHeader.Flags = EVENT_HEADER_FLAG_32_BIT_HEADER | EVENT_HEADER_FLAG_64_BIT_HEADER;
    check(!decode_sample(e, decoded), "contradictory pointer width rejected");
    e.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER; e.EventHeader.EventDescriptor.Version = 255;
    check(!decode_sample(e, decoded), "unknown sample schema rejected");
    e.EventHeader.EventDescriptor.Version = 2;
    data.assign(12, 0); put<DWORD>(data, 0, 0xabcdef); put<DWORD>(data, 4, 43);
    e.UserData = data.data(); e.UserDataLength = 12; e.EventHeader.Flags = EVENT_HEADER_FLAG_32_BIT_HEADER;
    check(decode_sample(e, decoded) && decoded.ip == 0xabcdef && decoded.tid == 43, "sample x86");

    data.assign(16 + 192 * 8, 0);
    put<unsigned long long>(data, 0, 456); put<DWORD>(data, 8, 100); put<DWORD>(data, 12, 42);
    for (size_t i = 0; i < 192; ++i) put<unsigned long long>(data, 16 + 8 * i, i + 1);
    e.UserData = data.data(); e.UserDataLength = static_cast<USHORT>(data.size());
    e.EventHeader.Flags = EVENT_HEADER_FLAG_64_BIT_HEADER;
    DWORD pid = 0; size_t clipped = 0;
    check(decode_stack(e, decoded, pid, clipped) && pid == 100 && decoded.timestamp == 456 && decoded.tid == 42,
        "stack correlation fields");
    check(decoded.frames.size() == 64 && decoded.frames.front() == 1 && decoded.frames.back() == 64 && clipped == 128, "stack depth cap");
    --e.UserDataLength;
    check(!decode_stack(e, decoded, pid, clipped), "misaligned stack rejected");
    e.UserDataLength = 16;
    check(!decode_stack(e, decoded, pid, clipped), "empty stack rejected");
    data.resize(16 + 193 * 8); e.UserData = data.data(); e.UserDataLength = static_cast<USHORT>(data.size());
    check(!decode_stack(e, decoded, pid, clipped), "oversized stack rejected");
    data.assign(24, 0); put<unsigned long long>(data, 0, 567); put<DWORD>(data, 8, 100);
    put<DWORD>(data, 12, 42); put<DWORD>(data, 16, 0x111); put<DWORD>(data, 20, 0x222);
    e.UserData = data.data(); e.UserDataLength = 24; e.EventHeader.Flags = EVENT_HEADER_FLAG_32_BIT_HEADER;
    check(decode_stack(e, decoded, pid, clipped) && decoded.frames.size() == 2 && decoded.frames[1] == 0x222 && clipped == 0,
        "stack x86");
    put<DWORD>(data, 8, 0);
    check(!decode_stack(e, decoded, pid, clipped), "stack pid zero rejected");
    unsigned long long dummy = 0;
    check(!field(e, static_cast<size_t>(-1), dummy), "overflow-safe field range");
}
static void check_aggregation()
{
    using namespace bh_cpu_profiler::detail;
    aggregate a;
    a.add({"A", "A", "B"}); a.add({"B"}); a.add({});
    check(a.samples == 2 && a.functions["A"].self == 1 && a.functions["A"].inclusive == 1, "recursion inclusive deduplicated");
    check(a.functions["B"].self == 1 && a.functions["B"].inclusive == 2, "caller inclusive");
    check(a.tree[std::vector<std::string>{"B"}] == 2 && a.tree[std::vector<std::string>{"B", "A", "A"}] == 1,
        "tree recursion preserved");
    check(a.paths[std::vector<std::string>{"B", "A", "A"}] == 1, "hot path count");
    const auto functions = report(a, "functions", "ok: profile");
    check(functions.find("A self=1(50.00%) incl=1(50.00%)") != std::string::npos, "function percentages");
    check(report(a, "functions", "ok: profile", 10000).find("self_cpu_ms_est=1.00 incl_cpu_ms_est=2.00") != std::string::npos,
        "sample interval converted from 100ns to CPU milliseconds");
    check(functions.find("cpu_ms_est=") == std::string::npos, "unknown interval suppresses timing estimates");
    check(report(a, "calltree", "ok: profile").find("B>A>A samples=1") != std::string::npos, "tree path orientation");
    check(report(a, "hotpaths", "ok: profile").find("B>A>A samples=1") != std::string::npos, "hot path orientation");
    aggregate partial;
    partial.add({"user_leaf", "user_root"}, "kernel_sample_ip");
    check(partial.functions["kernel_sample_ip"].self == 1 && partial.functions["user_leaf"].self == 0,
        "self IP authoritative for partial stack");
    check(partial.paths.count(std::vector<std::string>{"user_root", "user_leaf"}) == 1 &&
        partial.paths.count(std::vector<std::string>{"user_root", "user_leaf", "kernel_sample_ip"}) == 0,
        "no fabricated cross-fragment edge");
    aggregate bounded;
    for (size_t i = 0; i < 8300; ++i)
    {
        const auto name = std::to_string(i) + std::string(80, 'x');
        bounded.add(std::vector<std::string>(64, name));
    }
    check(bounded.functions.size() == 8192 && bounded.paths.size() == 8192 && bounded.tree.size() == 8192, "aggregation caps");
    check(bounded.dropped_functions > 0 && bounded.dropped_paths > 0 && bounded.dropped_tree > 0 && bounded.depth_clipped > 0,
        "aggregation omissions counted");
    for (const char* verb : {"functions", "calltree", "hotpaths"})
    {
        const auto output = report(bounded, verb, "ok: profile");
        check(output.size() < 7500 && output.find('\n') == std::string::npos && output.find('\r') == std::string::npos, "single-line byte bound");
        check(output.find("rows_limit=20 depth_limit=12") != std::string::npos && output.find("truncated_rows=") != std::string::npos &&
            output.find("dropped_tree_contributions=") != std::string::npos, "explicit report caps");
    }
    check(clean("unsafe\nname;|") == "unsafe_name__", "name sanitation");
}
static void check_idle()
{
    bh_cpu_profiler::session s;
    for (const char* verb : {"status", "stop"})
        check(s.command(std::string("profile ") + verb).find("ok: profile state=idle") == 0, "idle control");
    for (const char* verb : {"functions", "calltree", "hotpaths"})
        check(s.command(std::string("profile ") + verb).find("error:") == 0, "report without session");
    check(s.command("profile start 0").find("error:") == 0, "invalid start never opens a process");
}
// Explicit optional integration: profiles ONLY this newly launched test executable.
// No active application is attached or modified. Permission failure is a failure,
// not a synthetic pass. The caller must deliberately run --live in an elevated
// shell with ETW system profile permission. Normal invocation NEVER starts ETW.
static int live()
{
    bh_cpu_profiler::session s;
    const auto start = s.command("profile start " + std::to_string(GetCurrentProcessId()) + " 2");
    std::cout << start << '\n';
    check(start.find("ok:") == 0, "live ETW start (see Windows error above)");
    const auto until = GetTickCount64() + 2300;
    volatile unsigned long long work = 1;
    while (GetTickCount64() < until) work = work * 1664525 + 1013904223;
    (void)work;
    const auto status = s.command("profile stop");
    std::cout << status << '\n';
    check(status.find("state=stopped") != std::string::npos && status.find("stop_error=0") != std::string::npos,
        "live ETW cleanup");
    const auto output = s.command("profile functions");
    std::cout << output << '\n';
    check(output.find("shown=0") == std::string::npos && output.find("error:") != 0 && output.size() < 7500,
        "live real samples required");
    return 0;
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--live") return live();
        if (argc != 1) throw std::runtime_error("usage: test_cpu_profiler [--live]");
        check_parser(); check_events(); check_aggregation(); check_idle();
        std::cout << "PASS: CPU profiler deterministic backend tests; live ETW NOT RUN (explicit --live required)\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
