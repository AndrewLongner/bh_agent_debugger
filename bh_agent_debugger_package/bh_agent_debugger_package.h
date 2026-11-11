#pragma once

#include "bh_agent_debugger_event_handler.h"
#include "guids.h"
#include "..\bh_agent_help.h"
#include "..\bh_agent_command_chain.h"
#include "..\bh_agent_log.h"
#include "..\bh_agent_start_arguments.h"
#include "..\bh_agent_version.h"
#include "..\bh_agent_pipe_security.h"
#include "..\bh_agent_eval_grammar.h"

#include <proffserv.h>
#include <vsdebugeng.h>
#include <vsdebugeng.templates.h>
#include "bh_agent_eval.h"
#include <stdlib.h>
#include <atomic>
#include <string>
#include <string.h>
#include <vector>
#include <tlhelp32.h>
#include "bh_agent_dump.h"

using bh_package_vs_site_cache = VSL::IVsPackageImplDefaults<&CLSID_bh_agent_debugger_package>::VsSiteCache;
using bh_package_load_ui = VSL::LoadUILibraryNoop<bh_package_vs_site_cache>;

class ATL_NO_VTABLE bh_agent_debugger_package :
    public CComObjectRootEx<CComSingleThreadModel>,
    public CComCoClass<bh_agent_debugger_package, &CLSID_bh_agent_debugger_package>,
    public IVsPackageImpl<bh_agent_debugger_package, &CLSID_bh_agent_debugger_package, bh_package_vs_site_cache, bh_package_load_ui>,
    public IAsyncLoadablePackageInitialize,
    public IServiceProvider,
    public IVsDebuggerEvents,
    public ATL::ISupportErrorInfoImpl<&__uuidof(IVsPackage)>
{
public:
    BEGIN_COM_MAP(bh_agent_debugger_package)
        COM_INTERFACE_ENTRY(IVsPackage)
        COM_INTERFACE_ENTRY(IAsyncLoadablePackageInitialize)
        COM_INTERFACE_ENTRY(IServiceProvider)
        COM_INTERFACE_ENTRY(IVsDebuggerEvents)
        COM_INTERFACE_ENTRY(ISupportErrorInfo)
    END_COM_MAP()

    VSL_DECLARE_NOT_COPYABLE(bh_agent_debugger_package)

public:

    bh_agent_debugger_package()
    {
    }

    ~bh_agent_debugger_package()
    {
    }

    void PostSited(IVsPackageEnums::SetSiteResult)
    {
        m_is_stopping.store(false);

        HRESULT hr = EnsureEventHandlerReady();
        if (FAILED(hr))
        {
            bh_agent_logf("package.core", "EnsureEventHandlerReady failed hr=0x%08X", (unsigned)hr);
            return;
        }

        hr = SubscribeDebuggerEvents();
        if (FAILED(hr))
        {
            bh_agent_logf("package.core", "SubscribeDebuggerEvents failed hr=0x%08X", (unsigned)hr);
        }

        if (!m_eval_dispatcher.start())
        {
            bh_agent_logf("package.core", "eval main-thread dispatcher start failed err=%lu", (unsigned long)GetLastError());
        }

        StartAgentPipeServer();

        ProfferServiceUtilities utility(GetVsSiteCache());
        utility.ProfferService(bh_guid_vs_service, static_cast<IServiceProvider*>(this));
    }

    STDMETHOD(QueryService)(REFGUID guidService, REFIID riid, void** ppvObject) override
    {
        if (!ppvObject)
        {
            return E_POINTER;
        }
        *ppvObject = nullptr;

        if (guidService == bh_guid_vs_service)
        {
            return m_event_handler ? m_event_handler->QueryInterface(riid, ppvObject) : E_UNEXPECTED;
        }

        return E_NOINTERFACE;
    }

    void PreClosing()
    {
        m_is_stopping.store(true);
        UnsubscribeDebuggerEvents();

        bool stopped = StopAgentPipeServer();
        m_eval_dispatcher.stop();
        if (!stopped)
        {
            bh_agent_logf("package.core", "PreClosing deferred handler release because worker threads are still active");
            return;
        }

        m_state_lock.Lock();
        m_event_handler_provider.Release();
        m_event_handler.Release();
        m_state_lock.Unlock();
    }

    STDMETHOD(Initialize)(
        IAsyncServiceProvider* pServiceProvider,
        IProfferAsyncService* pProfferService,
        IAsyncProgressCallback* pProgressCallback,
        IVsTask** ppTask) override
    {
        UNREFERENCED_PARAMETER(pServiceProvider);
        UNREFERENCED_PARAMETER(pProfferService);
        UNREFERENCED_PARAMETER(pProgressCallback);

        if (!ppTask)
        {
            return E_POINTER;
        }

        *ppTask = nullptr;
        return EnsureEventHandlerReady();
    }

    STDMETHOD(OnModeChange)(DBGMODE dbgmodeNew) override
    {
        // Publish the mode before bumping a generation: a reader that sees the new
        // generation must also see the new mode.
        m_vs_event_mode.store(static_cast<int>(dbgmodeNew));
        if (dbgmodeNew == DBGMODE_Run)
        {
            const unsigned long long generation = m_vs_run_generation.fetch_add(1) + 1;
            bh_agent_logf("package.debug", "debugger mode=run generation=%llu", generation);
        }
        else if (dbgmodeNew == DBGMODE_Break)
        {
            const unsigned long long generation = m_vs_break_generation.fetch_add(1) + 1;
            bh_agent_logf("package.debug", "debugger mode=break generation=%llu", generation);
        }
        else if (dbgmodeNew == DBGMODE_Design)
        {
            const unsigned long long generation = m_vs_design_generation.fetch_add(1) + 1;
            bh_agent_logf("package.debug", "debugger mode=design generation=%llu", generation);
        }
        else
        {
            bh_agent_logf("package.debug", "debugger mode=%u", (unsigned)dbgmodeNew);
        }

        return S_OK;
    }

    static HRESULT WINAPI UpdateRegistry(BOOL bRegister)
    {
        UNREFERENCED_PARAMETER(bRegister);
        return S_OK;
    }

private:
    HRESULT SubscribeDebuggerEvents()
    {
        if (m_debugger_events_cookie != VSCOOKIE_NIL)
        {
            return S_OK;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            return E_UNEXPECTED;
        }

        CComPtr<IVsDebugger> shell_debugger;
        HRESULT hr = provider->QueryService(
            SID_SVsShellDebugger,
            __uuidof(IVsDebugger),
            reinterpret_cast<void**>(&shell_debugger));
        if (FAILED(hr) || !shell_debugger)
        {
            return FAILED(hr) ? hr : E_NOINTERFACE;
        }

        VSCOOKIE cookie = VSCOOKIE_NIL;
        hr = shell_debugger->AdviseDebuggerEvents(this, &cookie);
        if (FAILED(hr))
        {
            return hr;
        }

        m_shell_debugger = shell_debugger;
        m_debugger_events_cookie = cookie;
        m_debugger_events_ready.store(true);

        DBGMODE mode = DBGMODE_Design;
        hr = shell_debugger->GetMode(&mode);
        if (SUCCEEDED(hr))
        {
            (void)OnModeChange(mode);
        }

        bh_agent_logf("package.debug", "debugger mode events subscribed cookie=%lu", (unsigned long)cookie);
        return S_OK;
    }

    void UnsubscribeDebuggerEvents()
    {
        m_debugger_events_ready.store(false);

        if (m_shell_debugger && m_debugger_events_cookie != VSCOOKIE_NIL)
        {
            HRESULT hr = m_shell_debugger->UnadviseDebuggerEvents(m_debugger_events_cookie);
            if (FAILED(hr))
            {
                bh_agent_logf("package.debug", "UnadviseDebuggerEvents failed hr=0x%08X", (unsigned)hr);
            }
        }

        m_debugger_events_cookie = VSCOOKIE_NIL;
        m_shell_debugger.Release();
    }

    bool WaitForVsStepCompletion(
        ::Debugger* debugger,
        unsigned long long initial_run_generation,
        unsigned long long initial_break_generation,
        unsigned long long initial_design_generation,
        std::string& response)
    {
        if (!debugger)
        {
            response = "error: debugger unavailable while waiting for step completion";
            return false;
        }

        const DWORD timeout_ms = 10000;
        const DWORD stable_break_ms = 100;
        const DWORD poll_interval_ms = 25;
        const ULONGLONG start_tick = GetTickCount64();
        ULONGLONG stable_break_tick = 0;
        unsigned long long stable_break_generation = initial_break_generation;
        unsigned long long stable_run_generation = initial_run_generation;
        bool saw_run_transition = false;
        bool saw_engine_step_completion = false;

        while ((GetTickCount64() - start_tick) <= timeout_ms)
        {
            if (m_is_stopping.load())
            {
                response = "error: package stopping while waiting for step completion";
                return false;
            }

            const unsigned long long run_generation = m_vs_run_generation.load();
            const unsigned long long break_generation = m_vs_break_generation.load();
            const unsigned long long design_generation = m_vs_design_generation.load();

            if (run_generation > initial_run_generation)
            {
                if (!saw_run_transition || run_generation != stable_run_generation)
                {
                    stable_break_tick = 0;
                    stable_run_generation = run_generation;
                }
                saw_run_transition = true;
            }

            if (!saw_run_transition && break_generation > initial_break_generation)
            {
                dbgDebugMode mode = dbgDesignMode;
                if (SUCCEEDED(debugger->get_CurrentMode(&mode)) && mode == dbgBreakMode)
                {
                    std::string engine_status;
                    ForwardMessageToRemotePipe("status", engine_status);
                    if (engine_status.find("paused break") != std::string::npos &&
                        engine_status.find("reason=step") != std::string::npos)
                    {
                        saw_run_transition = true;
                        saw_engine_step_completion = true;
                        stable_break_tick = 0;
                        stable_run_generation = run_generation;
                        bh_agent_logf(
                            "package.pipe",
                            "step run transition confirmed by DKM status: %s",
                            engine_status.c_str());
                    }
                }
            }

            if (saw_run_transition && design_generation > initial_design_generation)
            {
                response = "error: debugging ended while waiting for step completion";
                return false;
            }

            if (saw_run_transition && break_generation > initial_break_generation)
            {
                const ULONGLONG now_tick = GetTickCount64();
                if (stable_break_tick == 0 ||
                    break_generation != stable_break_generation ||
                    run_generation != stable_run_generation)
                {
                    stable_break_tick = now_tick;
                    stable_break_generation = break_generation;
                    stable_run_generation = run_generation;
                }
                else if ((now_tick - stable_break_tick) >= stable_break_ms)
                {
                    dbgDebugMode mode = dbgDesignMode;
                    HRESULT hr = debugger->get_CurrentMode(&mode);
                    if (SUCCEEDED(hr) && mode == dbgBreakMode)
                    {
                        bh_agent_logf(
                            "package.pipe",
                            "step stabilized runGeneration=%llu breakGeneration=%llu elapsedMs=%llu source=%s",
                            run_generation,
                            break_generation,
                            (unsigned long long)(now_tick - start_tick),
                            saw_engine_step_completion ? "dkm-step-complete" : "vs-run-event");
                        return true;
                    }

                    if (SUCCEEDED(hr) && mode == dbgDesignMode)
                    {
                        response = "error: debugging ended while waiting for stable break state";
                        return false;
                    }

                    stable_break_tick = 0;
                }
            }

            if (m_pipe_stop_event)
            {
                if (WaitForSingleObject(m_pipe_stop_event, poll_interval_ms) == WAIT_OBJECT_0)
                {
                    response = "error: package stopping while waiting for step completion";
                    return false;
                }
            }
            else
            {
                Sleep(poll_interval_ms);
            }
        }

        bh_agent_logf(
            "package.pipe",
            "step stabilization timeout runGeneration=%llu->%llu breakGeneration=%llu->%llu designGeneration=%llu->%llu",
            initial_run_generation,
            m_vs_run_generation.load(),
            initial_break_generation,
            m_vs_break_generation.load(),
            initial_design_generation,
            m_vs_design_generation.load());
        response = saw_run_transition
            ? "error: step stabilization timeout waiting for stable break state"
            : "error: step stabilization timeout waiting for debugger to leave break state";
        return false;
    }

    // Waits until the IVsDebuggerEvents run event arrives after a continue. DTE reports
    // run mode as soon as Debug.Start is accepted, but VS only accepts Debug.BreakAll
    // once this event has fired (0.4-1.0 s later).
    bool WaitForVsRunReadiness(
        unsigned long long initial_run_generation,
        unsigned long long initial_break_generation,
        unsigned long long initial_design_generation,
        std::string& state,
        std::string& response)
    {
        const DWORD timeout_ms = 10000;
        const DWORD poll_interval_ms = 25;
        const ULONGLONG start_tick = GetTickCount64();

        while ((GetTickCount64() - start_tick) <= timeout_ms)
        {
            if (m_is_stopping.load())
            {
                response = "error: package stopping while waiting for run mode";
                return false;
            }
            if (!m_debugger_events_ready.load())
            {
                response = "error: debugger mode events became unavailable while waiting for run mode";
                return false;
            }

            const unsigned long long run_generation = m_vs_run_generation.load();
            const unsigned long long break_generation = m_vs_break_generation.load();
            const unsigned long long design_generation = m_vs_design_generation.load();
            if (run_generation > initial_run_generation || design_generation > initial_design_generation)
            {
                if (design_generation > initial_design_generation)
                {
                    state = "debugging ended";
                }
                else if (break_generation > initial_break_generation)
                {
                    state = "break hit";
                }
                else
                {
                    state = "running";
                }

                bh_agent_logf(
                    "package.pipe",
                    "continue reached %s runGeneration=%llu->%llu elapsedMs=%llu",
                    state.c_str(),
                    initial_run_generation,
                    run_generation,
                    (unsigned long long)(GetTickCount64() - start_tick));
                return true;
            }

            if (m_pipe_stop_event)
            {
                if (WaitForSingleObject(m_pipe_stop_event, poll_interval_ms) == WAIT_OBJECT_0)
                {
                    response = "error: package stopping while waiting for run mode";
                    return false;
                }
            }
            else
            {
                Sleep(poll_interval_ms);
            }
        }

        bh_agent_logf(
            "package.pipe",
            "continue run-mode timeout runGeneration=%llu->%llu breakGeneration=%llu->%llu designGeneration=%llu->%llu",
            initial_run_generation,
            m_vs_run_generation.load(),
            initial_break_generation,
            m_vs_break_generation.load(),
            initial_design_generation,
            m_vs_design_generation.load());
        response = "error: continue timeout waiting for VS run mode";
        return false;
    }

    bool WaitForVsBreakReadiness(
        ::Debugger* debugger,
        unsigned long long initial_run_generation,
        unsigned long long initial_break_generation,
        unsigned long long initial_design_generation,
        bool require_new_break,
        std::string& response)
    {
        if (!debugger)
        {
            response = "error: debugger unavailable while waiting for stable break state";
            return false;
        }
        if (!m_debugger_events_ready.load())
        {
            response = "error: debugger mode events unavailable for break synchronization";
            return false;
        }

        const DWORD timeout_ms = 10000;
        const DWORD stable_break_ms = 100;
        const DWORD poll_interval_ms = 25;
        const ULONGLONG start_tick = GetTickCount64();
        ULONGLONG stable_break_tick = 0;
        unsigned long long stable_run_generation = initial_run_generation;
        unsigned long long stable_break_generation = initial_break_generation;
        std::string stable_engine_status;
        std::string last_engine_status;

        while ((GetTickCount64() - start_tick) <= timeout_ms)
        {
            if (m_is_stopping.load())
            {
                response = "error: package stopping while waiting for stable break state";
                return false;
            }

            const unsigned long long run_generation = m_vs_run_generation.load();
            const unsigned long long break_generation = m_vs_break_generation.load();
            const unsigned long long design_generation = m_vs_design_generation.load();
            if (design_generation > initial_design_generation)
            {
                response = "error: debugging ended while waiting for stable break state";
                return false;
            }

            dbgDebugMode mode = dbgDesignMode;
            HRESULT hr = debugger->get_CurrentMode(&mode);
            if (FAILED(hr))
            {
                response = "error: unable to determine debugger mode while waiting for stable break state";
                return false;
            }
            if (mode == dbgDesignMode)
            {
                response = "error: debugging ended while waiting for stable break state";
                return false;
            }

            const bool break_generation_ready =
                !require_new_break || break_generation > initial_break_generation;
            if (mode == dbgBreakMode && break_generation_ready)
            {
                std::string engine_status;
                ForwardMessageToRemotePipe("status", engine_status);
                last_engine_status = engine_status;

                dbgDebugMode verified_mode = dbgDesignMode;
                hr = debugger->get_CurrentMode(&verified_mode);
                if (FAILED(hr))
                {
                    response = "error: unable to recheck debugger mode while waiting for stable break state";
                    return false;
                }
                if (m_is_stopping.load() ||
                    (m_pipe_stop_event && WaitForSingleObject(m_pipe_stop_event, 0) == WAIT_OBJECT_0))
                {
                    response = "error: package stopping while waiting for stable break state";
                    return false;
                }
                if (!m_debugger_events_ready.load())
                {
                    response = "error: debugger mode events became unavailable while waiting for stable break state";
                    return false;
                }
                if ((GetTickCount64() - start_tick) > timeout_ms)
                {
                    response = "error: break stabilization timeout waiting for stable DTE/DKM break session";
                    return false;
                }

                const unsigned long long verified_run_generation = m_vs_run_generation.load();
                const unsigned long long verified_break_generation = m_vs_break_generation.load();
                const unsigned long long verified_design_generation = m_vs_design_generation.load();
                if (verified_mode == dbgDesignMode || verified_design_generation > initial_design_generation)
                {
                    response = "error: debugging ended while waiting for stable break state";
                    return false;
                }

                const bool generations_stable =
                    verified_run_generation == run_generation &&
                    verified_break_generation == break_generation &&
                    verified_design_generation == design_generation;
                const bool engine_break_ready =
                    engine_status.rfind("ok: paused break ", 0) == 0;
                if (verified_mode == dbgBreakMode && generations_stable && engine_break_ready)
                {
                    const ULONGLONG now_tick = GetTickCount64();
                    if (stable_break_tick == 0 ||
                        stable_run_generation != verified_run_generation ||
                        stable_break_generation != verified_break_generation ||
                        stable_engine_status != engine_status)
                    {
                        stable_break_tick = now_tick;
                        stable_run_generation = verified_run_generation;
                        stable_break_generation = verified_break_generation;
                        stable_engine_status = engine_status;
                    }
                    else if ((now_tick - stable_break_tick) >= stable_break_ms)
                    {
                        if (m_is_stopping.load() ||
                            (m_pipe_stop_event && WaitForSingleObject(m_pipe_stop_event, 0) == WAIT_OBJECT_0))
                        {
                            response = "error: package stopping while waiting for stable break state";
                            return false;
                        }
                        if (!m_debugger_events_ready.load())
                        {
                            response = "error: debugger mode events became unavailable while waiting for stable break state";
                            return false;
                        }
                        if ((now_tick - start_tick) > timeout_ms)
                        {
                            response = "error: break stabilization timeout waiting for stable DTE/DKM break session";
                            return false;
                        }
                        if (m_vs_run_generation.load() != verified_run_generation ||
                            m_vs_break_generation.load() != verified_break_generation ||
                            m_vs_design_generation.load() != verified_design_generation)
                        {
                            stable_break_tick = 0;
                            stable_engine_status.clear();
                            continue;
                        }

                        bh_agent_logf(
                            "package.pipe",
                            "break stabilized runGeneration=%llu breakGeneration=%llu elapsedMs=%llu status=%s",
                            verified_run_generation,
                            verified_break_generation,
                            (unsigned long long)(now_tick - start_tick),
                            engine_status.c_str());
                        return true;
                    }
                }
                else
                {
                    stable_break_tick = 0;
                    stable_engine_status.clear();
                }
            }
            else
            {
                stable_break_tick = 0;
                stable_engine_status.clear();
            }

            if (m_pipe_stop_event)
            {
                if (WaitForSingleObject(m_pipe_stop_event, poll_interval_ms) == WAIT_OBJECT_0)
                {
                    response = "error: package stopping while waiting for stable break state";
                    return false;
                }
            }
            else
            {
                Sleep(poll_interval_ms);
            }
        }

        bh_agent_logf(
            "package.pipe",
            "break stabilization timeout runGeneration=%llu->%llu breakGeneration=%llu->%llu designGeneration=%llu->%llu lastStatus=%s",
            initial_run_generation,
            m_vs_run_generation.load(),
            initial_break_generation,
            m_vs_break_generation.load(),
            initial_design_generation,
            m_vs_design_generation.load(),
            last_engine_status.empty() ? "<none>" : last_engine_status.c_str());
        response = "error: break stabilization timeout waiting for stable DTE/DKM break session";
        return false;
    }

    static DWORD WINAPI AgentPipeThreadProc(LPVOID context)
    {
        if (!context)
        {
            return 0;
        }

        bh_agent_debugger_package* self = static_cast<bh_agent_debugger_package*>(context);
        self->RunAgentPipeServer();
        self->InternalRelease();
        return 0;
    }

    static DWORD WINAPI BrokerRegistrationThreadProc(LPVOID context)
    {
        if (!context)
        {
            return 0;
        }

        bh_agent_debugger_package* self = static_cast<bh_agent_debugger_package*>(context);
        self->RunBrokerRegistrationLoop();
        self->InternalRelease();
        return 0;
    }

    void StartAgentPipeServer()
    {
        m_is_stopping.store(false);

        if (m_pipe_thread)
        {
            return;
        }

        m_pipe_name = BuildAgentPipeName();

        m_pipe_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_pipe_stop_event)
        {
            bh_agent_logf("package.pipe", "CreateEventW failed");
            return;
        }

        DWORD thread_id = 0;
        InternalAddRef();
        m_pipe_thread = CreateThread(nullptr, 0, &AgentPipeThreadProc, this, 0, &thread_id);
        if (!m_pipe_thread)
        {
            InternalRelease();
            bh_agent_logf("package.pipe", "CreateThread failed");
            CloseHandle(m_pipe_stop_event);
            m_pipe_stop_event = nullptr;
            return;
        }

        m_broker_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_broker_stop_event)
        {
            bh_agent_logf("package.pipe", "CreateEventW failed for broker registration");
            SetEvent(m_pipe_stop_event);
            WaitForSingleObject(m_pipe_thread, 500);
            CloseHandle(m_pipe_thread);
            m_pipe_thread = nullptr;
            CloseHandle(m_pipe_stop_event);
            m_pipe_stop_event = nullptr;
            return;
        }

        InternalAddRef();
        m_broker_thread = CreateThread(nullptr, 0, &BrokerRegistrationThreadProc, this, 0, &thread_id);
        if (!m_broker_thread)
        {
            InternalRelease();
            bh_agent_logf("package.pipe", "CreateThread failed for broker registration");
            CloseHandle(m_broker_stop_event);
            m_broker_stop_event = nullptr;
            SetEvent(m_pipe_stop_event);
            WaitForSingleObject(m_pipe_thread, 500);
            CloseHandle(m_pipe_thread);
            m_pipe_thread = nullptr;
            CloseHandle(m_pipe_stop_event);
            m_pipe_stop_event = nullptr;
            return;
        }

        bh_agent_logf("package.pipe", "agent pipe server started name=%ws", m_pipe_name.c_str());
        RegisterPipeTargetWithBroker();
    }

    bool StopAgentPipeServer()
    {
        bool pipe_stopped = true;
        bool broker_stopped = true;

        if (m_pipe_stop_event)
        {
            SetEvent(m_pipe_stop_event);
        }

        if (m_broker_stop_event)
        {
            SetEvent(m_broker_stop_event);
        }

        WakeAgentPipeServer();

        if (m_pipe_thread)
        {
            DWORD wait_result = WaitForSingleObject(m_pipe_thread, 5000);

            if (wait_result == WAIT_OBJECT_0)
            {
                CloseHandle(m_pipe_thread);
                m_pipe_thread = nullptr;
            }
            else
            {
                pipe_stopped = false;
                bh_agent_logf("package.pipe", "pipe thread did not stop cleanly wait=%lu", (unsigned long)wait_result);
            }
        }

        if (m_pipe_stop_event && pipe_stopped)
        {
            CloseHandle(m_pipe_stop_event);
            m_pipe_stop_event = nullptr;
        }

        if (m_broker_thread)
        {
            DWORD wait_result = WaitForSingleObject(m_broker_thread, 5000);
            if (wait_result == WAIT_OBJECT_0)
            {
                CloseHandle(m_broker_thread);
                m_broker_thread = nullptr;
            }
            else
            {
                broker_stopped = false;
                bh_agent_logf("package.pipe", "broker thread did not stop cleanly wait=%lu", (unsigned long)wait_result);
            }
        }

        if (m_broker_stop_event && broker_stopped)
        {
            CloseHandle(m_broker_stop_event);
            m_broker_stop_event = nullptr;
        }

        return pipe_stopped && broker_stopped;
    }

    void WakeAgentPipeServer()
    {
        if (m_pipe_name.empty())
        {
            return;
        }

        std::wstring pipe_path = L"\\\\.\\pipe\\" + m_pipe_name;
        if (!WaitNamedPipeW(pipe_path.c_str(), 50))
        {
            return;
        }

        HANDLE wake_pipe = CreateFileW(
            pipe_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (wake_pipe == INVALID_HANDLE_VALUE)
        {
            return;
        }

        const char wake_message[] = "status\n";
        DWORD written = 0;
        WriteFile(wake_pipe, wake_message, static_cast<DWORD>(sizeof(wake_message) - 1), &written, nullptr);
        FlushFileBuffers(wake_pipe);
        CloseHandle(wake_pipe);
    }

    void RunBrokerRegistrationLoop()
    {
        while (m_broker_stop_event && WaitForSingleObject(m_broker_stop_event, 0) == WAIT_TIMEOUT)
        {
            RegisterPipeTargetWithBroker();

            DWORD wait_ms = m_broker_registered ? 5000 : 500;
            WaitForSingleObject(m_broker_stop_event, wait_ms);
        }

        UnregisterTargetFromBroker();
    }

    void RunAgentPipeServer()
    {
        bool com_thread_initialized = false;
        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com_hr))
        {
            com_thread_initialized = true;
            bh_agent_logf("package.pipe", "COM initialized for pipe thread");
        }
        else if (com_hr != RPC_E_CHANGED_MODE)
        {
            bh_agent_logf("package.pipe", "COM initialize failed for pipe thread hr=0x%08X", (unsigned)com_hr);
        }

        // DKM is initialized per command, after Concord becomes available.
        std::wstring pipe_path = L"\\\\.\\pipe\\" + m_pipe_name;
        // Current-user-only pipe; remote clients rejected. Never fall back to default security.
        bh_agent_pipe_security::server_attributes pipe_security;
        if (!pipe_security.get())
        {
            bh_agent_logf("package.pipe", "pipe server not started: could not build current-user pipe security");
        }
        while (pipe_security.get() && m_pipe_stop_event && WaitForSingleObject(m_pipe_stop_event, 0) == WAIT_TIMEOUT)
        {
            HANDLE pipe = CreateNamedPipeW(
                pipe_path.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | bh_agent_pipe_security::server_pipe_mode_flags,
                1,
                1024,
                1024,
                200,
                pipe_security.get());

            if (pipe == INVALID_HANDLE_VALUE)
            {
                Sleep(100);
                continue;
            }

            BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (WaitForSingleObject(m_pipe_stop_event, 0) != WAIT_TIMEOUT)
            {
                // Shutdown woke the server (WakeAgentPipeServer): don't run that connection's message.
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);
                break;
            }

            if (connected)
            {
                char buffer[1024] = {};
                DWORD read_bytes = 0;
                if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read_bytes, nullptr) && read_bytes > 0)
                {
                    buffer[read_bytes] = 0;

                    size_t len = strlen(buffer);
                    while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n'))
                    {
                        buffer[len - 1] = 0;
                        --len;
                    }

                    if (len == 0)
                    {
                        DisconnectNamedPipe(pipe);
                        CloseHandle(pipe);
                        continue;
                    }

                    bh_agent_logf("package.pipe", "message from script: %s", buffer);

                    std::string response;
                    if (strstr(buffer, ";") != nullptr || strstr(buffer, "&&") != nullptr)
                    {
                        std::vector<std::string> commands;
                        SplitChainedCommands(buffer, commands);
                        if (commands.empty())
                        {
                            response = "error: empty command chain";
                        }
                        else
                        {
                            std::string combined = "ok: chain count=" + std::to_string(commands.size());
                            bool chain_error = false;
                            for (size_t i = 0; i < commands.size(); ++i)
                            {
                                std::string step_response;
                                ExecuteSingleMessage(commands[i].c_str(), step_response);
                                if (step_response.empty())
                                {
                                    step_response = "ok";
                                }

                                const size_t skipped_count = commands.size() - i - 1;
                                const std::string item = " | #" + std::to_string(i) + " " + step_response;
                                if (!bh_agent_help::append_chain_item(combined, item))
                                {
                                    response =
                                        "error: chain response exceeds 8192-byte limit at #" + std::to_string(i) +
                                        " skipped=" + std::to_string(skipped_count);
                                    chain_error = true;
                                    break;
                                }
                                if (step_response.rfind("error:", 0) == 0)
                                {
                                    response =
                                        "error: chain failed at #" + std::to_string(i) +
                                        " skipped=" + std::to_string(skipped_count) + combined.substr(3);
                                    if (response.size() > 8192)
                                    {
                                        response =
                                            "error: chain failed at #" + std::to_string(i) +
                                            " skipped=" + std::to_string(skipped_count) +
                                            " | failure response exceeded 8192-byte limit";
                                    }
                                    chain_error = true;
                                    break;
                                }
                            }

                            if (!chain_error)
                            {
                                response = combined;
                            }
                        }
                    }
                    else
                    {
                        ExecuteSingleMessage(buffer, response);
                    }

                    if (response.empty())
                    {
                        response = "ok";
                    }

                    bh_agent_logf("package.pipe", "response to script: %s", response.c_str());

                    std::string response_line = response;
                    response_line.push_back('\n');

                    DWORD written = 0;
                    BOOL write_ok = WriteFile(pipe, response_line.c_str(), (DWORD)response_line.size(), &written, nullptr);
                    DWORD write_err = write_ok ? 0 : GetLastError();
                    if (!write_ok || written != response_line.size())
                    {
                        bh_agent_logf(
                            "package.pipe",
                            "response write failed ok=%d err=%lu written=%lu expected=%lu",
                            write_ok ? 1 : 0,
                            (unsigned long)write_err,
                            (unsigned long)written,
                            (unsigned long)response_line.size());
                    }
                    else
                    {
                        bh_agent_logf("package.pipe", "response write ok bytes=%lu", (unsigned long)written);
                    }

                    BOOL flush_ok = FlushFileBuffers(pipe);
                    if (!flush_ok)
                    {
                        bh_agent_logf("package.pipe", "FlushFileBuffers failed err=%lu", (unsigned long)GetLastError());
                    }
                }
            }

            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }

        if (com_thread_initialized)
        {
            CoUninitialize();
        }

        bh_agent_logf("package.pipe", "agent pipe server stopped");
    }

    void ExecuteSingleMessage(const char* message, std::string& response)
    {
        response.clear();
        if (!message || !message[0])
        {
            response = "error: empty message";
            return;
        }

        const std::string normalized_message = bh_dump::trim(message);
        message = normalized_message.c_str();
        if (bh_agent_help::try_handle(message, response))
        {
            return;
        }
        if (TryTransientStartWithArguments(message, response))
        {
            return;
        }

        bh_agent_dkm_thread_scope dkm_scope;
        (void)dkm_scope.ensure(); // A design-mode failure can be retried after dump launch.
        // This gate runs before every DTE/live-engine command, including chain steps.
        if (bh_agent_dump_session::try_handle(GetServiceProviderSnapshot(), message, response, dkm_scope))
        {
            return;
        }

        if (!TryPollBreakCommand(message, response) &&
            !TryGetVsDebugStatus(message, response) &&
            !TrySelectThread(message, response) &&
            !TryExecuteVsStepCommand(message, response))
        {
            if (!TrySetFileLineBreakpoint(message, response) &&
                !TryListBreakpoints(message, response) &&
                !TryRemoveBreakpoint(message, response) &&
                !TryClearBreakpoints(message, response) &&
                !TryEvaluateExpression(message, response) &&
                !TryListLocalVariables(message, response))
            {
                ForwardMessageToRemotePipe(message, response);
            }
        }
    }

    static bool TryGetSingleStartupProject(const VARIANT& startup_projects, CComBSTR& project_name)
    {
        project_name.Empty();
        if (startup_projects.vt == VT_BSTR && startup_projects.bstrVal)
        {
            project_name = startup_projects.bstrVal;
            return project_name.Length() != 0;
        }
        if (startup_projects.vt == (VT_BSTR | VT_BYREF) && startup_projects.pbstrVal && *startup_projects.pbstrVal)
        {
            project_name = *startup_projects.pbstrVal;
            return project_name.Length() != 0;
        }

        const VARTYPE element_type = static_cast<VARTYPE>(startup_projects.vt & ~(VT_ARRAY | VT_BYREF));
        if ((startup_projects.vt & VT_ARRAY) == 0 || (element_type != VT_BSTR && element_type != VT_VARIANT))
        {
            return false;
        }

        SAFEARRAY* values = (startup_projects.vt & VT_BYREF)
            ? (startup_projects.pparray ? *startup_projects.pparray : nullptr)
            : startup_projects.parray;
        if (!values || SafeArrayGetDim(values) != 1)
        {
            return false;
        }

        LONG lower = 0;
        LONG upper = -1;
        if (FAILED(SafeArrayGetLBound(values, 1, &lower)) ||
            FAILED(SafeArrayGetUBound(values, 1, &upper)) || upper != lower)
        {
            return false;
        }

        if (element_type == VT_BSTR)
        {
            BSTR item = nullptr;
            const HRESULT hr = SafeArrayGetElement(values, &lower, &item);
            if (FAILED(hr) || !item)
            {
                return false;
            }
            project_name = item;
            SysFreeString(item);
            return project_name.Length() != 0;
        }

        CComVariant item;
        if (FAILED(SafeArrayGetElement(values, &lower, &item)) || item.vt != VT_BSTR || !item.bstrVal)
        {
            return false;
        }
        project_name = item.bstrVal;
        return project_name.Length() != 0;
    }

    static void FreeQueriedDebugTarget(VsDebugTargetInfo4& target)
    {
        SysFreeString(target.bstrRemoteMachine);
        SysFreeString(target.bstrExe);
        SysFreeString(target.bstrArg);
        SysFreeString(target.bstrCurDir);
        SysFreeString(target.bstrEnv);
        SysFreeString(target.bstrPortName);
        SysFreeString(target.bstrOptions);
        SysFreeString(target.AppPackageLaunchInfo.PackageMoniker);
        SysFreeString(target.AppPackageLaunchInfo.AppUserModelID);
        if (target.pStartupInfo)
        {
            CoTaskMemFree(const_cast<LPWSTR>(target.pStartupInfo->lpReserved));
            CoTaskMemFree(const_cast<LPWSTR>(target.pStartupInfo->lpDesktop));
            CoTaskMemFree(const_cast<LPWSTR>(target.pStartupInfo->lpTitle));
            CoTaskMemFree(target.pStartupInfo->lpReserved2);
            CoTaskMemFree(target.pStartupInfo);
        }
        CoTaskMemFree(target.pDebugEngines);
        if (target.pUnknown) target.pUnknown->Release();
        if (target.project) target.project->Release();
        ZeroMemory(&target, sizeof(target));
    }

    class queried_debug_targets
    {
    public:
        explicit queried_debug_targets(ULONG count) : values(count), owned_count(0)
        {
            ZeroMemory(values.data(), values.size() * sizeof(VsDebugTargetInfo4));
        }

        ~queried_debug_targets()
        {
            for (std::size_t i = 0; i < owned_count; ++i) FreeQueriedDebugTarget(values[i]);
        }

        void accept_successful_results(ULONG count)
        {
            owned_count = count;
            values.resize(count);
        }

        // QueryDebugTargets returns COM out values: BSTRs, task-allocated pointer
        // fields, and AddRef'd interface pointers. Only a successful call's
        // pcActual entries are accepted for cleanup; failed partial output is ignored.
        std::vector<VsDebugTargetInfo4> values;
        std::size_t owned_count;
    };

    static bool CheckTransientLaunchState(::Debugger* debugger, std::string& response)
    {
        dbgDebugMode mode = dbgDesignMode;
        HRESULT hr = debugger ? debugger->get_CurrentMode(&mode) : E_POINTER;
        if (FAILED(hr) || mode != dbgDesignMode)
        {
            response = "error: transient launch requires Visual Studio design mode";
            return false;
        }

        bh_agent_dkm_thread_scope dkm_scope;
        (void)dkm_scope.ensure();
        unsigned process_count = 0;
        unsigned dump_count = 0;
        hr = bh_agent_dump_session::get_session_counts(debugger, process_count, dump_count);
        if (FAILED(hr))
        {
            response = "error: could not verify transient launch session state";
            return false;
        }
        if (process_count != 0 || dump_count != 0)
        {
            response = "error: transient launch refused while another debug or dump session is active";
            return false;
        }
        return true;
    }

    bool TryTransientStartWithArguments(const char* message, std::string& response)
    {
        const std::string prefix = bh_startup_arguments::command_prefix;
        const std::string request = message ? message : "";
        if (request.compare(0, prefix.size(), prefix) != 0)
        {
            return false;
        }

        std::string arguments_utf8;
        if (!bh_startup_arguments::try_decode(request.substr(prefix.size()), arguments_utf8))
        {
            response = "error: malformed transient launch arguments";
            return true;
        }

        std::wstring arguments;
        if (!TryUtf8ToWide(arguments_utf8.c_str(), arguments))
        {
            response = "error: transient launch arguments are not valid UTF-8";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider for transient launch";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(
            SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE for transient launch";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger || !CheckTransientLaunchState(debugger, response))
        {
            if (response.empty()) response = "error: could not get debugger for transient launch";
            return true;
        }

        CComPtr<_Solution> solution;
        hr = dte->get_Solution(reinterpret_cast<Solution**>(&solution));
        if (FAILED(hr) || !solution)
        {
            response = "error: could not get solution for transient launch";
            return true;
        }

        CComPtr<SolutionBuild> solution_build;
        hr = solution->get_SolutionBuild(&solution_build);
        if (FAILED(hr) || !solution_build)
        {
            response = "error: could not get solution build for transient launch";
            return true;
        }

        CComVariant startup_projects;
        hr = solution_build->get_StartupProjects(&startup_projects);
        CComBSTR startup_project;
        if (FAILED(hr) || !TryGetSingleStartupProject(startup_projects, startup_project))
        {
            response = "error: transient launch requires exactly one startup project";
            return true;
        }

        CComPtr<SolutionConfiguration> solution_configuration;
        hr = solution_build->get_ActiveConfiguration(&solution_configuration);
        if (FAILED(hr) || !solution_configuration)
        {
            response = "error: could not get active solution configuration";
            return true;
        }

        CComBSTR solution_configuration_name;
        hr = solution_configuration->get_Name(&solution_configuration_name);
        if (FAILED(hr) || solution_configuration_name.Length() == 0)
        {
            response = "error: active solution configuration has no name";
            return true;
        }

        CComPtr<SolutionContexts> solution_contexts;
        hr = solution_configuration->get_SolutionContexts(&solution_contexts);
        if (FAILED(hr) || !solution_contexts)
        {
            response = "error: could not get active solution contexts";
            return true;
        }

        long context_count = 0;
        hr = solution_contexts->get_Count(&context_count);
        if (FAILED(hr) || context_count <= 0)
        {
            response = "error: active solution configuration has no project contexts";
            return true;
        }

        CComBSTR project_configuration_name;
        CComBSTR project_platform_name;
        for (long index = 1; index <= context_count; ++index)
        {
            CComPtr<SolutionContext> context;
            CComVariant context_index(index);
            if (FAILED(solution_contexts->Item(context_index, &context)) || !context) continue;

            CComBSTR context_project;
            if (FAILED(context->get_ProjectName(&context_project)) || !context_project ||
                _wcsicmp(context_project, startup_project) != 0) continue;

            if (FAILED(context->get_ConfigurationName(&project_configuration_name)) ||
                FAILED(context->get_PlatformName(&project_platform_name)))
            {
                response = "error: could not resolve startup project configuration";
                return true;
            }
            break;
        }
        if (project_configuration_name.Length() == 0 || project_platform_name.Length() == 0)
        {
            response = "error: startup project is not in the active solution configuration";
            return true;
        }

        hr = solution_build->BuildProject(
            solution_configuration_name, startup_project, VARIANT_TRUE);
        if (FAILED(hr))
        {
            response = "error: startup project build could not be started";
            return true;
        }

        vsBuildState build_state = vsBuildStateNotStarted;
        long failed_project_count = 0;
        hr = solution_build->get_BuildState(&build_state);
        if (FAILED(hr) || build_state != vsBuildStateDone ||
            FAILED(solution_build->get_LastBuildInfo(&failed_project_count)) || failed_project_count != 0)
        {
            response = "error: startup project build failed or did not complete";
            return true;
        }
        if (!CheckTransientLaunchState(debugger, response))
        {
            return true;
        }

        CComPtr<IVsSolution> shell_solution;
        hr = provider->QueryService(
            SID_SVsSolution, __uuidof(IVsSolution), reinterpret_cast<void**>(&shell_solution));
        if (FAILED(hr) || !shell_solution)
        {
            response = "error: could not get shell solution for transient launch";
            return true;
        }

        CComPtr<IVsHierarchy> hierarchy;
        hr = shell_solution->GetProjectOfUniqueName(startup_project, &hierarchy);
        if (FAILED(hr) || !hierarchy)
        {
            response = "error: could not resolve startup project hierarchy";
            return true;
        }

        CComVariant cfg_provider_property;
        hr = hierarchy->GetProperty(
            VSITEMID_ROOT, VSHPROPID_ConfigurationProvider, &cfg_provider_property);
        IUnknown* cfg_provider_unknown = nullptr;
        if (SUCCEEDED(hr) && cfg_provider_property.vt == VT_UNKNOWN)
        {
            cfg_provider_unknown = cfg_provider_property.punkVal;
        }
        else if (SUCCEEDED(hr) && cfg_provider_property.vt == VT_DISPATCH)
        {
            cfg_provider_unknown = cfg_provider_property.pdispVal;
        }

        CComPtr<IVsCfgProvider2> cfg_provider2;
        if (!cfg_provider_unknown ||
            FAILED(cfg_provider_unknown->QueryInterface(
                __uuidof(IVsCfgProvider2), reinterpret_cast<void**>(&cfg_provider2))) ||
            !cfg_provider2)
        {
            response = "error: startup project configuration provider is unsupported";
            return true;
        }

        CComPtr<IVsCfg> cfg;
        hr = cfg_provider2->GetCfgOfName(project_configuration_name, project_platform_name, &cfg);
        CComPtr<IVsProjectCfg2> project_cfg;
        if (FAILED(hr) || !cfg ||
            FAILED(cfg->QueryInterface(__uuidof(IVsProjectCfg2), reinterpret_cast<void**>(&project_cfg))) ||
            !project_cfg)
        {
            response = "error: could not resolve startup project configuration";
            return true;
        }

        CComPtr<IVsDebuggableProjectCfg> debug_cfg_base;
        hr = project_cfg->get_CfgType(
            __uuidof(IVsDebuggableProjectCfg), reinterpret_cast<void**>(&debug_cfg_base));
        if (FAILED(hr) || !debug_cfg_base)
        {
            response = "error: startup project does not expose a debuggable configuration";
            return true;
        }

        CComPtr<IVsDebuggableProjectCfg2> debug_cfg;
        hr = debug_cfg_base->QueryInterface(
            __uuidof(IVsDebuggableProjectCfg2), reinterpret_cast<void**>(&debug_cfg));
        if (FAILED(hr) && hr != E_NOINTERFACE)
        {
            response = "error: startup project debug launch hook query failed";
            return true;
        }

        CComPtr<IVsQueryDebuggableProjectCfg2> query_debug_cfg;
        hr = project_cfg->get_CfgType(
            __uuidof(IVsQueryDebuggableProjectCfg2), reinterpret_cast<void**>(&query_debug_cfg));
        if (FAILED(hr) || !query_debug_cfg)
        {
            response = "error: startup project does not expose transient debug target queries";
            return true;
        }

        const VSDBGLAUNCHFLAGS launch_flags = static_cast<VSDBGLAUNCHFLAGS>(0);
        BOOL can_launch = FALSE;
        hr = debug_cfg_base->QueryDebugLaunch(launch_flags, &can_launch);
        if (FAILED(hr) || !can_launch)
        {
            response = "error: startup project is not ready to debug";
            return true;
        }
        if (debug_cfg)
        {
            hr = debug_cfg->OnBeforeDebugLaunch(launch_flags);
            if (FAILED(hr))
            {
                response = "error: startup project rejected transient debug launch";
                return true;
            }
        }

        constexpr ULONG maximum_target_count = 32;
        queried_debug_targets targets(maximum_target_count);
        ULONG actual_target_count = 0;
        hr = query_debug_cfg->QueryDebugTargets(
            launch_flags, maximum_target_count, targets.values.data(), &actual_target_count);
        if (FAILED(hr) || actual_target_count == 0 || actual_target_count > maximum_target_count)
        {
            response = "error: startup project debug target query failed";
            return true;
        }
        targets.accept_successful_results(actual_target_count);

        for (auto& target : targets.values)
        {
            BSTR replacement = SysAllocStringLen(arguments.data(), static_cast<UINT>(arguments.size()));
            if (!replacement)
            {
                response = "error: could not allocate transient launch arguments";
                return true;
            }
            SysFreeString(target.bstrArg);
            target.bstrArg = replacement;
        }

        CComPtr<IVsDebugger4> shell_debugger;
        hr = provider->QueryService(
            SID_SVsShellDebugger, __uuidof(IVsDebugger4), reinterpret_cast<void**>(&shell_debugger));
        if (FAILED(hr) || !shell_debugger)
        {
            response = "error: Visual Studio transient debugger service is unavailable";
            return true;
        }

        if (!CheckTransientLaunchState(debugger, response))
        {
            return true;
        }

        std::vector<VsDebugTargetProcessInfo> launch_results(targets.values.size());
        ZeroMemory(launch_results.data(), launch_results.size() * sizeof(VsDebugTargetProcessInfo));
        hr = shell_debugger->LaunchDebugTargets4(
            static_cast<ULONG>(targets.values.size()), targets.values.data(), launch_results.data());
        if (FAILED(hr))
        {
            response = "error: Visual Studio transient debug launch failed";
            return true;
        }

        response = "ok: transient debug launch requested";
        return true;
    }

    bool TryParseUnsigned(const char* text, unsigned int& value)
    {
        value = 0;
        if (!text || !text[0])
        {
            return false;
        }

        char* end_ptr = nullptr;
        const unsigned long long parsed = _strtoui64(text, &end_ptr, 10);
        if (end_ptr == text || parsed == 0 || parsed > 0xFFFFFFFFULL)
        {
            return false;
        }

        while (*end_ptr)
        {
            if (*end_ptr != ' ' && *end_ptr != '\t')
            {
                return false;
            }

            ++end_ptr;
        }

        value = static_cast<unsigned int>(parsed);
        return true;
    }

    bool TryPollBreakCommand(const char* message, std::string& response)
    {
        if (!message)
        {
            return false;
        }

        unsigned int timeout_ms = 15000;
        unsigned int interval_ms = 200;

        if (_stricmp(message, "poll break") == 0)
        {
        }
        else if (_strnicmp(message, "poll break ", 11) == 0)
        {
            const char* payload = message + 11;
            while (*payload == ' ' || *payload == '\t')
            {
                ++payload;
            }

            if (!*payload)
            {
                return false;
            }

            char first_token[32] = {};
            size_t first_len = 0;
            while (payload[first_len] && payload[first_len] != ' ' && payload[first_len] != '\t' && first_len < sizeof(first_token) - 1)
            {
                first_token[first_len] = payload[first_len];
                ++first_len;
            }
            first_token[first_len] = 0;

            if (!TryParseUnsigned(first_token, timeout_ms))
            {
                response = "error: poll break parse failed";
                return true;
            }

            const char* rest = payload + first_len;
            while (*rest == ' ' || *rest == '\t')
            {
                ++rest;
            }

            if (*rest)
            {
                if (!TryParseUnsigned(rest, interval_ms))
                {
                    response = "error: poll break parse failed";
                    return true;
                }
            }
        }
        else
        {
            return false;
        }

        if (interval_ms < 50)
        {
            interval_ms = 50;
        }

        const ULONGLONG start_tick = GetTickCount64();
        unsigned int tries = 0;
        std::string last_status;

        while ((GetTickCount64() - start_tick) <= static_cast<ULONGLONG>(timeout_ms))
        {
            ++tries;
            std::string status_response;
            if (!TryGetVsDebugStatus("status", status_response))
            {
                response = "error: poll break status unavailable";
                return true;
            }

            last_status = status_response;
            if (status_response.rfind("error:", 0) == 0)
            {
                response = "error: poll break status failed | " + status_response;
                return true;
            }

            if (status_response.find("vs break mode") != std::string::npos)
            {
                response = "ok: poll break tries=" + std::to_string(tries) + " | " + status_response;
                return true;
            }

            Sleep(interval_ms);
        }

        response = "error: poll break timeout tries=" + std::to_string(tries);
        if (!last_status.empty())
        {
            response += " | " + last_status;
        }

        return true;
    }

    static void SplitChainedCommands(const char* message, std::vector<std::string>& commands)
    {
        bh_agent_command_chain::split(message, commands);
    }

    bool TryFindDteThread(
        ::Debugger* debugger,
        unsigned int requested_thread_id,
        CComPtr<::Thread>& selected_thread)
    {
        selected_thread.Release();
        if (!debugger || requested_thread_id == 0)
        {
            return false;
        }

        CComPtr<::Processes> processes;
        long process_count = 0;
        if (FAILED(debugger->get_DebuggedProcesses(&processes)) || !processes ||
            FAILED(processes->get_Count(&process_count)))
        {
            return false;
        }

        for (long process_index = 1; process_index <= process_count; ++process_index)
        {
            CComPtr<::Process> process;
            if (FAILED(processes->Item(CComVariant(process_index), &process)) || !process)
            {
                continue;
            }

            CComPtr<::Programs> programs;
            long program_count = 0;
            if (FAILED(process->get_Programs(&programs)) || !programs ||
                FAILED(programs->get_Count(&program_count)))
            {
                continue;
            }

            for (long program_index = 1; program_index <= program_count; ++program_index)
            {
                CComPtr<::Program> program;
                if (FAILED(programs->Item(CComVariant(program_index), &program)) ||
                    !program)
                {
                    continue;
                }

                CComPtr<::Threads> threads;
                long thread_count = 0;
                if (FAILED(program->get_Threads(&threads)) || !threads ||
                    FAILED(threads->get_Count(&thread_count)))
                {
                    continue;
                }

                for (long thread_index = 1; thread_index <= thread_count; ++thread_index)
                {
                    CComPtr<::Thread> thread;
                    long thread_id = 0;
                    if (FAILED(threads->Item(CComVariant(thread_index), &thread)) ||
                        !thread || FAILED(thread->get_ID(&thread_id)))
                    {
                        continue;
                    }

                    if (static_cast<unsigned int>(thread_id) == requested_thread_id)
                    {
                        selected_thread = thread;
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool TrySelectThread(const char* message, std::string& response)
    {
        if (!message || _strnicmp(message, "select thread", 13) != 0)
        {
            return false;
        }

        if (message[13] != ' ' && message[13] != '\t')
        {
            return false;
        }

        const char* thread_id_text = message + 14;
        while (*thread_id_text == ' ' || *thread_id_text == '\t')
        {
            ++thread_id_text;
        }

        if (*thread_id_text < '0' || *thread_id_text > '9')
        {
            response = "error: select thread usage: select thread <tid>";
            return true;
        }

        unsigned int requested_thread_id = 0;
        if (!TryParseUnsigned(thread_id_text, requested_thread_id))
        {
            response = "error: select thread usage: select thread <tid>";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider for thread selection";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(
            SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE for thread selection";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger for thread selection";
            return true;
        }

        dbgDebugMode mode = dbgDesignMode;
        hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr) || mode != dbgBreakMode)
        {
            response = "error: select thread requires debugger break mode";
            return true;
        }

        CComPtr<::Thread> selected_thread;
        if (!TryFindDteThread(debugger, requested_thread_id, selected_thread) ||
            !selected_thread)
        {
            response = "error: thread id not found in debugged processes";
            return true;
        }

        CComPtr<::Thread> previous_thread;
        hr = debugger->get_CurrentThread(&previous_thread);
        if (FAILED(hr))
        {
            response = "error: could not read current DTE thread";
            return true;
        }

        hr = debugger->put_CurrentThread(selected_thread);
        if (FAILED(hr))
        {
            char error[112] = {};
            _snprintf_s(error,
                        sizeof(error),
                        _TRUNCATE,
                        "error: could not select DTE thread hr=0x%08X",
                        (unsigned)hr);
            response = error;
            return true;
        }

        const std::string engine_command =
            "select thread " + std::to_string(requested_thread_id);
        ForwardMessageToRemotePipe(engine_command.c_str(), response);
        if (response.rfind("ok:", 0) != 0)
        {
            const HRESULT rollback_hr = debugger->put_CurrentThread(previous_thread);
            if (FAILED(rollback_hr))
            {
                char rollback[96] = {};
                _snprintf_s(rollback,
                            sizeof(rollback),
                            _TRUNCATE,
                            " | DTE rollback failed hr=0x%08X",
                            (unsigned)rollback_hr);
                response += rollback;
            }
            return true;
        }

        bh_agent_logf("package.pipe",
                      "selected DTE and engine thread tid=%u",
                      requested_thread_id);
        return true;
    }

    bool TryExecuteVsStepCommand(const char* message, std::string& response)
    {
        if (!message || !message[0])
        {
            return false;
        }

        const bool is_step_command =
            (_stricmp(message, "step into") == 0 ||
             _stricmp(message, "step over") == 0 ||
             _stricmp(message, "step out") == 0);
        const bool is_continue_command = (_stricmp(message, "continue") == 0);
        const bool is_break_command = (_stricmp(message, "async break") == 0 || _stricmp(message, "break") == 0);

        const wchar_t* command_name = nullptr;
        if (_stricmp(message, "step into") == 0)
        {
            command_name = L"Debug.StepInto";
        }
        else if (_stricmp(message, "step over") == 0)
        {
            command_name = L"Debug.StepOver";
        }
        else if (_stricmp(message, "step out") == 0)
        {
            command_name = L"Debug.StepOut";
        }
        else if (_stricmp(message, "continue") == 0)
        {
            command_name = L"Debug.Start";
        }
        else if (_stricmp(message, "start debugging") == 0 || _stricmp(message, "start") == 0)
        {
            command_name = L"Debug.Start";
        }
        else if (_stricmp(message, "async break") == 0 || _stricmp(message, "break") == 0)
        {
            command_name = L"Debug.BreakAll";
        }
        else if (_stricmp(message, "stop debugging") == 0 || _stricmp(message, "stop") == 0)
        {
            command_name = L"Debug.StopDebugging";
        }

        if (!command_name)
        {
            return false;
        }

        if (m_is_stopping.load())
        {
            bh_agent_logf("package.pipe", "step command '%s' skipped: package stopping", message);
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            bh_agent_logf("package.pipe", "step command '%s' skipped: no service provider", message);
            response = "error: no service provider for step command";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            bh_agent_logf("package.pipe", "step command '%s' failed: QueryService hr=0x%08X", message, (unsigned)hr);
            response = "error: could not get DTE for step command";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            bh_agent_logf("package.pipe", "step command '%s' failed: get_Debugger hr=0x%08X", message, (unsigned)hr);
            response = "error: could not get debugger for step command";
            return true;
        }

        if (is_continue_command)
        {
            dbgDebugMode mode = dbgDesignMode;
            hr = debugger->get_CurrentMode(&mode);
            if (FAILED(hr))
            {
                bh_agent_logf("package.pipe", "continue skipped: unable to read debugger mode hr=0x%08X", (unsigned)hr);
                response = "error: unable to determine debugger mode";
                return true;
            }

            if (mode == dbgRunMode)
            {
                // A previous continue may still be in flight: DTE already says run, but
                // the run event has not arrived. Load generations before the event mode
                // (OnModeChange publishes in the reverse order).
                const unsigned long long pending_run_generation = m_vs_run_generation.load();
                const unsigned long long pending_break_generation = m_vs_break_generation.load();
                const unsigned long long pending_design_generation = m_vs_design_generation.load();
                if (m_debugger_events_ready.load() && m_vs_event_mode.load() != DBGMODE_Run)
                {
                    std::string state;
                    if (!WaitForVsRunReadiness(
                            pending_run_generation,
                            pending_break_generation,
                            pending_design_generation,
                            state,
                            response))
                    {
                        bh_agent_logf("package.pipe", "pending continue did not reach run mode: %s", response.c_str());
                        return true;
                    }
                }

                bh_agent_logf("package.pipe", "continue ignored: debugger already running");
                response = "ok: already running";
                return true;
            }

            if (mode != dbgBreakMode)
            {
                bh_agent_logf("package.pipe", "continue ignored: debugger mode=%u", (unsigned)mode);
                response = "ok: not in break mode";
                return true;
            }

            command_name = L"Debug.Start";
        }

        unsigned long long initial_run_generation = 0;
        unsigned long long initial_break_generation = 0;
        unsigned long long initial_design_generation = 0;
        if (is_break_command)
        {
            if (!m_debugger_events_ready.load())
            {
                response = "error: debugger mode events unavailable for break synchronization";
                return true;
            }

            dbgDebugMode mode = dbgDesignMode;
            hr = debugger->get_CurrentMode(&mode);
            if (FAILED(hr))
            {
                response = "error: unable to determine debugger mode before break";
                return true;
            }

            initial_run_generation = m_vs_run_generation.load();
            initial_break_generation = m_vs_break_generation.load();
            initial_design_generation = m_vs_design_generation.load();

            hr = debugger->get_CurrentMode(&mode);
            if (FAILED(hr))
            {
                response = "error: unable to recheck debugger mode before break";
                return true;
            }
            if (mode == dbgBreakMode)
            {
                if (!WaitForVsBreakReadiness(
                        debugger,
                        initial_run_generation,
                        initial_break_generation,
                        initial_design_generation,
                        false,
                        response))
                {
                    bh_agent_logf("package.pipe", "already-break state did not stabilize: %s", response.c_str());
                    return true;
                }

                bh_agent_logf("package.pipe", "break ignored: debugger already in stable break mode");
                response = "ok: already break";
                return true;
            }
        }

        if (is_step_command)
        {
            if (!m_debugger_events_ready.load())
            {
                response = "error: debugger mode events unavailable for step synchronization";
                return true;
            }

            dbgDebugMode mode = dbgDesignMode;
            hr = debugger->get_CurrentMode(&mode);
            if (FAILED(hr))
            {
                response = "error: unable to determine debugger mode before step";
                return true;
            }
            if (mode != dbgBreakMode)
            {
                response = "error: step command requires debugger break mode";
                return true;
            }

            initial_run_generation = m_vs_run_generation.load();
            initial_break_generation = m_vs_break_generation.load();
            initial_design_generation = m_vs_design_generation.load();
        }

        if (is_continue_command)
        {
            initial_run_generation = m_vs_run_generation.load();
            initial_break_generation = m_vs_break_generation.load();
            initial_design_generation = m_vs_design_generation.load();
        }

        hr = dte->ExecuteCommand(CComBSTR(command_name), CComBSTR(L""));
        if (FAILED(hr))
        {
            if (is_break_command)
            {
                dbgDebugMode mode = dbgDesignMode;
                if (SUCCEEDED(debugger->get_CurrentMode(&mode)) && mode == dbgBreakMode)
                {
                    if (WaitForVsBreakReadiness(
                            debugger,
                            initial_run_generation,
                            initial_break_generation,
                            initial_design_generation,
                            false,
                            response))
                    {
                        bh_agent_logf(
                            "package.pipe",
                            "break command completed despite ExecuteCommand failure hr=0x%08X",
                            (unsigned)hr);
                        response = "ok: already break";
                    }
                    return true;
                }
            }

            bh_agent_logf("package.pipe", "step command '%s' failed: ExecuteCommand hr=0x%08X", message, (unsigned)hr);
            char err[96] = {};
            _snprintf_s(err, sizeof(err), _TRUNCATE, "error: ExecuteCommand failed hr=0x%08X", (unsigned)hr);
            response = err;
            return true;
        }

        bh_agent_logf("package.pipe", "executed VS step command: %s", message);
        if (is_step_command &&
            !WaitForVsStepCompletion(
                debugger,
                initial_run_generation,
                initial_break_generation,
                initial_design_generation,
                response))
        {
            bh_agent_logf("package.pipe", "VS step command '%s' did not stabilize: %s", message, response.c_str());
            return true;
        }
        if (is_break_command &&
            !WaitForVsBreakReadiness(
                debugger,
                initial_run_generation,
                initial_break_generation,
                initial_design_generation,
                true,
                response))
        {
            bh_agent_logf("package.pipe", "VS break command '%s' did not stabilize: %s", message, response.c_str());
            return true;
        }
        if (is_continue_command)
        {
            if (!m_debugger_events_ready.load())
            {
                bh_agent_logf("package.pipe", "continue run-mode wait skipped: debugger mode events unavailable");
                response = "ok: executed continue";
                return true;
            }

            std::string state;
            if (!WaitForVsRunReadiness(
                    initial_run_generation,
                    initial_break_generation,
                    initial_design_generation,
                    state,
                    response))
            {
                bh_agent_logf("package.pipe", "VS continue did not reach run mode: %s", response.c_str());
                return true;
            }

            response = "ok: executed continue | " + state;
            return true;
        }

        response = "ok: executed " + std::string(message);
        if (is_step_command || is_break_command)
        {
            response += " | stable break";
        }

        return true;
    }

    bool TryGetVsDebugStatus(const char* message, std::string& response)
    {
        if (!message || _stricmp(message, "status") != 0)
        {
            return false;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        dbgDebugMode mode = dbgDesignMode;
        hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr))
        {
            char err[96] = {};
            _snprintf_s(err, sizeof(err), _TRUNCATE, "error: could not get debugger mode hr=0x%08X", (unsigned)hr);
            response = err;
            return true;
        }

        std::string vs_status;
        if (mode == dbgBreakMode)
        {
            vs_status = "ok: vs break mode";
        }
        else if (mode == dbgRunMode)
        {
            vs_status = "ok: vs run mode";
        }
        else
        {
            vs_status = "ok: vs design mode";
        }

        std::string remote_status;
        ForwardMessageToRemotePipe("status", remote_status);

        if (mode == dbgBreakMode &&
            remote_status.find("running (no active break session)") != std::string::npos)
        {
            std::string capture_response;
            ForwardMessageToRemotePipe("capture break", capture_response);

            std::string refreshed_remote_status;
            ForwardMessageToRemotePipe("status", refreshed_remote_status);
            if (!refreshed_remote_status.empty())
            {
                remote_status = refreshed_remote_status;
            }
        }

        if (!remote_status.empty())
        {
            response = vs_status + " | " + remote_status;
        }
        else
        {
            response = vs_status;
        }

        return true;
    }

    bool TryListBreakpoints(const char* message, std::string& response)
    {
        if (!message || _stricmp(message, "list bp") != 0)
        {
            return false;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        CComPtr<::Breakpoints> breakpoints;
        hr = debugger->get_Breakpoints(&breakpoints);
        if (FAILED(hr) || !breakpoints)
        {
            response = "error: could not get breakpoints";
            return true;
        }

        long count = 0;
        hr = breakpoints->get_Count(&count);
        if (FAILED(hr))
        {
            response = "error: could not count breakpoints";
            return true;
        }

        if (count <= 0)
        {
            response = "ok: bp empty";
            return true;
        }

        char header[64] = {};
        _snprintf_s(header, sizeof(header), _TRUNCATE, "ok: bp count=%ld", count);
        response = header;

        for (long i = 1; i <= count; ++i)
        {
            CComVariant index(i);
            CComPtr<::Breakpoint> breakpoint;
            hr = breakpoints->Item(index, &breakpoint);
            if (FAILED(hr) || !breakpoint)
            {
                response += " | #" + std::to_string(i - 1) + " <unavailable>";
                continue;
            }

            CComBSTR file_bstr;
            long line = 0;
            (void)breakpoint->get_File(&file_bstr);
            (void)breakpoint->get_FileLine(&line);

            std::string file_utf8;
            if (file_bstr && file_bstr.Length() > 0)
            {
                (void)TryWideToUtf8(file_bstr, file_utf8);
            }

            response += " | #" + std::to_string(i - 1) + " ";
            if (!file_utf8.empty() && line > 0)
            {
                response += file_utf8 + ":" + std::to_string(line);
            }
            else if (!file_utf8.empty())
            {
                response += file_utf8;
            }
            else
            {
                response += "<non-file>";
            }
        }

        return true;
    }

    bool TryClearBreakpoints(const char* message, std::string& response)
    {
        if (!message ||
            (_stricmp(message, "clear bp") != 0 &&
             _stricmp(message, "clear all bp") != 0))
        {
            return false;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        CComPtr<::Breakpoints> breakpoints;
        hr = debugger->get_Breakpoints(&breakpoints);
        if (FAILED(hr) || !breakpoints)
        {
            response = "error: could not get breakpoints";
            return true;
        }

        long count = 0;
        hr = breakpoints->get_Count(&count);
        if (FAILED(hr))
        {
            response = "error: could not count breakpoints";
            return true;
        }

        long cleared_count = 0;
        for (long collection_index = count; collection_index >= 1; --collection_index)
        {
            CComVariant index(collection_index);
            CComPtr<::Breakpoint> breakpoint;
            hr = breakpoints->Item(index, &breakpoint);
            if (FAILED(hr) || !breakpoint)
            {
                response = "error: failed to get bp #" +
                    std::to_string(collection_index - 1) +
                    " after clearing " + std::to_string(cleared_count);
                return true;
            }

            hr = breakpoint->Delete();
            if (FAILED(hr))
            {
                response = "error: failed to clear bp #" +
                    std::to_string(collection_index - 1) +
                    " after clearing " + std::to_string(cleared_count);
                return true;
            }

            ++cleared_count;
        }

        bh_agent_logf("package.pipe", "cleared breakpoints count=%ld", cleared_count);
        response = "ok: bp cleared count=" + std::to_string(cleared_count);
        return true;
    }

    bool TryRemoveBreakpoint(const char* message, std::string& response)
    {
        if (!message)
        {
            return false;
        }

        if (_strnicmp(message, "remove bp ", 10) != 0)
        {
            return false;
        }

        const char* payload = message + 10;
        while (*payload == ' ' || *payload == '\t')
        {
            ++payload;
        }

        if (!*payload)
        {
            response = "error: missing bp index";
            return true;
        }

        char* end_ptr = nullptr;
        long bp_index = strtol(payload, &end_ptr, 10);
        if (end_ptr == payload || bp_index < 0)
        {
            response = "error: invalid bp index";
            return true;
        }

        while (*end_ptr)
        {
            if (*end_ptr != ' ' && *end_ptr != '\t')
            {
                response = "error: invalid bp index";
                return true;
            }
            ++end_ptr;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        CComPtr<::Breakpoints> breakpoints;
        hr = debugger->get_Breakpoints(&breakpoints);
        if (FAILED(hr) || !breakpoints)
        {
            response = "error: could not get breakpoints";
            return true;
        }

        long count = 0;
        hr = breakpoints->get_Count(&count);
        if (FAILED(hr))
        {
            response = "error: could not count breakpoints";
            return true;
        }

        long collection_index = bp_index + 1;
        if (collection_index <= 0 || collection_index > count)
        {
            response = "error: bp index out of range";
            return true;
        }

        CComVariant index(collection_index);
        CComPtr<::Breakpoint> breakpoint;
        hr = breakpoints->Item(index, &breakpoint);
        if (FAILED(hr) || !breakpoint)
        {
            response = "error: bp not found";
            return true;
        }

        CComBSTR file_bstr;
        long line = 0;
        (void)breakpoint->get_File(&file_bstr);
        (void)breakpoint->get_FileLine(&line);

        std::string file_utf8;
        if (file_bstr && file_bstr.Length() > 0)
        {
            (void)TryWideToUtf8(file_bstr, file_utf8);
        }

        hr = breakpoint->Delete();
        if (FAILED(hr))
        {
            response = "error: failed to remove bp";
            return true;
        }

        if (!file_utf8.empty() && line > 0)
        {
            response = "ok: bp removed #" + std::to_string(bp_index) + " " + file_utf8 + ":" + std::to_string(line);
        }
        else
        {
            response = "ok: bp removed #" + std::to_string(bp_index);
        }

        return true;
    }

    bool TrySetFileLineBreakpoint(const char* message, std::string& response)
    {
        if (!message)
        {
            return false;
        }

        if (_strnicmp(message, "set bp ", 7) != 0)
        {
            return false;
        }

        const char* payload = message + 7;
        while (*payload == ' ' || *payload == '\t')
        {
            ++payload;
        }

        if (!*payload)
        {
            return false;
        }

        const char* colon = strrchr(payload, ':');
        if (!colon)
        {
            return false;
        }

        char* end_ptr = nullptr;
        long line = strtol(colon + 1, &end_ptr, 10);
        if (end_ptr == (colon + 1) || line <= 0)
        {
            return false;
        }

        while (*end_ptr)
        {
            if (*end_ptr != ' ' && *end_ptr != '\t')
            {
                return false;
            }
            ++end_ptr;
        }

        std::string file_path(payload, (size_t)(colon - payload));
        while (!file_path.empty() && (file_path.back() == ' ' || file_path.back() == '\t'))
        {
            file_path.pop_back();
        }

        if (file_path.empty())
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: empty file path");
            response = "error: empty file path";
            return true;
        }

        if (m_is_stopping.load())
        {
            bh_agent_logf("package.pipe", "set bp file:line skipped: package stopping");
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: no service provider");
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: QueryService hr=0x%08X", (unsigned)hr);
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: get_Debugger hr=0x%08X", (unsigned)hr);
            response = "error: could not get debugger";
            return true;
        }

        CComPtr<::Breakpoints> breakpoints;
        hr = debugger->get_Breakpoints(&breakpoints);
        if (FAILED(hr) || !breakpoints)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: get_Breakpoints hr=0x%08X", (unsigned)hr);
            response = "error: could not get breakpoints";
            return true;
        }

        int wide_len = MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, nullptr, 0);
        if (wide_len <= 0)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: path conversion");
            response = "error: path conversion failed";
            return true;
        }

        std::wstring wide_file;
        wide_file.resize((size_t)wide_len);
        if (MultiByteToWideChar(CP_UTF8, 0, file_path.c_str(), -1, &wide_file[0], wide_len) <= 0)
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: path conversion 2");
            response = "error: path conversion failed";
            return true;
        }

        CComPtr<::Breakpoints> added_breakpoints;
        hr = breakpoints->Add(
            CComBSTR(L""),
            CComBSTR(wide_file.c_str()),
            line,
            1,
            CComBSTR(L""),
            dbgBreakpointConditionTypeWhenTrue,
            CComBSTR(L""),
            CComBSTR(L""),
            0,
            CComBSTR(L""),
            1,
            dbgHitCountTypeNone,
            &added_breakpoints);

        if (FAILED(hr))
        {
            bh_agent_logf("package.pipe", "set bp file:line failed: Add hr=0x%08X path=%s line=%ld", (unsigned)hr, file_path.c_str(), line);
            response = "error: set bp file:line failed";
        }
        else
        {
            bh_agent_logf("package.pipe", "set bp file:line ok path=%s line=%ld", file_path.c_str(), line);
            response = "ok: bp set " + file_path + ":" + std::to_string(line);
        }

        return true;
    }

    void SanitizeResponseField(std::string& text, size_t max_length)
    {
        for (size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(text[i]);
            if (ch < 0x20 || ch == 0x7F || ch == '|')
            {
                text[i] = ' ';
            }
        }

        if (text.size() > max_length)
        {
            size_t truncate_at = max_length;
            while (truncate_at > 0 &&
                   (static_cast<unsigned char>(text[truncate_at]) & 0xC0) == 0x80)
            {
                --truncate_at;
            }

            text.resize(truncate_at);
            text += "...";
        }
    }

    bool TryEvaluateExpression(const char* message, std::string& response)
    {
        bool allow_side_effects = false;
        const char* expression_text = nullptr;
        const bh_eval_grammar::parse_result parsed = bh_eval_grammar::parse(message, allow_side_effects, expression_text);
        if (parsed == bh_eval_grammar::parse_result::not_eval)
        {
            return false;
        }

        if (parsed == bh_eval_grammar::parse_result::usage)
        {
            response = "error: eval usage: eval [--side-effects] <expr>";
            return true;
        }

        if (strlen(expression_text) > 1024)
        {
            response = "error: eval expression too long (max 1024 bytes)";
            return true;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        if (!allow_side_effects)
        {
            return EvaluateExpressionWithoutSideEffects(expression_text, provider, response);
        }

        return EvaluateExpressionWithSideEffects(expression_text, provider, response);
    }

    // Default path: Concord evaluation with side effects and function evaluation blocked.
    bool EvaluateExpressionWithoutSideEffects(const char* expression_text, IServiceProvider* provider, std::string& response)
    {
        std::wstring expression_wide;
        if (!TryUtf8ToWide(expression_text, expression_wide))
        {
            response = "error: eval expression conversion failed";
            return true;
        }

        bh_eval::frame_request* request = nullptr;
        if (!m_eval_dispatcher.capture(provider, request, response))
        {
            return true;
        }

        CComPtr<Microsoft::VisualStudio::Debugger::CallStack::DkmStackFrame> frame = request->frame;
        const std::string capture_error = request->error;
        request->release();
        if (!capture_error.empty())
        {
            response = capture_error;
            return true;
        }

        bh_eval::outcome outcome;
        bh_eval::evaluate(frame, expression_wide, outcome);
        if (!outcome.evaluated)
        {
            response = outcome.error;
            return true;
        }

        return FormatEvalResponse(expression_text, outcome.type.c_str(), outcome.value.c_str(), outcome.valid, response);
    }

    // Explicit opt-in (eval --side-effects): the EnvDTE evaluator may assign variables or
    // call functions in the debuggee.
    bool EvaluateExpressionWithSideEffects(const char* expression_text, IServiceProvider* provider, std::string& response)
    {
        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        dbgDebugMode mode = dbgDesignMode;
        hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr))
        {
            response = "error: could not get debugger mode";
            return true;
        }

        if (mode != dbgBreakMode)
        {
            response = "error: eval requires break mode";
            return true;
        }

        std::wstring expression_wide;
        if (!TryUtf8ToWide(expression_text, expression_wide))
        {
            response = "error: eval expression conversion failed";
            return true;
        }

        CComPtr<::Expression> expression;
        hr = debugger->GetExpression(
            CComBSTR(expression_wide.c_str()),
            VARIANT_FALSE,
            5000,
            &expression);
        if (FAILED(hr) || !expression)
        {
            if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ||
                hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT) ||
                hr == RPC_E_TIMEOUT)
            {
                response = "error: eval timeout";
            }
            else
            {
                char err[96] = {};
                _snprintf_s(err, sizeof(err), _TRUNCATE,
                    "error: eval failed hr=0x%08X", (unsigned)hr);
                response = err;
            }
            return true;
        }

        VARIANT_BOOL is_valid = VARIANT_FALSE;
        hr = expression->get_IsValidValue(&is_valid);
        if (FAILED(hr))
        {
            response = "error: eval validity check failed";
            return true;
        }

        CComBSTR type_bstr;
        CComBSTR value_bstr;
        (void)expression->get_Type(&type_bstr);
        (void)expression->get_Value(&value_bstr);
        return FormatEvalResponse(expression_text, type_bstr.m_str, value_bstr.m_str, is_valid != VARIANT_FALSE, response);
    }

    bool FormatEvalResponse(const char* expression_text, const wchar_t* type_text, const wchar_t* value_text, bool valid, std::string& response)
    {
        std::string requested_utf8(expression_text);
        std::string type_utf8;
        std::string value_utf8;
        if (type_text && *type_text &&
            !TryWideToUtf8(type_text, type_utf8))
        {
            response = "error: eval type conversion failed";
            return true;
        }

        if (value_text && *value_text &&
            !TryWideToUtf8(value_text, value_utf8))
        {
            response = "error: eval value conversion failed";
            return true;
        }

        SanitizeResponseField(requested_utf8, 512);
        SanitizeResponseField(type_utf8, 256);
        SanitizeResponseField(value_utf8, 1024);

        if (!valid)
        {
            response = "error: eval invalid expression=" + requested_utf8;
            if (!value_utf8.empty())
            {
                response += " value=" + value_utf8;
            }
            return true;
        }

        if (type_utf8.empty())
        {
            type_utf8 = "<unknown>";
        }
        if (value_utf8.empty())
        {
            value_utf8 = "<empty>";
        }

        response = "ok: eval expr=" + requested_utf8 +
            " type=" + type_utf8 + " value=" + value_utf8;
        return true;
    }

    bool TryListLocalVariables(const char* message, std::string& response)
    {
        if (!message)
        {
            return false;
        }

        unsigned int requested_count = 32;
        if (_stricmp(message, "local variables") == 0 ||
            _stricmp(message, "locals") == 0)
        {
        }
        else if (_strnicmp(message, "local variables ", 16) == 0)
        {
            if (!TryParseUnsigned(message + 16, requested_count))
            {
                response = "error: local variables parse failed";
                return true;
            }
        }
        else if (_strnicmp(message, "locals ", 7) == 0)
        {
            if (!TryParseUnsigned(message + 7, requested_count))
            {
                response = "error: locals parse failed";
                return true;
            }
        }
        else
        {
            return false;
        }

        if (requested_count > 128)
        {
            requested_count = 128;
        }

        if (m_is_stopping.load())
        {
            response = "error: package stopping";
            return true;
        }

        CComPtr<IServiceProvider> provider = GetServiceProviderSnapshot();
        if (!provider)
        {
            response = "error: no service provider";
            return true;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            response = "error: could not get DTE";
            return true;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            response = "error: could not get debugger";
            return true;
        }

        dbgDebugMode mode = dbgDesignMode;
        hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr))
        {
            response = "error: could not get debugger mode";
            return true;
        }

        if (mode != dbgBreakMode)
        {
            response = "error: local variables requires break mode";
            return true;
        }

        CComPtr<::StackFrame> frame;
        hr = debugger->get_CurrentStackFrame(&frame);
        if (FAILED(hr) || !frame)
        {
            response = "error: no active stack frame";
            return true;
        }

        CComPtr<::Expressions> locals;
        hr = frame->get_Locals(&locals);
        if (FAILED(hr) || !locals)
        {
            response = "error: could not get locals";
            return true;
        }

        long total_count = 0;
        hr = locals->get_Count(&total_count);
        if (FAILED(hr))
        {
            response = "error: could not count locals";
            return true;
        }

        if (total_count <= 0)
        {
            response = "ok: locals empty";
            return true;
        }

        unsigned int shown_count = requested_count;
        if (shown_count > static_cast<unsigned int>(total_count))
        {
            shown_count = static_cast<unsigned int>(total_count);
        }

        response = "ok: locals count=" + std::to_string(total_count) +
            " shown=" + std::to_string(shown_count);

        for (unsigned int i = 0; i < shown_count; ++i)
        {
            CComVariant index(static_cast<long>(i + 1));
            CComPtr<::Expression> expression;
            hr = locals->Item(index, &expression);
            if (FAILED(hr) || !expression)
            {
                response += " | #" + std::to_string(i) + " <unavailable>";
                continue;
            }

            CComBSTR name_bstr;
            CComBSTR type_bstr;
            CComBSTR value_bstr;
            (void)expression->get_Name(&name_bstr);
            (void)expression->get_Type(&type_bstr);
            (void)expression->get_Value(&value_bstr);

            std::string name_utf8;
            std::string type_utf8;
            std::string value_utf8;
            if (name_bstr && name_bstr.Length() > 0)
            {
                (void)TryWideToUtf8(name_bstr, name_utf8);
            }

            if (type_bstr && type_bstr.Length() > 0)
            {
                (void)TryWideToUtf8(type_bstr, type_utf8);
            }

            if (value_bstr && value_bstr.Length() > 0)
            {
                (void)TryWideToUtf8(value_bstr, value_utf8);
            }

            if (name_utf8.empty())
            {
                name_utf8 = "<unnamed>";
            }

            response += " | #" + std::to_string(i) + " " + name_utf8;
            if (!type_utf8.empty())
            {
                response += ":" + type_utf8;
            }

            if (!value_utf8.empty())
            {
                response += "=" + value_utf8;
            }
        }

        if (shown_count < static_cast<unsigned int>(total_count))
        {
            response += " | ...";
        }

        return true;
    }

    void ForwardMessageToRemotePipe(const char* message, std::string& response)
    {
        SendMessageToLocalComponent(message, response);
    }

    void SendMessageToLocalComponent(const char* message, std::string& response)
    {
        if (!message || !message[0])
        {
            response = "error: empty message";
            return;
        }

        CComPtr<Microsoft::VisualStudio::Debugger::DkmVariant> message_variant;
        CComPtr<Microsoft::VisualStudio::Debugger::DkmCustomMessage> request;
        CComPtr<Microsoft::VisualStudio::Debugger::DkmCustomMessage> reply;
        std::wstring message_wide;

        CComPtr<Microsoft::VisualStudio::Debugger::DkmProcess> process;
        if (!TryGetActiveDebugProcess(process) || !process)
        {
            response = "error: no active debug process";
            return;
        }

        if (!TryUtf8ToWide(message, message_wide))
        {
            response = "error: message conversion failed";
            return;
        }

        HRESULT hr = Microsoft::VisualStudio::Debugger::DkmVariant::Create(message_wide.c_str(), &message_variant);
        if (FAILED(hr) || !message_variant)
        {
            response = "error: DkmVariant::Create failed";
            return;
        }

        static const GUID bh_guid_agent_message_to_local =
        {
            0x1f3d8a6e,
            0x8de6,
            0x4ccb,
            { 0xb1, 0x8c, 0x3a, 0xa4, 0x41, 0x02, 0xf5, 0x01 }
        };
        static const UINT32 bh_agent_message_from_script = 2;

        hr = Microsoft::VisualStudio::Debugger::DkmCustomMessage::Create(
            process->Connection(),
            process,
            bh_guid_agent_message_to_local,
            bh_agent_message_from_script,
            message_variant,
            nullptr,
            &request);
        if (FAILED(hr) || !request)
        {
            response = "error: DkmCustomMessage::Create failed";
            return;
        }

        bh_agent_logf("package.pipe", "sending DkmCustomMessage to local component");
        hr = request->SendLower(&reply);
        if (FAILED(hr))
        {
            char err[128] = {};
            _snprintf_s(err, sizeof(err), _TRUNCATE, "error: SendLower failed hr=0x%08X", (unsigned)hr);
            response = err;
            return;
        }
        bh_agent_logf("package.pipe", "SendLower to local component completed");

        if (!reply || !reply->Parameter1())
        {
            response = "ok: local custom message sent";
            return;
        }

        const VARIANT& value = reply->Parameter1()->Value();
        if (value.vt == VT_BSTR && value.bstrVal)
        {
            if (!TryWideToUtf8(value.bstrVal, response))
            {
                response = "error: response conversion failed";
            }
        }

        if (response.empty())
        {
            response = "ok: local custom message sent";
        }

    }

    bool TryGetActiveDebugProcess(CComPtr<Microsoft::VisualStudio::Debugger::DkmProcess>& process)
    {
        process.Release();

        Microsoft::VisualStudio::Debugger::CAutoDkmArray<Microsoft::VisualStudio::Debugger::DkmProcess*> processes;
        if (FAILED(Microsoft::VisualStudio::Debugger::DkmProcess::GetProcesses(&processes)) || processes.Length == 0)
        {
            return false;
        }

        for (UINT32 i = 0; i < processes.Length; ++i)
        {
            Microsoft::VisualStudio::Debugger::DkmProcess* candidate = processes.Members[i];
            if (!candidate || !candidate->Connection())
            {
                continue;
            }

            if (candidate->LivePart())
            {
                process = candidate;
                return true;
            }
        }

        for (UINT32 i = 0; i < processes.Length; ++i)
        {
            Microsoft::VisualStudio::Debugger::DkmProcess* candidate = processes.Members[i];
            if (!candidate || !candidate->Connection())
            {
                continue;
            }

            process = candidate;
            return true;
        }

        return false;
    }

    bool TryUtf8ToWide(const char* text, std::wstring& wide)
    {
        wide.clear();
        if (!text)
        {
            return false;
        }

        int wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (wide_len <= 0)
        {
            return false;
        }

        if (wide_len == 1)
        {
            return true;
        }

        wide.resize((size_t)wide_len - 1);
        return MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], wide_len) > 0;
    }

    bool TryWideToUtf8(const wchar_t* text, std::string& utf8)
    {
        utf8.clear();
        if (!text)
        {
            return false;
        }

        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 0)
        {
            return false;
        }

        if (utf8_len == 1)
        {
            return true;
        }

        utf8.resize((size_t)utf8_len - 1);
        return WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], utf8_len, nullptr, nullptr) > 0;
    }

    std::wstring BuildAgentPipeName()
    {
        std::wstring base = L"bh_agent_debugger_agent";
        m_target_id_utf8 = std::string("vs_") + std::to_string(GetCurrentProcessId());

        std::wstring suffix_w;
        if (!TryUtf8ToWide(m_target_id_utf8.c_str(), suffix_w) || suffix_w.empty())
        {
            return base;
        }

        return base + L"_" + suffix_w;
    }

    bool FileExists(const wchar_t* path)
    {
        if (!path || !path[0])
        {
            return false;
        }

        DWORD attrs = GetFileAttributesW(path);
        return (attrs != INVALID_FILE_ATTRIBUTES) && ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
    }

    bool TryGetDirectoryFromPath(const std::wstring& full_path, std::wstring& directory)
    {
        directory.clear();
        if (full_path.empty())
        {
            return false;
        }

        size_t slash_pos = full_path.find_last_of(L"\\/");
        if (slash_pos == std::wstring::npos)
        {
            return false;
        }

        directory = full_path.substr(0, slash_pos);
        return !directory.empty();
    }

    bool TryResolveServiceExePath(std::wstring& service_exe_path)
    {
        service_exe_path.clear();

        wchar_t env_path[1024] = {};
        DWORD env_len = GetEnvironmentVariableW(L"BH_AGENT_SERVICE_EXE", env_path, static_cast<DWORD>(sizeof(env_path) / sizeof(env_path[0])));
        if (env_len > 0 && env_len < (sizeof(env_path) / sizeof(env_path[0])) && FileExists(env_path))
        {
            service_exe_path = env_path;
            return true;
        }

        std::wstring module_path;
        wchar_t current_module[MAX_PATH] = {};
        HMODULE module_handle = nullptr;
        if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&bh_agent_debugger_package::AgentPipeThreadProc),
            &module_handle) && module_handle)
        {
            DWORD len = GetModuleFileNameW(module_handle, current_module, MAX_PATH);
            if (len > 0 && len < MAX_PATH)
            {
                module_path.assign(current_module, current_module + len);
            }
        }

        std::wstring module_dir;
        if (TryGetDirectoryFromPath(module_path, module_dir))
        {
            const std::wstring candidate = module_dir + L"\\bh_agent_debugger_service.exe";
            if (FileExists(candidate.c_str()))
            {
                service_exe_path = candidate;
                return true;
            }
        }

        return false;
    }

    bool StartBrokerServiceProcess()
    {
        if (IsBrokerServiceProcessRunning())
        {
            return true;
        }

        std::wstring service_exe_path;
        if (!TryResolveServiceExePath(service_exe_path))
        {
            bh_agent_logf("package.pipe", "service launch skipped: exe path not found");
            return false;
        }

        std::wstring command_line = L"\"" + service_exe_path + L"\"";
        STARTUPINFOW si = {};
        PROCESS_INFORMATION pi = {};
        si.cb = sizeof(si);

        BOOL ok = CreateProcessW(
            nullptr,
            &command_line[0],
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi);
        if (!ok)
        {
            bh_agent_logf("package.pipe", "service launch failed path=%ws err=%lu", service_exe_path.c_str(), (unsigned long)GetLastError());
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        bh_agent_logf("package.pipe", "service launch requested path=%ws", service_exe_path.c_str());
        return true;
    }

    bool IsBrokerServiceProcessRunning()
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot, &entry))
        {
            CloseHandle(snapshot);
            return false;
        }

        bool found = false;
        do
        {
            if (_wcsicmp(entry.szExeFile, L"bh_agent_debugger_service.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"bh_agent_debugger.exe") == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));

        CloseHandle(snapshot);
        return found;
    }

    bool EnsureBrokerRunning(const wchar_t* broker_path)
    {
        if (!broker_path)
        {
            return false;
        }

        if (WaitNamedPipeW(broker_path, 200))
        {
            return true;
        }

        static ULONGLONG last_launch_attempt_ms = 0;
        ULONGLONG now_ms = GetTickCount64();
        if ((now_ms - last_launch_attempt_ms) > 15000)
        {
            last_launch_attempt_ms = now_ms;
            (void)StartBrokerServiceProcess();
        }

        return WaitNamedPipeW(broker_path, 2500) != FALSE;
    }

    bool RegisterTargetWithBroker(const std::string& target_id, const std::string& pipe_name)
    {
        const wchar_t* broker_path = L"\\\\.\\pipe\\bh_agent_ctl";
        if (!EnsureBrokerRunning(broker_path))
        {
            return false;
        }

        HANDLE broker = CreateFileW(
            broker_path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (broker == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        // Fail closed if another account created the broker pipe name.
        if (!bh_agent_pipe_security::server_owned_by_current_user(broker))
        {
            bh_agent_logf("package.pipe", "broker pipe rejected: not owned by current user");
            CloseHandle(broker);
            return false;
        }

        std::string request =
            std::string("{\"protocolVersion\":1,\"id\":\"pkg-register\",\"method\":\"targets.register\",\"targetId\":\"") +
            target_id +
            "\",\"pipeName\":\"" +
            pipe_name +
            "\",\"packageVersion\":\"" +
            bh_agent_version::version +
            "\",\"timeoutMs\":1000}\n";

        DWORD written = 0;
        bool ok = (WriteFile(broker, request.c_str(), static_cast<DWORD>(request.size()), &written, nullptr) != FALSE);
        if (ok)
        {
            char response_buffer[512] = {};
            DWORD read_bytes = 0;
            ok = (ReadFile(broker, response_buffer, sizeof(response_buffer) - 1, &read_bytes, nullptr) != FALSE);
        }

        CloseHandle(broker);
        return ok;
    }

    void RegisterPipeTargetWithBroker()
    {
        std::string pipe_name_utf8;
        if (!TryWideToUtf8(m_pipe_name.c_str(), pipe_name_utf8) || pipe_name_utf8.empty())
        {
            m_broker_registered = false;
            return;
        }

        std::string target_id = m_target_id_utf8;
        if (target_id.empty())
        {
            target_id = std::string("vs_") + std::to_string(GetCurrentProcessId());
        }

        bool registered = RegisterTargetWithBroker(target_id, pipe_name_utf8);
        if (registered && !m_broker_registered)
        {
            bh_agent_logf("package.pipe", "broker registration active targetId=%hs pipe=%hs", target_id.c_str(), pipe_name_utf8.c_str());
        }
        else if (!registered && m_broker_registered)
        {
            bh_agent_logf("package.pipe", "broker registration lost targetId=%hs", target_id.c_str());
        }

        m_broker_registered = registered;
    }

    // One overlapped read or write on a pipe opened with FILE_FLAG_OVERLAPPED, cancelled after timeout_ms.
    static bool TransferBrokerPipeBytes(HANDLE pipe, bool write, char* buffer, DWORD size, DWORD timeout_ms, DWORD& transferred)
    {
        transferred = 0;
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent)
        {
            return false;
        }

        const BOOL started = write
            ? WriteFile(pipe, buffer, size, nullptr, &overlapped)
            : ReadFile(pipe, buffer, size, nullptr, &overlapped);
        bool ok = false;
        if (started || GetLastError() == ERROR_IO_PENDING)
        {
            if (WaitForSingleObject(overlapped.hEvent, timeout_ms) != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &overlapped);
            }

            ok = GetOverlappedResult(pipe, &overlapped, &transferred, TRUE) != FALSE;
        }

        CloseHandle(overlapped.hEvent);
        return ok;
    }

    // Removes this instance from the broker's target list when the package closes, so a closed
    // Visual Studio isn't listed until the broker prunes it. Never starts a broker; every wait is
    // bounded and stays below the 5 s broker-thread wait in StopAgentPipeServer.
    void UnregisterTargetFromBroker()
    {
        std::string target_id = m_target_id_utf8;
        if (target_id.empty())
        {
            target_id = std::string("vs_") + std::to_string(GetCurrentProcessId());
        }

        m_broker_registered = false;
        const wchar_t* broker_path = L"\\\\.\\pipe\\bh_agent_ctl";
        if (!WaitNamedPipeW(broker_path, 500))
        {
            bh_agent_logf("package.pipe", "broker unregister skipped targetId=%hs err=%lu", target_id.c_str(), (unsigned long)GetLastError());
            return;
        }

        HANDLE broker = CreateFileW(
            broker_path,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (broker == INVALID_HANDLE_VALUE)
        {
            bh_agent_logf("package.pipe", "broker unregister skipped targetId=%hs open err=%lu", target_id.c_str(), (unsigned long)GetLastError());
            return;
        }

        // Fail closed if another account created the broker pipe name.
        if (!bh_agent_pipe_security::server_owned_by_current_user(broker))
        {
            bh_agent_logf("package.pipe", "broker pipe rejected: not owned by current user");
            CloseHandle(broker);
            return;
        }

        std::string request =
            std::string("{\"protocolVersion\":1,\"id\":\"pkg-unregister\",\"method\":\"targets.unregister\",\"targetId\":\"") +
            target_id +
            "\",\"timeoutMs\":1000}\n";

        char response_buffer[512] = {};
        DWORD transferred = 0;
        bool ok = TransferBrokerPipeBytes(broker, true, &request[0], static_cast<DWORD>(request.size()), 1000, transferred) &&
            transferred == request.size();
        if (ok)
        {
            ok = TransferBrokerPipeBytes(broker, false, response_buffer, sizeof(response_buffer) - 1, 1000, transferred);
            response_buffer[ok ? transferred : 0] = 0;
        }

        CloseHandle(broker);

        std::string response = response_buffer;
        while (!response.empty() && (response.back() == '\r' || response.back() == '\n'))
        {
            response.pop_back();
        }

        bh_agent_logf("package.pipe", "broker unregister targetId=%hs sent=%d response=%hs", target_id.c_str(), ok ? 1 : 0, response.c_str());
    }

    HRESULT EnsureEventHandlerReady()
    {
        m_state_lock.Lock();
        if (m_event_handler)
        {
            m_state_lock.Unlock();
            return S_OK;
        }

        CComObject<bh_agent_debugger_event_handler>* handler = nullptr;
        HRESULT hr = CComObject<bh_agent_debugger_event_handler>::CreateInstance(&handler);
        if (FAILED(hr))
        {
            m_state_lock.Unlock();
            return hr;
        }

        handler->AddRef();
        m_event_handler.Attach(handler);
        m_event_handler_provider = GetVsSiteCache().GetSite();
        handler->SetServiceProvider(m_event_handler_provider);
        m_state_lock.Unlock();
        return S_OK;
    }

    CComPtr<IServiceProvider> GetServiceProviderSnapshot()
    {
        CComPtr<IServiceProvider> provider;
        m_state_lock.Lock();
        provider = m_event_handler_provider;
        m_state_lock.Unlock();
        return provider;
    }

    CComPtr<IVsCustomDebuggerEventHandler110> m_event_handler;
    CComPtr<IServiceProvider> m_event_handler_provider;
    CComPtr<IVsDebugger> m_shell_debugger;
    VSCOOKIE m_debugger_events_cookie = VSCOOKIE_NIL;
    CComAutoCriticalSection m_state_lock;
    std::atomic<bool> m_is_stopping{ false };
    std::atomic<bool> m_debugger_events_ready{ false };
    std::atomic<unsigned long long> m_vs_run_generation{ 0 };
    std::atomic<unsigned long long> m_vs_break_generation{ 0 };
    std::atomic<unsigned long long> m_vs_design_generation{ 0 };
    std::atomic<int> m_vs_event_mode{ -1 };
    bh_eval::ui_dispatcher m_eval_dispatcher;
    HANDLE m_pipe_thread = nullptr;
    HANDLE m_pipe_stop_event = nullptr;
    HANDLE m_broker_thread = nullptr;
    HANDLE m_broker_stop_event = nullptr;
    bool m_broker_registered = false;
    std::wstring m_pipe_name = L"bh_agent_debugger_agent";
    std::string m_target_id_utf8;
};

OBJECT_ENTRY_AUTO(CLSID_bh_agent_debugger_package, bh_agent_debugger_package)
