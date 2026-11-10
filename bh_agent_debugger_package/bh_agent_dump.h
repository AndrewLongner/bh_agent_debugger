#pragma once

#include "../bh_agent_dump_support.h"
#include "../bh_agent_dump_registers.h"
#include "../bh_agent_dump_launch_target.h"
#include "../bh_agent_dkm_thread_scope.h"
#include <vsdebugeng.h>
#include <vsdebugeng.templates.h>
#include <algorithm>

// All dump operations stay in DKM/DTE. Never route a historical dump PID to the
// live engine's process-handle, trap, or execution-control implementation.
class bh_agent_dump_session
{
    using process_type = Microsoft::VisualStudio::Debugger::DkmProcess;
    using thread_type = Microsoft::VisualStudio::Debugger::DkmThread;
    using monitor_type = Microsoft::VisualStudio::Debugger::DkmBaseDebugMonitorId;
    template<class T> using array_type = Microsoft::VisualStudio::Debugger::CAutoDkmArray<T>;

    static bool is_dump(process_type* process)
    {
        return process && (process->BaseDebugMonitorId() == monitor_type::DumpFile ||
            process->BaseDebugMonitorId() == monitor_type::DumpFileInterop);
    }

    static std::string failure(const char* operation, HRESULT hr)
    {
        return std::string("error: dump ") + operation + " hr=" + bh_dump::hex(static_cast<UINT32>(hr), 8);
    }

    static std::string utf8(const wchar_t* text)
    {
        if (!text) return "<unavailable>";
        const int length = static_cast<int>(wcslen(text));
        const int count = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
        std::string result(count, '\0');
        if (count) WideCharToMultiByte(CP_UTF8, 0, text, length, &result[0], count, nullptr, nullptr);
        for (char& c : result) if (c == '\r' || c == '\n' || c == '|' || c == '\t') c = ' ';
        return result;
    }

    static DWORD thread_id(thread_type* thread)
    {
        return thread && thread->SystemPart() ? thread->SystemPart()->Id : 0;
    }

    static unsigned pointer_width(process_type* process)
    {
        if (!process || !process->SystemInformation()) return 0;
        const UINT16 arch = process->SystemInformation()->ProcessorArchitecture();
        return arch == PROCESSOR_ARCHITECTURE_AMD64 ? 8 : arch == PROCESSOR_ARCHITECTURE_INTEL ? 4 : 0;
    }

    static HRESULT registers(thread_type* thread, unsigned width, UINT64& ip, UINT64& sp, std::string& text)
    {
        return bh_dump_registers::read(thread, width, ip, sp, text);
    }

    static std::string read_memory(process_type* process, const bh_dump::memory_request& request)
    {
        std::vector<BYTE> bytes(request.size);
        UINT32 read = 0;
        const HRESULT hr = process->ReadMemory(request.address,
            Microsoft::VisualStudio::Debugger::DkmReadMemoryFlags::None, bytes.data(), request.size, &read);
        if (FAILED(hr) || read != request.size)
        {
            const bool unavailable = hr == E_INVALID_MEMORY_ADDRESS || hr == HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY) || SUCCEEDED(hr);
            return failure(unavailable ? "memory unavailable (range not captured or inaccessible)" : "memory read failed",
                FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY));
        }
        return "ok: dump memory address=" + bh_dump::hex(request.address) + " size=" + std::to_string(read) +
            " format=" + request.format + " | " + bh_dump::format_memory(bytes.data(), read, request);
    }

    static std::string modules(process_type* process)
    {
        CComPtr<Microsoft::VisualStudio::Debugger::Native::DkmNativeRuntimeInstance> runtime;
        HRESULT hr = process->GetNativeRuntimeInstance(&runtime);
        if (FAILED(hr) || !runtime) return failure("native runtime unavailable", hr);
        array_type<Microsoft::VisualStudio::Debugger::Native::DkmNativeModuleInstance*> list;
        hr = runtime->GetNativeModuleInstances(&list);
        if (FAILED(hr)) return failure("module enumeration failed", hr);
        std::string out = "modules: session=dump count=" + std::to_string(list.Length) + " | num base name";
        for (UINT32 i = 0; i < list.Length && i < 1000; ++i)
        {
            auto module = list.Members[i];
            if (!module) continue;
            out += " | #" + std::to_string(i) + " " + bh_dump::hex(module->BaseAddress()) + " " +
                utf8(module->Name() ? module->Name()->Value() : nullptr);
        }
        return out;
    }

    static std::string stack(::Thread* thread, const std::string& argument)
    {
        UINT64 maximum = 16;
        if (!argument.empty() && (!bh_dump::parse_uint(argument, 128, maximum) || !maximum))
            return "error: dump stack usage: where [1..128]";
        if (!thread) return "error: dump selected thread unavailable";
        CComPtr<::StackFrames> frames;
        HRESULT hr = thread->get_StackFrames(&frames);
        long count = 0;
        if (FAILED(hr) || !frames || FAILED(hr = frames->get_Count(&count))) return failure("stack unavailable", hr);
        std::string out = "ok: dump stack count=" + std::to_string(count);
        for (long i = 1; i <= count && i <= static_cast<long>(maximum); ++i)
        {
            CComPtr<::StackFrame> frame;
            CComBSTR name;
            if (SUCCEEDED(frames->Item(CComVariant(i), &frame)) && frame) (void)frame->get_FunctionName(&name);
            out += " | #" + std::to_string(i - 1) + " " + bh_dump::stack_function_name(name && name.Length() ? utf8(name) : std::string());
        }
        if (!count) out += " | stack/symbols unavailable in captured dump";
        return out;
    }

    static HRESULT get_processes(::Debugger* debugger, array_type<process_type*>& processes)
    {
        const HRESULT hr = process_type::GetProcesses(&processes);
        if (hr == RPC_E_DISCONNECTED)
        {
            dbgDebugMode mode = dbgDesignMode;
            if (debugger && SUCCEEDED(debugger->get_CurrentMode(&mode)) && mode == dbgDesignMode)
                return S_OK; // DKM has no connection until the first debug session.
        }
        return hr;
    }

    static std::string open(IServiceProvider* provider, ::Debugger* debugger, const std::string& argument, bh_agent_dkm_thread_scope& dkm_scope)
    {
        dbgDebugMode mode = dbgDesignMode;
        HRESULT hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr)) return failure("mode query failed", hr);
        array_type<process_type*> existing;
        hr = get_processes(debugger, existing);
        if (FAILED(hr)) return failure("session discovery failed", hr);
        if (mode != dbgDesignMode || existing.Length) return "error: open dump refused: active debugging session";
        std::wstring path;
        if (!bh_dump::parse_path(argument, path)) return "error: open dump usage: open dump \"<absolute local .dmp path>\"";
        bh_dump::dump_info info;
        std::string error;
        if (!bh_dump::validate_file(path, info, error)) return error;
        // Recheck immediately before launch: validation must never authorize replacing user work.
        if (FAILED(hr = debugger->get_CurrentMode(&mode))) return failure("mode query failed", hr);
        if (mode != dbgDesignMode) return "error: open dump refused: active debugging session";
        CComPtr<IVsDebugger2> launcher;
        hr = provider->QueryService(SID_SVsShellDebugger, __uuidof(IVsDebugger2), reinterpret_cast<void**>(&launcher));
        if (FAILED(hr) || !launcher) return failure("native launcher unavailable", hr);
        const GUID native_engine = {0x3b476d35, 0xa401, 0x11d2, {0xaa, 0xd4, 0x00, 0xc0, 0x4f, 0x99, 0x01, 0x71}};
        bh_dump_launch_target<VsDebugTargetInfo2> target(path.c_str(), native_engine);
        if (!target.valid()) return failure("launch allocation failed", E_OUTOFMEMORY);
        target.value.dlo = DLO_CreateProcess;
        target.value.LaunchFlags = DBGLAUNCH_Silent;
        hr = launcher->LaunchDebugTargets2(1, &target.value);
        if (FAILED(hr))
        {
            CComPtr<IErrorInfo> detail;
            CComBSTR description;
            if (SUCCEEDED(GetErrorInfo(0, &detail)) && detail) (void)detail->GetDescription(&description);
            return failure("native launch failed", hr) + (description ? " | " + utf8(description) : std::string());
        }
        if (FAILED(hr = dkm_scope.ensure())) return failure("opened but inspection initialization failed", hr);
        const ULONGLONG deadline = GetTickCount64() + 20000;
        do
        {
            array_type<process_type*> processes;
            if (SUCCEEDED(process_type::GetProcesses(&processes)) &&
                SUCCEEDED(debugger->get_CurrentMode(&mode)) && mode == dbgBreakMode)
            {
                for (UINT32 i = 0; i < processes.Length; ++i)
                {
                    if (is_dump(processes.Members[i]) && pointer_width(processes.Members[i]))
                    {
                        CComPtr<Microsoft::VisualStudio::Debugger::Native::DkmNativeRuntimeInstance> runtime;
                        if (FAILED(processes.Members[i]->GetNativeRuntimeInstance(&runtime)) || !runtime) continue;
                        return "ok: opened dump session=dump readOnly=true architecture=" + std::string(info.architecture == 9 ? "x64" : "x86") +
                            " exception=" + bh_dump::hex(info.exception_code, 8) + " exceptionTid=" + std::to_string(info.exception_thread) +
                            " exceptionAddress=" + bh_dump::hex(info.exception_address);
                    }
                }
            }
            Sleep(100);
        } while (GetTickCount64() < deadline);
        return "error: dump launch did not reach a verified native dump break session; inspect VS (no automatic retry/stop)";
    }

public:
    static HRESULT get_session_counts(::Debugger* debugger, unsigned& process_count, unsigned& dump_count)
    {
        process_count = 0;
        dump_count = 0;
        array_type<process_type*> processes;
        const HRESULT hr = get_processes(debugger, processes);
        if (FAILED(hr)) return hr;
        process_count = processes.Length;
        for (UINT32 i = 0; i < processes.Length; ++i)
        {
            if (is_dump(processes.Members[i])) ++dump_count;
        }
        return S_OK;
    }

    // false is permitted ONLY for live sessions or explicitly whitelisted DTE reads/stop.
    static bool try_handle(IServiceProvider* provider, const char* message, std::string& response, bh_agent_dkm_thread_scope& dkm_scope)
    {
        const std::string command = bh_dump::trim(message ? message : "");
        const bool opening = bh_dump::command_is(command, "open dump");
        CComPtr<_DTE> dte;
        CComPtr<::Debugger> debugger;
        HRESULT hr = E_NOINTERFACE;
        if (!provider || FAILED(hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte))) ||
            !dte || FAILED(hr = dte->get_Debugger(&debugger)) || !debugger)
        {
            response = failure("DTE unavailable", hr);
            return true;
        }
        array_type<process_type*> processes;
        hr = get_processes(debugger, processes);
        if (FAILED(hr))
        {
            response = failure("session discovery failed; live routing disabled", hr);
            return true;
        }
        CComPtr<process_type> process;
        unsigned dumps = 0;
        for (UINT32 i = 0; i < processes.Length; ++i)
        {
            if (is_dump(processes.Members[i])) { process = processes.Members[i]; ++dumps; }
        }
        if (!opening && !dumps) return false;
        if (opening)
        {
            response = open(provider, debugger, bh_dump::trim(command.substr(9)), dkm_scope);
            return true;
        }
        if (_stricmp(command.c_str(), "stop") == 0 || _stricmp(command.c_str(), "stop debugging") == 0) return false;
        if (dumps != 1 || processes.Length != 1)
        {
            response = "error: ambiguous dump session; inspection requires exactly one debugged process";
            return true;
        }
        dbgDebugMode mode = dbgDesignMode;
        if (FAILED(hr = debugger->get_CurrentMode(&mode)) || mode != dbgBreakMode)
        {
            response = "error: dump inspection requires break mode";
            return true;
        }
        if (bh_dump::dte_read_only_command(command)) return false;
        if (FAILED(hr = dkm_scope.ensure()))
        {
            response = failure("inspection initialization failed", hr);
            return true;
        }
        const unsigned width = pointer_width(process);
        if (!width)
        {
            response = "error: unsupported dump architecture (native x86/x64 required)";
            return true;
        }
        if (_stricmp(command.c_str(), "modules") == 0)
        {
            response = modules(process);
            return true;
        }
        if (bh_dump::command_is(command, "read mem"))
        {
            bh_dump::memory_request request;
            response = bh_dump::parse_memory(bh_dump::trim(command.substr(8)), width, request) ? read_memory(process, request) :
                "error: read mem usage: read mem <address> <1..4096 bytes> [hex|ascii|u32|u64|ptr]; typed sizes must align";
            return true;
        }
        CComPtr<::Thread> current;
        long selected = 0;
        if (SUCCEEDED(debugger->get_CurrentThread(&current)) && current) (void)current->get_ID(&selected);
        array_type<thread_type*> threads;
        hr = process->GetThreads(&threads);
        if (FAILED(hr)) { response = failure("threads unavailable", hr); return true; }
        CComPtr<thread_type> selected_thread;
        for (UINT32 i = 0; i < threads.Length; ++i)
            if (thread_id(threads.Members[i]) == selected) selected_thread = threads.Members[i];
        if (_stricmp(command.c_str(), "list threads") == 0)
        {
            std::vector<DWORD> ids;
            for (UINT32 i = 0; i < threads.Length; ++i) if (thread_id(threads.Members[i])) ids.push_back(thread_id(threads.Members[i]));
            std::sort(ids.begin(), ids.end());
            response = "threads: session=dump count=" + std::to_string(ids.size()) + " selected=" + std::to_string(selected);
            for (size_t i = 0; i < ids.size(); ++i)
                response += " | #" + std::to_string(i) + (ids[i] == selected ? " * tid=" : " - tid=") + std::to_string(ids[i]);
            return true;
        }
        if (bh_dump::command_is(command, "select thread"))
        {
            UINT64 requested = 0;
            if (!bh_dump::parse_uint(bh_dump::trim(command.substr(13)), UINT32_MAX, requested) || !requested)
            { response = "error: select thread usage: select thread <tid>"; return true; }
            bool exists = false;
            for (UINT32 i = 0; i < threads.Length; ++i) if (thread_id(threads.Members[i]) == requested) exists = true;
            if (!exists) { response = "error: thread id not found in dump"; return true; }
            CComPtr<::Processes> dte_processes;
            CComPtr<::Process> dte_process;
            CComPtr<::Programs> programs;
            long program_count = 0;
            hr = debugger->get_DebuggedProcesses(&dte_processes);
            if (SUCCEEDED(hr) && dte_processes) hr = dte_processes->Item(CComVariant(1L), &dte_process);
            if (SUCCEEDED(hr) && dte_process) hr = dte_process->get_Programs(&programs);
            if (SUCCEEDED(hr) && programs) hr = programs->get_Count(&program_count);
            for (long p = 1; SUCCEEDED(hr) && p <= program_count; ++p)
            {
                CComPtr<::Program> program;
                CComPtr<::Threads> dte_threads;
                long count = 0;
                if (FAILED(programs->Item(CComVariant(p), &program)) || !program ||
                    FAILED(program->get_Threads(&dte_threads)) || !dte_threads || FAILED(dte_threads->get_Count(&count))) continue;
                for (long i = 1; i <= count; ++i)
                {
                    CComPtr<::Thread> candidate;
                    long id = 0;
                    if (SUCCEEDED(dte_threads->Item(CComVariant(i), &candidate)) && candidate &&
                        SUCCEEDED(candidate->get_ID(&id)) && static_cast<DWORD>(id) == requested)
                    {
                        hr = debugger->put_CurrentThread(candidate);
                        response = FAILED(hr) ? failure("thread selection failed", hr) : "ok: selected dump thread tid=" + std::to_string(static_cast<DWORD>(id));
                        return true;
                    }
                }
            }
            response = "error: dump thread unavailable through DTE";
            return true;
        }
        if (bh_dump::command_is(command, "where") || bh_dump::command_is(command, "stack"))
        {
            response = stack(current, bh_dump::trim(command.substr(5)));
            return true;
        }
        UINT64 ip = 0, sp = 0;
        std::string regs;
        if (_stricmp(command.c_str(), "status") == 0 || bh_dump::command_is(command, "poll break"))
        {
            hr = registers(selected_thread, width, ip, sp, regs);
            response = "ok: paused break session=dump readOnly=true architecture=" + std::string(width == 8 ? "x64" : "x86") +
                " tid=" + std::to_string(selected) + (SUCCEEDED(hr) ? " ip=" + bh_dump::hex(ip) : " registers=unavailable");
            return true;
        }
        if (_stricmp(command.c_str(), "read regs") == 0 || bh_dump::command_is(command, "read stack"))
        {
            hr = registers(selected_thread, width, ip, sp, regs);
            if (FAILED(hr)) { response = failure(selected_thread ? "register context read failed or not captured" : "selected thread unavailable", hr); return true; }
            if (_stricmp(command.c_str(), "read regs") == 0) response = "ok: dump regs tid=" + std::to_string(selected) + " " + regs;
            else
            {
                const std::string size = bh_dump::trim(command.substr(10));
                bh_dump::memory_request request;
                response = bh_dump::parse_memory(bh_dump::hex(sp) + " " + (size.empty() ? "128" : size), width, request) ?
                    read_memory(process, request) : "error: read stack usage: read stack [1..4096 bytes]";
            }
            return true;
        }
        // Default deny is intentional: future engine commands must not escape this gate.
        response = "error: command unsupported in read-only dump session (no execution, traps, queues, or breakpoint mutation)";
        return true;
    }
};
