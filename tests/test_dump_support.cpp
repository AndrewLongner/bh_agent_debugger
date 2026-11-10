#define NOMINMAX
#include "../bh_agent_dump_support.h"
#include "../bh_agent_dump_registers.h"
#include "../bh_agent_dump_launch_target.h"
#include "../bh_agent_help.h"
#include "../bh_agent_start_arguments.h"
#include "../bh_agent_command_chain.h"
#include "../bh_agent_symbol_search_budget.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

static void check(bool condition, const char* name)
{
    if (!condition) throw std::runtime_error(name);
}

struct context_reader
{
    bool x64;
    HRESULT result = S_OK;
    HRESULT GetContext(UINT32 flags, void* data, UINT32 size)
    {
        check(flags == static_cast<UINT32>(x64 ? CONTEXT_CONTROL | CONTEXT_INTEGER : WOW64_CONTEXT_CONTROL | WOW64_CONTEXT_INTEGER), "context flags");
        check(size == (x64 ? sizeof(CONTEXT) : sizeof(WOW64_CONTEXT)), "context size");
        if (FAILED(result)) return result;
        if (x64)
        {
            auto context = static_cast<CONTEXT*>(data);
            context->Rip = 0x1234567812345678;
            context->Rsp = 0xabcdef0098765432;
            context->R15 = 0xfedcba9876543210;
        }
        else
        {
            auto context = static_cast<WOW64_CONTEXT*>(data);
            context->Eip = 0xfedcba98;
            context->Esp = 0x87654321;
        }
        return result;
    }
};

static void check_startup_argument_codec()
{
    const std::string input = u8"--control-pipe \\\"pipe name; && Ω猫\\\"";
    std::string encoded;
    std::string decoded;
    check(bh_startup_arguments::try_encode(input, encoded), "encode launch arguments");
    check(encoded.find_first_not_of("0123456789abcdef") == std::string::npos, "lowercase hex grammar");
    check(bh_startup_arguments::try_decode(encoded, decoded) && decoded == input, "launch arguments round trip");
    check(bh_startup_arguments::try_encode("", encoded) && encoded == "-", "empty launch arguments sentinel");
    check(bh_startup_arguments::try_decode(encoded, decoded) && decoded.empty(), "empty launch arguments round trip");

    for (const char* malformed : {"", "0", "GG", "c0af", "eda080", "f4908080"})
    {
        check(!bh_startup_arguments::try_decode(malformed, decoded), "reject malformed launch arguments");
    }
    check(!bh_startup_arguments::try_decode(std::string(bh_startup_arguments::max_argument_bytes * 2U + 2U, '0'), decoded),
        "reject oversized encoded launch arguments");
    check(!bh_startup_arguments::try_encode(std::string(bh_startup_arguments::max_argument_bytes + 1U, 'a'), encoded),
        "reject oversized launch arguments");
    check(!bh_startup_arguments::try_encode(std::string("a\0b", 3), encoded), "reject embedded nul launch arguments");
}

static void check_command_chain()
{
    std::vector<std::string> commands;

    bh_agent_command_chain::split(R"(eval "left;right && \"quoted\""; status)", commands);
    check(commands.size() == 2, "escaped quote chain count");
    check(commands[0] == R"(eval "left;right && \"quoted\"")", "escaped quote separators remain quoted");
    check(commands[1] == "status", "escaped quote trailing command");

    bh_agent_command_chain::split(R"(eval "\"; still quoted" && status)", commands);
    check(commands.size() == 2, "escaped opening content chain count");
    check(commands[0] == R"(eval "\"; still quoted")", "escaped quote does not end quoted content");

    bh_agent_command_chain::split(R"(eval "single; && command")", commands);
    check(commands.size() == 1 && commands[0] == R"(eval "single; && command")",
        "quoted separators do not create a chain");
}

static void check_symbol_search_budget()
{
    using bh_agent_symbol_search::budget;
    using bh_agent_symbol_search::limit;

    budget symbol_budget(60000, 2, 3);
    check(symbol_budget.try_begin_module(), "symbol budget first module");
    check(symbol_budget.try_examine_symbol() && symbol_budget.try_examine_symbol(), "symbol budget work allowance");
    check(!symbol_budget.try_examine_symbol() && symbol_budget.limit_reason() == limit::symbol_work,
        "symbol budget work limit");
    check(symbol_budget.symbols_examined() == 2 && symbol_budget.modules_examined() == 1,
        "symbol budget work counters");

    budget module_budget(60000, 5, 1);
    check(module_budget.try_begin_module(), "symbol budget module allowance");
    check(!module_budget.try_begin_module() && module_budget.limit_reason() == limit::modules,
        "symbol budget module limit");

    budget time_budget(0, 5, 5);
    check(time_budget.exhausted() && time_budget.limit_reason() == limit::elapsed_time,
        "symbol budget elapsed-time limit");
}

static void check_file_validation()
{
    wchar_t directory[MAX_PATH] = {}, path[MAX_PATH] = {};
    check(GetTempPathW(MAX_PATH, directory) != 0 && GetTempFileNameW(directory, L"bhd", 0, path) != 0, "temp fixture");
    struct cleanup { const wchar_t* path; ~cleanup() { DeleteFileW(path); } } guard = {path};
    struct fixture
    {
        MINIDUMP_HEADER header;
        MINIDUMP_DIRECTORY entry;
        MINIDUMP_SYSTEM_INFO system;
    } data = {};
    data.header.Signature = MINIDUMP_SIGNATURE;
    data.header.Version = MINIDUMP_VERSION;
    data.header.NumberOfStreams = 1;
    data.header.StreamDirectoryRva = offsetof(fixture, entry);
    data.entry.StreamType = SystemInfoStream;
    data.entry.Location.Rva = offsetof(fixture, system);
    data.entry.Location.DataSize = sizeof(data.system);
    auto write = [&]()
    {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(file != INVALID_HANDLE_VALUE, "fixture create");
        DWORD written = 0;
        const bool ok = WriteFile(file, &data, sizeof(data), &written, nullptr) && written == sizeof(data);
        CloseHandle(file);
        check(ok, "fixture write");
    };
    bh_dump::dump_info info;
    std::string error;
    for (WORD arch : {WORD(0), WORD(9)})
    {
        data.system.ProcessorArchitecture = arch;
        write();
        check(bh_dump::validate_file(path, info, error) && info.architecture == arch, "x86/x64 fixture parse");
    }
    data.system.ProcessorArchitecture = 12;
    write();
    check(!bh_dump::validate_file(path, info, error) && error.find("unsupported") != std::string::npos, "reject ARM64");
    data.entry.Location.Rva = UINT32_MAX;
    write();
    check(!bh_dump::validate_file(path, info, error) && error.find("bounds") != std::string::npos, "reject stream overflow");
    data.header.StreamDirectoryRva = UINT32_MAX;
    write();
    check(!bh_dump::validate_file(path, info, error), "reject directory overflow");
    data.header.Signature = 0;
    write();
    check(!bh_dump::validate_file(path, info, error), "reject non-minidump");
    check(!bh_dump::validate_file(std::wstring(path) + L".missing", info, error), "reject missing file");
}

// The pointer fields participating in LaunchDebugTargets2's in/out marshalling.
struct launch_fields
{
    DWORD cbSize;
    BSTR bstrRemoteMachine, bstrExe, bstrArg, bstrCurDir, bstrEnv;
    GUID guidLaunchDebugEngine;
    DWORD dwDebugEngineCount;
    GUID* pDebugEngines;
    BSTR bstrPortName, bstrOptions;
    IUnknown* pUnknown;
};

static void check_launch_ownership()
{
    const GUID engine = {0x3b476d35, 0xa401, 0x11d2, {0xaa, 0xd4, 0, 0xc0, 0x4f, 0x99, 1, 0x71}};
    for (bool replace : {false, true})
    {
        bh_dump_launch_target<launch_fields> target(L"C:\\CrashDumps\\input.dmp", engine);
        check(target.valid() && target.value.cbSize == sizeof(launch_fields), "launch allocation");
        check(target.value.pDebugEngines != &engine && IsEqualGUID(target.value.pDebugEngines[0], engine), "COM-owned engine array");
        check(target.value.dwDebugEngineCount == 1 && IsEqualGUID(target.value.guidLaunchDebugEngine, engine), "explicit native engine");
        if (replace)
        {
            // Emulate an in/out COM proxy replacing nested buffers. The old stack
            // array would corrupt the heap here; an aliased BSTR would double-free.
            CoTaskMemFree(target.value.pDebugEngines);
            target.value.pDebugEngines = static_cast<GUID*>(CoTaskMemAlloc(2 * sizeof(GUID)));
            check(target.value.pDebugEngines != nullptr, "replacement array");
            target.value.dwDebugEngineCount = 2;
            SysFreeString(target.value.bstrExe);
            target.value.bstrExe = SysAllocString(L"C:\\CrashDumps\\returned.dmp");
            target.value.bstrOptions = SysAllocString(L"returned options");
            check(target.valid(), "returned buffer ownership");
        }
    } // Destructor frees exactly the final buffers on both success/failure paths.
}

static void check_help()
{
    std::string response;
    check(!bh_agent_help::try_handle("status", response), "non-help falls through");
    check(bh_agent_help::try_handle("help", response), "help handled");
    check(response.size() <= bh_agent_help::max_response_bytes, "help list bounded");
    check(response.find("where [frames]") != std::string::npos, "help lists canonical symbolic stack");
    check(response.find("read stack [bytes]") != std::string::npos, "help lists raw stack memory");
    check(response.find("queue trap") == std::string::npos, "help omits unsupported queue trap");

    const char* topics[] =
    {
        "help", "status", "modules", "open dump", "poll break", "eval", "locals", "local variables",
        "list threads", "select thread", "where", "call stack", "read regs", "read stack", "stack", "read mem",
        "step", "step into", "step over", "step out", "continue", "start", "start debugging", "break", "async break",
        "stop", "stop debugging", "set bp", "list bp", "remove bp", "clear bp", "clear all bp", "set target pid",
        "trap add", "set trap", "trap add symbol", "set trap symbol", "trap list", "trap remove", "trap clear",
        "queue continue", "queue list", "queue clear", "queue run", "trap resume", "resolve symbol", "chain"
    };
    for (const char* topic : topics)
    {
        const std::string command = std::string("help ") + topic;
        check(bh_agent_help::try_handle(command.c_str(), response), "help topic handled");
        check(response.rfind("ok: help ", 0) == 0, topic);
        check(response.size() <= bh_agent_help::max_response_bytes, "help detail bounded");
    }

    check(bh_agent_help::try_handle("HELP   SET TRAP SYMBOL", response) &&
        response.find("set trap symbol <symbol>") != std::string::npos, "multiword topic normalization");
    check(bh_agent_help::try_handle("help where", response) &&
        response.find("Canonical symbolic call stack") != std::string::npos, "where symbolic detail");
    check(bh_agent_help::try_handle("help stack", response) &&
        response.find("live sessions") != std::string::npos && response.find("dump sessions") != std::string::npos,
        "stack context detail");
    check(bh_agent_help::try_handle("help read stack", response) &&
        response.find("64 bytes live") != std::string::npos && response.find("128 bytes in a dump") != std::string::npos &&
        response.find("256 bytes live") != std::string::npos && response.find("4096 bytes in a dump") != std::string::npos,
        "read stack bounds detail");
    check(bh_agent_help::try_handle("help unknown topic", response) &&
        response.rfind("error: help topic not found:", 0) == 0, "unknown help topic");
    check(bh_agent_help::try_handle("help \x01", response) && response.rfind("error:", 0) == 0,
        "malformed help remains local");

    check(bh_agent_help::try_handle("help", response), "help list restored for chain bound");
    std::string combined = "ok: chain count=100";
    bool bounded = true;
    for (int i = 0; i < 100; ++i)
    {
        const std::string item = " | #" + std::to_string(i) + " " + response;
        if (!bh_agent_help::append_chain_item(combined, item))
        {
            bounded = false;
            break;
        }
    }
    check(!bounded && combined.size() <= bh_agent_help::max_chain_response_bytes, "repeated help chain bounded");

    std::ifstream package("bh_agent_debugger_package/bh_agent_debugger_package.h", std::ios::binary);
    std::ostringstream package_text;
    package_text << package.rdbuf();
    const std::string source = package_text.str();
    const size_t normalize_position = source.find("const std::string normalized_message = bh_dump::trim(message);");
    const size_t help_position = source.find("bh_agent_help::try_handle(message, response)", normalize_position);
    const size_t dkm_position = source.find("bh_agent_dkm_thread_scope dkm_scope;", normalize_position);
    const size_t dump_position = source.find("bh_agent_dump_session::try_handle", normalize_position);
    check(package.good() || package.eof(), "package source readable");
    check(normalize_position != std::string::npos && help_position > normalize_position &&
        dkm_position > help_position && dump_position > dkm_position, "help precedes DKM and dump routing");
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        check_startup_argument_codec();
        check_command_chain();
        check_symbol_search_budget();
        using namespace bh_dump;
        UINT64 value = 0;
        check(parse_uint("18446744073709551615", UINT64_MAX, value) && value == UINT64_MAX, "uint64 maximum");
        for (const char* bad : {"", "-1", "+1", "0x", "18446744073709551616", "12junk", " 12"})
            check(!parse_uint(bad, UINT64_MAX, value), "invalid integer");
        check(!range_valid(UINT64_MAX, 1, UINT64_MAX), "range overflow");
        check(stack_function_name("") == "<symbols unavailable>", "empty stack name");
        check(stack_function_name("00007ffe44502654") == "00007ffe44502654 [symbols unavailable]", "x64 address-only stack");
        check(stack_function_name("0x1234ABCD") == "0x1234ABCD [symbols unavailable]", "x86 address-only stack");
        check(stack_function_name("b2CreateChain") == "b2CreateChain", "resolved stack symbol");
        std::wstring path;
        check(parse_path("\"C:\\space dir\\a; b&&c.dmp\"", path) && path == L"C:\\space dir\\a; b&&c.dmp", "quoted path");
        for (const char* bad : {"relative.dmp", "C:relative.dmp", "C:\\file.txt", "\"C:\\a.dmp", "C:\\a.dmp\ncontinue", "\\\\server\\a.dmp"})
            check(!parse_path(bad, path), "reject invalid path");
        for (const char* cmd : {"continue", "step into", "step over", "step out", "break", "start", "trap resume", "queue continue", "set target pid 34424", "clear bp", "set bp a:1", "eval\t1", "evalx", "locals\t2",
            "eval --side-effects x = 1", "eval  --SIDE-EFFECTS\t1", "eval --side-effects", "eval --side-effects  "})
            check(!dte_read_only_command(cmd), "read-only grammar denies live/unknown commands");
        for (const char* cmd : {"eval 1 + 2", "eval", "eval --side-effectsx", "eval x == 1", "locals", "locals 2", "local variables 4", "list bp"})
            check(dte_read_only_command(cmd), "read-only grammar allows inspection");
        BYTE bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
        memory_request request;
        check(parse_memory("0x1000 8 u64", 8, request) && format_memory(bytes, 8, request) == "0x0807060504030201", "little-endian u64");
        check(parse_memory("0x1000 8 ptr", 4, request) && format_memory(bytes, 8, request) == "0x04030201 0x08070605", "x86 ptr");
        check(parse_memory("0x1000 8 ptr", 8, request) && format_memory(bytes, 8, request) == "0x0807060504030201", "x64 ptr");
        check(parse_memory("0 8", 8, request) && request.format == "hex", "default hex");
        check(parse_memory("0 4 ascii", 8, request) && format_memory(reinterpret_cast<const BYTE*>("A\nZ\0"), 4, request) == "A.Z.", "ascii unavailable characters");
        for (const char* bad : {"0 0", "0 4097", "0 3 u32", "0 7 ptr", "0 4 bad", "0 4 hex extra", "0xffffffffffffffff 2"})
            check(!parse_memory(bad, 8, request), "invalid memory request");
        check(!parse_memory("0xffffffff 2", 4, request), "x86 address overflow");
        for (bool x64 : {false, true})
        {
            context_reader reader = {x64};
            UINT64 ip = 0, sp = 0;
            std::string text;
            check(SUCCEEDED(bh_dump_registers::read(&reader, x64 ? 8 : 4, ip, sp, text)), "context read");
            check(ip == (x64 ? 0x1234567812345678ULL : 0xfedcba98ULL), "context IP width");
            check(sp == (x64 ? 0xabcdef0098765432ULL : 0x87654321ULL), "context SP width");
            check(text.find(x64 ? "r15=0xFEDCBA9876543210" : "eip=0xFEDCBA98") != std::string::npos, "context format");
            reader.result = E_FAIL;
            check(FAILED(bh_dump_registers::read(&reader, x64 ? 8 : 4, ip, sp, text)) && ip == 0 && sp == 0 && text.empty(), "unavailable context not fabricated");
        }
        check_file_validation();
        check_launch_ownership();
        check_help();
        if (argc == 2)
        {
            dump_info info;
            std::string error;
            check(validate_file(argv[1], info, error), error.c_str());
            std::cout << "fixture architecture=" << info.architecture << " exception=" << hex(info.exception_code, 8)
                << " tid=" << info.exception_thread << " address=" << hex(info.exception_address) << '\n';
        }
        std::cout << "PASS dump parser, command-chain quoting, symbol-search budgets, help catalog/bounds/routing, read-only grammar, memory formats, x86/x64 contexts, and COM launch ownership\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
