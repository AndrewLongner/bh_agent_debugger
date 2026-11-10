#include "bh_agent_remote_component.h"
#include "..\bh_agent_log.h"
#include "bh_typedef.h"

#include <algorithm>
#include <string.h>
#include <string>
#include <vector>
#include <wchar.h>

using namespace Microsoft::VisualStudio::Debugger::Breakpoints;
using namespace Microsoft::VisualStudio::Debugger::Native;

#define bh_check_remote_hr(expr) bh_agent_check_hr("engine.remote", (expr))

namespace
{
const GUID bh_guid_agent_remote_component = {
	0x3fd6e14f, 0x1d4e, 0x478b, {0x8d, 0x7, 0xc9, 0x4e, 0x7e, 0x55, 0x2, 0x1d}};
const GUID bh_guid_agent_trap_breakpoint = {
	0x4ad5e4c2,
	0x9d7a,
	0x4be8,
	{0xa4, 0x55, 0x34, 0x63, 0x84, 0x16, 0x2d, 0x11}};
const GUID bh_guid_agent_message_to_local = {
	0x1f3d8a6e, 0x8de6, 0x4ccb, {0xb1, 0x8c, 0x3a, 0xa4, 0x41, 0x2, 0xf5, 0x1}};
const GUID bh_guid_agent_message_to_local_worker = {
	0xa5d0071b, 0xd3b8, 0x43b5, {0x8e, 0xd9, 0xdd, 0xb2, 0x4, 0xf6, 0xe3, 0x3}};
const GUID bh_guid_agent_message_to_remote = {
	0xdd0faea9, 0x3c8a, 0x44b8, {0xa4, 0xb7, 0xcb, 0x53, 0xa9, 0xd6, 0x2b, 0x0b}};
const UINT32 bh_agent_message_from_script = 2;
const UINT32 bh_message_to_local_resolve_symbol = 1;
const UINT32 bh_message_to_local_worker_resolve_symbol = 1;
const UINT32 bh_message_to_local_worker_resolve_address = 2;

struct bh_remote_breakpoint_state
{
	struct trap_entry
	{
		UINT64 id = 0;
		UINT64 address = 0;
		std::string symbol_name;
		CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint> breakpoint;
	};

	DWORD process_id = 0;
	GUID process_unique_id = {};
	UINT64 address = 0;
	bool has_target = false;
	bool trap_enabled = false;
	UINT64 next_trap_id = 1;
	CComPtr<DkmNativeRuntimeInstance> runtime_instance;
	CComPtr<DkmNativeModuleInstance> module_instance;
	CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint> trap_breakpoint;
	CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint>
		forwarded_trap_breakpoint;
	std::vector<trap_entry> traps;
};

struct bh_break_session_state
{
	enum class reason_type
	{
		unknown = 0,
		trap = 1,
		breakpoint = 2,
		step = 3,
		async_break = 4,
		exception = 5
	};

	bool active = false;
	DWORD process_id = 0;
	GUID process_unique_id = {};
	UINT32 thread_id = 0;
	UINT64 instruction_pointer = 0;
	UINT64 trap_address = 0;
	UINT64 generation = 0;
	bool has_exception = false;
	reason_type reason = reason_type::unknown;
};

const char *bh_break_reason_to_text(bh_break_session_state::reason_type reason)
{
	switch (reason)
	{
	case bh_break_session_state::reason_type::trap:
		return "trap";
	case bh_break_session_state::reason_type::breakpoint:
		return "breakpoint";
	case bh_break_session_state::reason_type::step:
		return "step";
	case bh_break_session_state::reason_type::async_break:
		return "async_break";
	case bh_break_session_state::reason_type::exception:
		return "exception";
	default:
		return "break";
	}
}

struct bh_pending_action
{
	enum class action_type
	{
		set_trap_symbol,
		set_trap_address,
		continue_exec
	};

	UINT64 id = 0;
	action_type type = action_type::set_trap_symbol;
	std::string symbol_name;
	UINT64 address = 0;
	UINT32 attempts = 0;
};

struct bh_pending_exception_state
{
	bool pending = false;
	DWORD process_id = 0;
	UINT64 thread_marker = 0;
	UINT32 code = 0;
	UINT32 stage = 0;
	ULONGLONG tick_ms = 0;
	std::string name;
};

class ATL_NO_VTABLE __declspec(uuid("52D363E8-8D6B-4E4C-A0E9-590A6A0F41E2"))
bh_agent_remote_process_data : public IUnknown,
							   public CComObjectRootEx<CComMultiThreadModel>
{
  public:
	SRWLOCK breakpoint_lock;
	SRWLOCK trap_session_lock;
	SRWLOCK exception_lock;
	volatile long runtime_onhit_dispatching;
	UINT64 break_session_generation;
	bh_remote_breakpoint_state breakpoint_state;
	bh_break_session_state trap_session;
	bh_pending_exception_state pending_exception;

	bh_agent_remote_process_data() :
		runtime_onhit_dispatching(0),
		break_session_generation(0)
	{
		InitializeSRWLock(&breakpoint_lock);
		InitializeSRWLock(&trap_session_lock);
		InitializeSRWLock(&exception_lock);
	}

	static HRESULT GetInstance(DkmProcess *process,
							   bh_agent_remote_process_data **process_data)
	{
		if (!process || !process_data)
		{
			return E_POINTER;
		}

		HRESULT hr = process->GetDataItem(process_data);
		if (hr == S_OK)
		{
			return S_OK;
		}

		CComObject<bh_agent_remote_process_data> *com_object = nullptr;
		hr = CComObject<bh_agent_remote_process_data>::CreateInstance(&com_object);
		if (FAILED(hr) || !com_object)
		{
			return FAILED(hr) ? hr : E_FAIL;
		}

		CComPtr<bh_agent_remote_process_data> created_instance(com_object);
		hr = process->SetDataItem(DkmDataCreationDisposition::CreateNew,
								  created_instance);
		if (FAILED(hr))
		{
			return hr;
		}

		*process_data = created_instance.Detach();
		return S_OK;
	}

  protected:
	HRESULT _InternalQueryInterface(REFIID riid, void **ppvObject)
	{
		if (!ppvObject)
		{
			return E_POINTER;
		}

		if (riid == __uuidof(IUnknown))
		{
			*ppvObject = static_cast<IUnknown *>(this);
			AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
};

class ATL_NO_VTABLE __declspec(uuid("D4E92717-8CA0-4D79-8B2D-C8B9F045A2C1"))
bh_agent_remote_connection_data : public IUnknown,
									 public CComObjectRootEx<CComMultiThreadModel>
{
  public:
	SRWLOCK state_lock;
	SRWLOCK pending_lock;
	GUID active_process_unique_id;
	bool has_active_process;
	bool target_pid_selected;
	DWORD target_pid;
	bool tracked_target_pid_valid;
	DWORD tracked_target_pid;
	UINT64 next_action_id;
	std::vector<bh_pending_action> pending_actions;

	bh_agent_remote_connection_data() :
		active_process_unique_id{},
		has_active_process(false),
		target_pid_selected(false),
		target_pid(0),
		tracked_target_pid_valid(false),
		tracked_target_pid(0),
		next_action_id(1)
	{
		InitializeSRWLock(&state_lock);
		InitializeSRWLock(&pending_lock);
	}

	static HRESULT GetInstance(DefaultPort::DkmTransportConnection *connection,
							   bh_agent_remote_connection_data **connection_data)
	{
		if (!connection || !connection_data)
		{
			return E_POINTER;
		}

		HRESULT hr = connection->GetDataItem(connection_data);
		if (hr == S_OK)
		{
			return S_OK;
		}

		CComObject<bh_agent_remote_connection_data> *com_object = nullptr;
		hr =
			CComObject<bh_agent_remote_connection_data>::CreateInstance(&com_object);
		if (FAILED(hr) || !com_object)
		{
			return FAILED(hr) ? hr : E_FAIL;
		}

		CComPtr<bh_agent_remote_connection_data> created_instance(com_object);
		hr = connection->SetDataItem(DkmDataCreationDisposition::CreateNew,
								created_instance);
		if (FAILED(hr))
		{
			return hr;
		}

		*connection_data = created_instance.Detach();
		return S_OK;
	}

  protected:
	HRESULT _InternalQueryInterface(REFIID riid, void **ppvObject)
	{
		if (!ppvObject)
		{
			return E_POINTER;
		}

		if (riid == __uuidof(IUnknown))
		{
			*ppvObject = static_cast<IUnknown *>(this);
			AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}
};

bool bh_get_tracked_target_pid_for_process(DkmProcess *process,
									 DWORD &tracked_pid);

bool bh_get_connection_data_from_any_process(
	CComPtr<bh_agent_remote_connection_data> &connection_data)
{
	connection_data.Release();

	CAutoDkmArray<DkmProcess *> processes;
	if (FAILED(DkmProcess::GetProcesses(&processes)))
	{
		return false;
	}

	for (UINT32 i = 0; i < processes.Length; ++i)
	{
		DkmProcess *candidate = processes.Members[i];
		if (!candidate)
		{
			continue;
		}

		DefaultPort::DkmTransportConnection *connection =
			candidate->Connection();
		if (!connection)
		{
			continue;
		}

		if (SUCCEEDED(bh_agent_remote_connection_data::GetInstance(
				connection, &connection_data)) &&
			connection_data)
		{
			return true;
		}
	}

	return false;
}

bool bh_get_active_process_data(
	CComPtr<DkmProcess> &process,
	CComPtr<bh_agent_remote_process_data> &process_data)
{
	process.Release();
	process_data.Release();

	CComPtr<bh_agent_remote_connection_data> connection_data;
	if (!bh_get_connection_data_from_any_process(connection_data) ||
		!connection_data)
	{
		return false;
	}

	GUID process_unique_id = {};
	bool target_pid_selected = false;
	DWORD target_pid = 0;
	bool has_active = false;
	AcquireSRWLockShared(&connection_data->state_lock);
	has_active = connection_data->has_active_process;
	process_unique_id = connection_data->active_process_unique_id;
	target_pid_selected = connection_data->target_pid_selected;
	target_pid = connection_data->target_pid;
	ReleaseSRWLockShared(&connection_data->state_lock);

	if (target_pid_selected)
	{
		CAutoDkmArray<DkmProcess *> processes;
		if (FAILED(DkmProcess::GetProcesses(&processes)))
		{
			return false;
		}

		for (UINT32 i = 0; i < processes.Length; ++i)
		{
			DkmProcess *candidate = processes.Members[i];
			if (!candidate)
			{
				continue;
			}

			const DkmProcess::Live *live = candidate->LivePart();
			if (!live || live->Id != target_pid)
			{
				continue;
			}

			process = candidate;
			if (FAILED(bh_agent_remote_process_data::GetInstance(
				process,
				&process_data)) ||
				!process_data)
			{
				return false;
			}

			AcquireSRWLockExclusive(&connection_data->state_lock);
			connection_data->active_process_unique_id = process->UniqueId();
			connection_data->has_active_process = true;
			const DkmProcess::Live *selected_live = process->LivePart();
			if (selected_live && selected_live->Id != 0)
			{
				connection_data->tracked_target_pid_valid = true;
				connection_data->tracked_target_pid = selected_live->Id;
			}
			ReleaseSRWLockExclusive(&connection_data->state_lock);
			return true;
		}

		return false;
	}

	if (!has_active)
	{
		return false;
	}

	if (FAILED(DkmProcess::FindProcess(process_unique_id, &process)) ||
		!process)
	{
		return false;
	}

	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return false;
	}

	return true;
}

bool bh_refresh_target_module_for_process(
	DkmProcess *process, bh_agent_remote_process_data *process_data)
{
	if (!process || !process_data)
	{
		return false;
	}

	CComPtr<DkmNativeRuntimeInstance> runtime_instance;
	if (FAILED(process->GetNativeRuntimeInstance(&runtime_instance)) ||
		!runtime_instance)
	{
		return false;
	}

	CAutoDkmArray<DkmModuleInstance *> modules;
	if (FAILED(runtime_instance->GetModuleInstances(&modules)) ||
		modules.Length == 0)
	{
		return false;
	}

	DkmNativeModuleInstance *selected_module = nullptr;
	for (UINT32 i = 0; i < modules.Length; ++i)
	{
		DkmModuleInstance *module_instance = modules.Members[i];
		if (!module_instance)
		{
			continue;
		}

		DkmNativeModuleInstance *native_module =
			DkmNativeModuleInstance::TryCast(module_instance);
		if (!native_module)
		{
			continue;
		}

		DkmString *module_name = module_instance->Name();
		const wchar_t *module_text = DkmString::Value(module_name);
		const wchar_t *extension =
			module_text ? wcsrchr(module_text, L'.') : nullptr;
		if (!extension || _wcsicmp(extension, L".exe") != 0)
		{
			continue;
		}

		selected_module = native_module;
		break;
	}

	if (!selected_module)
	{
		return false;
	}

	const DkmProcess::Live *live = process->LivePart();
	if (!live)
	{
		return false;
	}

	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	process_data->breakpoint_state.process_id = live->Id;
	process_data->breakpoint_state.process_unique_id = process->UniqueId();
	process_data->breakpoint_state.has_target = true;
	process_data->breakpoint_state.runtime_instance = runtime_instance;
	process_data->breakpoint_state.module_instance = selected_module;
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	return true;
}

void bh_request_async_break(DkmProcess *process, const char *reason)
{
	if (!process)
	{
		return;
	}

	HRESULT hr = process->AsyncBreak(false);
	if (FAILED(hr))
	{
		bh_agent_logf("engine.remote", "AsyncBreak failed hr=0x%08X (%s)",
					  (unsigned)hr, reason ? reason : "no reason");
	}
	else
	{
		bh_agent_logf("engine.remote", "AsyncBreak requested (%s)",
					  reason ? reason : "no reason");
	}
}

bool bh_get_thread_instruction_pointer(DWORD process_id, DWORD thread_id,
								 UINT64 &instruction_pointer,
								 std::string &error_text)
{
	instruction_pointer = 0;
	error_text.clear();

	if (process_id == 0 || thread_id == 0)
	{
		error_text = "invalid pid/tid";
		return false;
	}

	HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
					   THREAD_SUSPEND_RESUME,
					   FALSE, thread_id);
	if (!thread)
	{
		error_text = "OpenThread failed";
		return false;
	}

	bool did_suspend = false;
	const bool is_current_process = (process_id == GetCurrentProcessId());

#if defined(_M_X64)
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id);
	BOOL is_wow64 = FALSE;
	if (process)
	{
		IsWow64Process(process, &is_wow64);
		CloseHandle(process);
	}

	if (is_wow64)
	{
		WOW64_CONTEXT wow64_ctx = {};
		wow64_ctx.ContextFlags = WOW64_CONTEXT_CONTROL;

		BOOL got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
		if (!got_ctx)
		{
			if (SuspendThread(thread) != (DWORD)-1)
			{
				did_suspend = true;
				got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
			}
		}

		if (!got_ctx)
		{
			if (did_suspend)
			{
				ResumeThread(thread);
			}
			CloseHandle(thread);
			error_text = "Wow64GetThreadContext failed";
			return false;
		}

		if (did_suspend)
		{
			ResumeThread(thread);
		}

		CloseHandle(thread);
		instruction_pointer = (UINT64)wow64_ctx.Eip;
		return true;
	}
#endif

	CONTEXT context = {};
	context.ContextFlags = CONTEXT_CONTROL;

	BOOL got_context = GetThreadContext(thread, &context);
	if (!got_context)
	{
		if (!is_current_process && SuspendThread(thread) != (DWORD)-1)
		{
			did_suspend = true;
			got_context = GetThreadContext(thread, &context);
		}
	}

	if (!got_context)
	{
		if (did_suspend)
		{
			ResumeThread(thread);
		}
		CloseHandle(thread);
		error_text = "GetThreadContext failed";
		return false;
	}

	if (did_suspend)
	{
		ResumeThread(thread);
	}

	CloseHandle(thread);

#if defined(_M_X64)
	instruction_pointer = (UINT64)context.Rip;
#else
	instruction_pointer = (UINT64)context.Eip;
#endif

	return true;
}

void bh_set_break_session(DkmProcess *process, DkmThread *thread,
						 UINT64 instruction_pointer, UINT64 trap_address,
						 bool has_exception,
						 bh_break_session_state::reason_type reason)
{
	if (!process || !thread)
	{
		return;
	}

	DWORD process_id = 0;
	const DkmProcess::Live *live = process->LivePart();
	if (live)
	{
		process_id = live->Id;
	}

	DWORD tracked_target_pid = 0;
	if (process_id != 0 &&
		bh_get_tracked_target_pid_for_process(process, tracked_target_pid) &&
		tracked_target_pid != 0 && process_id != tracked_target_pid)
	{
		return;
	}

	UINT32 thread_id = 0;
	const DkmThread::System *system = thread->SystemPart();
	if (system)
	{
		thread_id = system->Id;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return;
	}

	AcquireSRWLockExclusive(&process_data->trap_session_lock);
	++process_data->break_session_generation;
	process_data->trap_session.active = true;
	process_data->trap_session.process_id = process_id;
	process_data->trap_session.process_unique_id = process->UniqueId();
	process_data->trap_session.thread_id = thread_id;
	process_data->trap_session.instruction_pointer = instruction_pointer;
	process_data->trap_session.trap_address = trap_address;
	process_data->trap_session.generation = process_data->break_session_generation;
	process_data->trap_session.has_exception = has_exception;
	process_data->trap_session.reason = reason;
	ReleaseSRWLockExclusive(&process_data->trap_session_lock);
}

void bh_set_break_session_from_thread(
	DkmThread *thread,
	bool has_exception,
	bh_break_session_state::reason_type reason)
{
	if (!thread)
	{
		return;
	}

	DkmProcess *process = thread->Process();
	if (!process)
	{
		return;
	}

	DWORD process_id = 0;
	const DkmProcess::Live *live = process->LivePart();
	if (live)
	{
		process_id = live->Id;
	}

	UINT32 thread_id = 0;
	const DkmThread::System *system = thread->SystemPart();
	if (system)
	{
		thread_id = system->Id;
	}

	UINT64 instruction_pointer = 0;
	std::string error_text;
	(void)bh_get_thread_instruction_pointer(process_id, thread_id,
							instruction_pointer,
							error_text);

	bh_set_break_session(process, thread, instruction_pointer, instruction_pointer,
				   has_exception, reason);
}

bool bh_capture_break_session_from_process(
	DkmProcess *process,
	bh_break_session_state::reason_type reason)
{
	if (!process)
	{
		return false;
	}

	CAutoDkmArray<DkmThread *> threads;
	if (FAILED(process->GetThreads(&threads)))
	{
		return false;
	}

	for (UINT32 i = 0; i < threads.Length; ++i)
	{
		DkmThread *thread = threads.Members[i];
		if (!thread)
		{
			continue;
		}

		bh_set_break_session_from_thread(thread, false, reason);
		return true;
	}

	return false;
}

void bh_clear_break_session()
{
	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(process, process_data) || !process_data)
	{
		return;
	}

	AcquireSRWLockExclusive(&process_data->trap_session_lock);
	process_data->trap_session = {};
	ReleaseSRWLockExclusive(&process_data->trap_session_lock);
}

void bh_clear_break_session_for_process(DkmProcess *process)
{
	if (!process)
	{
		return;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return;
	}

	AcquireSRWLockExclusive(&process_data->trap_session_lock);
	if (process_data->trap_session.active)
	{
		process_data->trap_session = {};
		bh_agent_logf("engine.remote",
					  "OnProcessContinue: cleared trap session state");
	}
	ReleaseSRWLockExclusive(&process_data->trap_session_lock);
}

bool bh_get_process_data_for_context(
	DkmProcess *preferred_process,
	CComPtr<DkmProcess> &process,
	CComPtr<bh_agent_remote_process_data> &process_data)
{
	process.Release();
	process_data.Release();

	if (preferred_process)
	{
		CComPtr<bh_agent_remote_process_data> preferred_data;
		if (SUCCEEDED(bh_agent_remote_process_data::GetInstance(preferred_process, &preferred_data)) &&
			preferred_data)
		{
			process = preferred_process;
			process_data = preferred_data;
			return true;
		}
	}

	return bh_get_active_process_data(process, process_data);
}

bh_break_session_state bh_get_break_session_snapshot(DkmProcess *preferred_process)
{
	bh_break_session_state snapshot = {};

	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_process_data_for_context(preferred_process, process, process_data) || !process_data)
	{
		return snapshot;
	}

	AcquireSRWLockShared(&process_data->trap_session_lock);
	snapshot = process_data->trap_session;
	ReleaseSRWLockShared(&process_data->trap_session_lock);
	if (snapshot.active)
	{
		return snapshot;
	}

	if (preferred_process)
	{
		CComPtr<DkmProcess> fallback_process;
		CComPtr<bh_agent_remote_process_data> fallback_data;
		if (bh_get_active_process_data(fallback_process, fallback_data) && fallback_data)
		{
			AcquireSRWLockShared(&fallback_data->trap_session_lock);
			snapshot = fallback_data->trap_session;
			ReleaseSRWLockShared(&fallback_data->trap_session_lock);
		}
	}

	return snapshot;
}

bool bh_try_parse_u64(const char *text, UINT64 &value)
{
	if (!text)
	{
		return false;
	}

	while (*text == ' ' || *text == '\t')
	{
		++text;
	}

	if (!*text)
	{
		return false;
	}

	char *end_ptr = nullptr;
	unsigned __int64 parsed = _strtoui64(text, &end_ptr, 0);
	if (end_ptr == text)
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

	value = parsed;
	return true;
}

bool bh_process_is_devenv(DWORD process_id)
{
	if (process_id == 0)
	{
		return false;
	}

	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
					 process_id);
	if (!process)
	{
		return false;
	}

	wchar_t path[MAX_PATH] = {};
	DWORD path_len = (DWORD)_countof(path);
	BOOL ok = QueryFullProcessImageNameW(process, 0, path, &path_len);
	CloseHandle(process);
	if (!ok || path_len == 0)
	{
		return false;
	}

	const wchar_t *name = wcsrchr(path, L'\\');
	name = name ? (name + 1) : path;
	return _wcsicmp(name, L"devenv.exe") == 0;
}

bool bh_get_tracked_target_pid_for_process(DkmProcess *process,
									 DWORD &tracked_pid)
{
	tracked_pid = 0;
	if (!process)
	{
		return false;
	}

	DefaultPort::DkmTransportConnection *connection = process->Connection();
	if (!connection)
	{
		return false;
	}

	CComPtr<bh_agent_remote_connection_data> connection_data;
	if (FAILED(bh_agent_remote_connection_data::GetInstance(connection,
										&connection_data)) ||
		!connection_data)
	{
		return false;
	}

	AcquireSRWLockShared(&connection_data->state_lock);
	const bool has_tracked = connection_data->tracked_target_pid_valid;
	tracked_pid = connection_data->tracked_target_pid;
	ReleaseSRWLockShared(&connection_data->state_lock);

	return has_tracked && tracked_pid != 0;
}

std::string bh_set_trap_at_address(UINT64 absolute_address);
std::string bh_set_trap_by_symbol(const char *symbol_name);
std::string bh_continue_process(DkmProcess *process);
void bh_clear_breakpoint_if_set();

const char *bh_pending_action_type_text(bh_pending_action::action_type type)
{
	switch (type)
	{
	case bh_pending_action::action_type::set_trap_symbol:
		return "set_trap_symbol";
	case bh_pending_action::action_type::set_trap_address:
		return "set_trap_address";
	case bh_pending_action::action_type::continue_exec:
		return "continue";
	default:
		return "unknown";
	}
}

bool bh_get_connection_data_for_active_process(
	CComPtr<bh_agent_remote_connection_data> &connection_data)
{
	connection_data.Release();

	CComPtr<DkmProcess> active_process;
	CComPtr<bh_agent_remote_process_data> active_process_data;
	if (!bh_get_active_process_data(active_process, active_process_data) ||
		!active_process)
	{
		return false;
	}

	DefaultPort::DkmTransportConnection *connection = active_process->Connection();
	if (!connection)
	{
		return false;
	}

	if (FAILED(bh_agent_remote_connection_data::GetInstance(connection,
													&connection_data)) ||
		!connection_data)
	{
		return false;
	}

	return true;
}

bool bh_consume_pending_exception_for_process(
	DkmProcess *preferred_process,
	bh_pending_exception_state &exception_state)
{
	exception_state = {};
	if (!preferred_process)
	{
		return false;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(bh_agent_remote_process_data::GetInstance(preferred_process,
									 &process_data)) ||
		!process_data)
	{
		return false;
	}

	AcquireSRWLockExclusive(&process_data->exception_lock);
	if (!process_data->pending_exception.pending)
	{
		ReleaseSRWLockExclusive(&process_data->exception_lock);
		return false;
	}

	exception_state = process_data->pending_exception;
	process_data->pending_exception = {};
	ReleaseSRWLockExclusive(&process_data->exception_lock);
	return true;
}

bool bh_get_pending_exception_for_process(
	DkmProcess *preferred_process,
	bh_pending_exception_state &exception_state)
{
	exception_state = {};
	if (!preferred_process)
	{
		return false;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(bh_agent_remote_process_data::GetInstance(preferred_process,
									 &process_data)) ||
		!process_data)
	{
		return false;
	}

	AcquireSRWLockShared(&process_data->exception_lock);
	const bool has_pending = process_data->pending_exception.pending;
	if (has_pending)
	{
		exception_state = process_data->pending_exception;
	}
	ReleaseSRWLockShared(&process_data->exception_lock);
	if (has_pending)
	{
		return true;
	}

	return false;
}

UINT64 bh_enqueue_pending_action(
	bh_agent_remote_connection_data *connection_data,
	const bh_pending_action &action_template)
{
	if (!connection_data)
	{
		return 0;
	}

	bh_pending_action action = action_template;
	AcquireSRWLockExclusive(&connection_data->pending_lock);
	action.id = connection_data->next_action_id++;
	connection_data->pending_actions.push_back(action);
	ReleaseSRWLockExclusive(&connection_data->pending_lock);
	return action.id;
}

std::string bh_pending_queue_status_text(
	bh_agent_remote_connection_data *connection_data)
{
	if (!connection_data)
	{
		return "ok: pending queue empty";
	}

	std::string status = "ok: pending queue ";
	AcquireSRWLockShared(&connection_data->pending_lock);
	if (connection_data->pending_actions.empty())
	{
		ReleaseSRWLockShared(&connection_data->pending_lock);
		status += "empty";
		return status;
	}

	char count_text[32] = {};
	_snprintf_s(count_text, sizeof(count_text), _TRUNCATE, "count=%u",
					(unsigned)connection_data->pending_actions.size());
	status += count_text;

	const size_t max_preview = 4;
	for (size_t i = 0;
		 i < connection_data->pending_actions.size() && i < max_preview; ++i)
	{
		const bh_pending_action &action = connection_data->pending_actions[i];
		char item[256] = {};
		if (action.type == bh_pending_action::action_type::set_trap_symbol)
		{
			_snprintf_s(item, sizeof(item), _TRUNCATE,
						" | #%llu %s %s (attempts=%u)",
						(unsigned long long)action.id,
						bh_pending_action_type_text(action.type),
						action.symbol_name.c_str(),
						(unsigned)action.attempts);
		}
		else if (action.type == bh_pending_action::action_type::set_trap_address)
		{
			_snprintf_s(item, sizeof(item), _TRUNCATE,
						" | #%llu %s 0x%llX (attempts=%u)",
						(unsigned long long)action.id,
						bh_pending_action_type_text(action.type),
						(unsigned long long)action.address,
						(unsigned)action.attempts);
		}
		else
		{
			_snprintf_s(item, sizeof(item), _TRUNCATE,
						" | #%llu %s (attempts=%u)",
						(unsigned long long)action.id,
						bh_pending_action_type_text(action.type),
						(unsigned)action.attempts);
		}
		status += item;
	}

	ReleaseSRWLockShared(&connection_data->pending_lock);
	return status;
}

std::string bh_try_apply_pending_actions(
	bh_agent_remote_connection_data *connection_data)
{
	if (!connection_data)
	{
		return "error: no connection data";
	}

	UINT32 applied_count = 0;
	for (UINT32 iteration = 0; iteration < 32; ++iteration)
	{
		bh_pending_action action = {};
		bool has_action = false;

		AcquireSRWLockShared(&connection_data->pending_lock);
		if (!connection_data->pending_actions.empty())
		{
			action = connection_data->pending_actions.front();
			has_action = true;
		}
		ReleaseSRWLockShared(&connection_data->pending_lock);

		if (!has_action)
		{
			break;
		}

		std::string result;
		if (action.type == bh_pending_action::action_type::set_trap_symbol)
		{
			bh_clear_breakpoint_if_set();
			result = bh_set_trap_by_symbol(action.symbol_name.c_str());
		}
		else if (action.type == bh_pending_action::action_type::set_trap_address)
		{
			bh_clear_breakpoint_if_set();
			result = bh_set_trap_at_address(action.address);
		}
		else
		{
			CComPtr<DkmProcess> process;
			CComPtr<bh_agent_remote_process_data> process_data;
			if (!bh_get_active_process_data(process, process_data) || !process)
			{
				result = "error: continue failed (target process not available)";
			}
			else
			{
				result = bh_continue_process(process);
				if (result.rfind("ok:", 0) == 0)
				{
					bh_clear_break_session();
				}
			}
		}

		bool success = (result.rfind("ok:", 0) == 0);
		bool found = false;
		AcquireSRWLockExclusive(&connection_data->pending_lock);
		for (size_t i = 0; i < connection_data->pending_actions.size(); ++i)
		{
			bh_pending_action &existing = connection_data->pending_actions[i];
			if (existing.id != action.id)
			{
				continue;
			}

			found = true;
			if (success)
			{
				connection_data->pending_actions.erase(
					connection_data->pending_actions.begin() + i);
			}
			else
			{
				existing.attempts += 1;
			}
			break;
		}
		ReleaseSRWLockExclusive(&connection_data->pending_lock);

		if (success)
		{
			applied_count += 1;
			continue;
		}

		if (found)
		{
			char fail_msg[320] = {};
			_snprintf_s(fail_msg, sizeof(fail_msg), _TRUNCATE,
						"pending action #%llu %s not ready: %s",
						(unsigned long long)action.id,
						bh_pending_action_type_text(action.type), result.c_str());
			return fail_msg;
		}
	}

	char ok_msg[96] = {};
	_snprintf_s(ok_msg, sizeof(ok_msg), _TRUNCATE,
				"ok: applied %u pending action(s)", (unsigned)applied_count);
	return ok_msg;
}

std::string bh_resolve_symbol_address(const char *symbol_name,
									  UINT64 &absolute_address)
{
	absolute_address = 0;

	if (!symbol_name || !symbol_name[0])
	{
		return "error: missing symbol name";
	}

	const bool module_qualified = strchr(symbol_name, '!') != nullptr;

	bh_remote_breakpoint_state snapshot = {};
	CComPtr<DkmProcess> active_process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(active_process, process_data) ||
		!process_data)
	{
		return "error: no active process data available";
	}

	AcquireSRWLockShared(&process_data->breakpoint_lock);
	snapshot = process_data->breakpoint_state;
	ReleaseSRWLockShared(&process_data->breakpoint_lock);

	CComPtr<DkmProcess> process;
	HRESULT hr = DkmProcess::FindProcess(snapshot.process_unique_id, &process);
	if (FAILED(hr) || !process)
	{
		CComPtr<DkmProcess> active_process;
		CComPtr<bh_agent_remote_process_data> active_process_data;
		if (!bh_get_active_process_data(active_process, active_process_data) ||
			!active_process)
		{
			return "error: could not resolve process for symbol trap";
		}

		process = active_process;
	}

	int symbol_wlen =
		MultiByteToWideChar(CP_UTF8, 0, symbol_name, -1, nullptr, 0);
	if (symbol_wlen <= 0)
	{
		return "error: symbol conversion failed";
	}

	std::wstring symbol_w;
	symbol_w.resize((size_t)symbol_wlen);
	if (MultiByteToWideChar(CP_UTF8, 0, symbol_name, -1, &symbol_w[0],
							symbol_wlen) <= 0)
	{
		return "error: symbol conversion failed";
	}

	// Without a cached module the name stays empty and the worker searches every loaded native module.
	std::wstring module_w;
	if (snapshot.module_instance)
	{
		DkmString *cached_module_name = snapshot.module_instance->Name();
		const wchar_t *cached_module_text = DkmString::Value(cached_module_name);
		if (cached_module_text && cached_module_text[0])
		{
			module_w = cached_module_text;
		}
	}

	CComPtr<DkmVariant> param_symbol;
	CComPtr<DkmVariant> param_module;
	hr = DkmVariant::Create(symbol_w.c_str(), &param_symbol);
	if (FAILED(hr) || !param_symbol)
	{
		return "error: failed to create symbol variant";
	}

	hr = DkmVariant::Create(module_w.c_str(), &param_module);
	if (FAILED(hr) || !param_module)
	{
		return "error: failed to create module variant";
	}

	CComPtr<DkmCustomMessage> request;
	hr = DkmCustomMessage::Create(
		process->Connection(), process, bh_guid_agent_message_to_local,
		bh_message_to_local_resolve_symbol, param_symbol, param_module, &request);
	if (FAILED(hr) || !request)
	{
		bh_agent_logf("engine.remote",
					  "set trap symbol: create request failed hr=0x%08X",
					  (unsigned)hr);
		return "error: failed to create local resolve request";
	}

	CComPtr<DkmCustomMessage> reply;
	hr = request->SendHigher(&reply);
	if (FAILED(hr) || !reply)
	{
		bh_agent_logf("engine.remote",
					  "set trap symbol: SendHigher failed hr=0x%08X", (unsigned)hr);

		CComPtr<DefaultPort::DkmWorkerProcessConnection> worker_connection;
		HRESULT worker_hr =
			DefaultPort::DkmWorkerProcessConnection::GetLocalSymbolsConnection(
				&worker_connection);
		if (SUCCEEDED(worker_hr) && worker_connection)
		{
			CComPtr<DkmCustomMessage> worker_request;
			worker_hr = DkmCustomMessage::Create(
				process->Connection(), process, bh_guid_agent_message_to_local_worker,
				bh_message_to_local_worker_resolve_symbol, param_symbol, param_module,
				nullptr, worker_connection, &worker_request);

			if (SUCCEEDED(worker_hr) && worker_request)
			{
				CComPtr<DkmCustomMessage> worker_reply;
				worker_hr = worker_request->SendLower(&worker_reply);
				if (SUCCEEDED(worker_hr) && worker_reply)
				{
					reply = worker_reply;
					bh_agent_logf(
						"engine.remote",
						"set trap symbol: resolved via direct local worker path");
				}
				else
				{
					bh_agent_logf(
						"engine.remote",
						"set trap symbol: direct worker SendLower failed hr=0x%08X",
						(unsigned)worker_hr);
				}
			}
		}

		if (!reply)
		{
			if (snapshot.module_instance && !module_qualified)
			{
				CComPtr<DkmString> symbol_fallback;
				if (SUCCEEDED(DkmString::Create(symbol_w.c_str(), &symbol_fallback)) &&
					symbol_fallback)
				{
					CComPtr<DkmNativeInstructionAddress> fallback_address;
					if (SUCCEEDED(snapshot.module_instance->FindExportName(
							symbol_fallback, false, &fallback_address)) &&
						fallback_address)
					{
						UINT64 absolute = snapshot.module_instance->BaseAddress() +
										  fallback_address->RVA();
						auto cpu = fallback_address->CPUInstructionPart();
						if (cpu)
						{
							absolute = cpu->InstructionPointer;
						}

						bh_agent_logf("engine.remote",
									  "resolve symbol: fallback resolved symbol locally");
						absolute_address = absolute;
						return "ok";
					}
				}
			}

			return "error: local worker failed to resolve symbol";
		}
	}

	DkmVariant *address_variant = reply->Parameter1();
	if (!address_variant)
	{
		return "error: symbol resolution reply missing address";
	}

	const VARIANT &value = address_variant->Value();
	if (value.vt != VT_BSTR || !value.bstrVal)
	{
		return "error: symbol resolution reply invalid type";
	}

	int address_utf8_len = WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1,
											   nullptr, 0, nullptr, nullptr);
	if (address_utf8_len <= 0)
	{
		return "error: symbol resolution address conversion failed";
	}

	std::string address_text;
	address_text.resize((size_t)address_utf8_len);
	if (WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, -1, &address_text[0],
							address_utf8_len, nullptr, nullptr) <= 0)
	{
		return "error: symbol resolution address conversion failed";
	}
	address_text.resize(strlen(address_text.c_str()));

	if (address_text.rfind("error:", 0) == 0)
	{
		return address_text;
	}

	if (!bh_try_parse_u64(address_text.c_str(), absolute_address))
	{
		return "error: symbol resolution address parse failed";
	}

	return "ok";
}

std::string bh_set_trap_by_symbol(const char *symbol_name)
{
	if (!symbol_name || !symbol_name[0])
	{
		return "error: missing symbol name";
	}

	UINT64 absolute_address = 0;
	std::string resolve_result =
		bh_resolve_symbol_address(symbol_name, absolute_address);
	if (resolve_result != "ok")
	{
		return resolve_result;
	}

	std::string result = bh_set_trap_at_address(absolute_address);
	if (result.rfind("ok: trap added", 0) == 0)
	{
		CComPtr<DkmProcess> process;
		CComPtr<bh_agent_remote_process_data> process_data;
		if (bh_get_active_process_data(process, process_data) && process_data)
		{
			AcquireSRWLockExclusive(&process_data->breakpoint_lock);
			if (!process_data->breakpoint_state.traps.empty())
			{
				process_data->breakpoint_state.traps.back().symbol_name = symbol_name;
			}
			ReleaseSRWLockExclusive(&process_data->breakpoint_lock);
		}

		result += " symbol=";
		result += symbol_name;
	}
	return result;
}

bool bh_resolve_address_symbol_text(DkmProcess *process,
								 const wchar_t *module_name,
								 UINT64 absolute_address,
								 std::string &symbol_text)
{
	symbol_text.clear();
	if (!process || !module_name || !module_name[0] || absolute_address == 0)
	{
		return false;
	}

	wchar_t address_w[64] = {};
	swprintf_s(address_w, L"0x%llX", (unsigned long long)absolute_address);

	CComPtr<DkmVariant> param_address;
	CComPtr<DkmVariant> param_module;
	HRESULT hr = DkmVariant::Create(address_w, &param_address);
	if (FAILED(hr) || !param_address)
	{
		return false;
	}

	hr = DkmVariant::Create(module_name, &param_module);
	if (FAILED(hr) || !param_module)
	{
		return false;
	}

	CComPtr<DkmCustomMessage> request;
	hr = DkmCustomMessage::Create(process->Connection(),
							 process,
							 bh_guid_agent_message_to_local,
							 bh_message_to_local_worker_resolve_address,
							 param_address,
							 param_module,
							 &request);
	if (FAILED(hr) || !request)
	{
		return false;
	}

	CComPtr<DkmCustomMessage> reply;
	hr = request->SendHigher(&reply);
	if (FAILED(hr) || !reply)
	{
		CComPtr<DefaultPort::DkmWorkerProcessConnection> worker_connection;
		HRESULT worker_hr =
			DefaultPort::DkmWorkerProcessConnection::GetLocalSymbolsConnection(
				&worker_connection);
		if (SUCCEEDED(worker_hr) && worker_connection)
		{
			CComPtr<DkmCustomMessage> worker_request;
			worker_hr = DkmCustomMessage::Create(
				process->Connection(),
				process,
				bh_guid_agent_message_to_local_worker,
				bh_message_to_local_worker_resolve_address,
				param_address,
				param_module,
				nullptr,
				worker_connection,
				&worker_request);
			if (SUCCEEDED(worker_hr) && worker_request)
			{
				CComPtr<DkmCustomMessage> worker_reply;
				worker_hr = worker_request->SendLower(&worker_reply);
				if (SUCCEEDED(worker_hr) && worker_reply)
				{
					reply = worker_reply;
				}
			}
		}

		if (!reply)
		{
			return false;
		}
	}

	DkmVariant *symbol_variant = reply->Parameter1();
	if (!symbol_variant)
	{
		return false;
	}

	const VARIANT &value = symbol_variant->Value();
	if (value.vt != VT_BSTR || !value.bstrVal)
	{
		return false;
	}

	int utf8_len = WideCharToMultiByte(CP_UTF8,
							   0,
							   value.bstrVal,
							   -1,
							   nullptr,
							   0,
							   nullptr,
							   nullptr);
	if (utf8_len <= 1)
	{
		return false;
	}

	symbol_text.resize((size_t)utf8_len);
	if (WideCharToMultiByte(CP_UTF8,
						0,
						value.bstrVal,
						-1,
						&symbol_text[0],
						utf8_len,
						nullptr,
						nullptr) <= 0)
	{
		symbol_text.clear();
		return false;
	}

	while (!symbol_text.empty() &&
			(symbol_text.back() == '\0' || symbol_text.back() == '\r' ||
			 symbol_text.back() == '\n'))
	{
		symbol_text.pop_back();
	}

	return !symbol_text.empty();
}

void bh_clear_breakpoint_if_set()
{
	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(process, process_data) || !process_data)
	{
		bh_agent_logf("engine.remote", "no active process data for clear breakpoint");
		return;
	}

	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	auto traps = process_data->breakpoint_state.traps;
	process_data->breakpoint_state.traps.clear();
	process_data->breakpoint_state.trap_enabled = false;
	process_data->breakpoint_state.trap_breakpoint = nullptr;
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	for (size_t i = 0; i < traps.size(); ++i)
	{
		if (!traps[i].breakpoint)
		{
			continue;
		}

		(void)traps[i].breakpoint->Disable();
		traps[i].breakpoint->Close();
	}

	bh_agent_logf("engine.remote", "cleared all traps count=%u", (unsigned)traps.size());
}

std::string bh_set_trap_at_address(UINT64 absolute_address)
{
	bh_remote_breakpoint_state snapshot = {};
	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(process, process_data) || !process_data)
	{
		bool target_pid_selected = false;
		DWORD target_pid = 0;
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (bh_get_connection_data_from_any_process(connection_data) &&
			connection_data)
		{
			AcquireSRWLockShared(&connection_data->state_lock);
			target_pid_selected = connection_data->target_pid_selected;
			target_pid = connection_data->target_pid;
			ReleaseSRWLockShared(&connection_data->state_lock);
		}

		if (!target_pid_selected)
		{
			bh_agent_logf("engine.remote",
						  "set trap failed: no active process data (set target pid <pid> first)");
			return "error: no active process data available (set target pid <pid> first)";
		}

		bh_agent_logf("engine.remote",
					  "set trap failed: target pid %u not active/available",
					  (unsigned)target_pid);
		return "error: target pid is set but process data is unavailable";
	}

	AcquireSRWLockShared(&process_data->breakpoint_lock);
	snapshot = process_data->breakpoint_state;
	ReleaseSRWLockShared(&process_data->breakpoint_lock);

	DWORD process_id = 0;
	const DkmProcess::Live *live = process ? process->LivePart() : nullptr;
	if (live)
	{
		process_id = live->Id;
	}

	char module_utf8[260] = {};
	if (snapshot.module_instance)
	{
		DkmString *module_name = snapshot.module_instance->Name();
		const wchar_t *module_text = DkmString::Value(module_name);
		if (module_text && module_text[0])
		{
			WideCharToMultiByte(CP_UTF8, 0, module_text, -1, module_utf8,
								(int)sizeof(module_utf8), nullptr, nullptr);
		}
	}

	bh_agent_logf("engine.remote",
				  "set trap create ctx pid=%u process=%p module=%s module_instance=%p runtime_instance=%p address=0x%llX",
				  (unsigned)process_id,
				  process.p,
				  module_utf8[0] ? module_utf8 : "<unknown>",
				  snapshot.module_instance.p,
				  snapshot.runtime_instance.p,
				  (unsigned long long)absolute_address);

	CComPtr<DkmNativeRuntimeInstance> runtime_instance;
	HRESULT hr = process->GetNativeRuntimeInstance(&runtime_instance);
	if (FAILED(hr) || !runtime_instance)
	{
		return "error: native runtime not ready for trap";
	}

	CAutoDkmArray<DkmModuleInstance *> modules;
	hr = runtime_instance->GetModuleInstances(&modules);
	if (FAILED(hr))
	{
		return "error: failed to enumerate native modules for trap";
	}

	CComPtr<DkmNativeModuleInstance> containing_module;
	for (UINT32 i = 0; i < modules.Length; ++i)
	{
		DkmNativeModuleInstance *native_module =
			DkmNativeModuleInstance::TryCast(modules.Members[i]);
		if (!native_module)
		{
			continue;
		}

		const UINT64 module_base = native_module->BaseAddress();
		const UINT64 module_size = native_module->Size();
		if (absolute_address >= module_base &&
			absolute_address - module_base < module_size)
		{
			containing_module = native_module;
			break;
		}
	}

	if (!containing_module)
	{
		return "error: trap address outside all loaded native modules";
	}

	CComPtr<DkmInstructionAddress> instruction_address;
	hr = process->CreateNativeInstructionAddress(absolute_address,
											 &instruction_address);
	if (FAILED(hr) || !instruction_address)
	{
		return "error: failed to create instruction address for trap";
	}

	CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint> new_trap;
	hr = Breakpoints::DkmRuntimeInstructionBreakpoint::Create(
		bh_guid_agent_trap_breakpoint, 
		nullptr, 
		instruction_address, 
		false,
		DkmDataItem::Null(), &new_trap);
	if (FAILED(hr) || !new_trap)
	{
		return "error: failed to create runtime trap";
	}

	hr = new_trap->Enable();
	if (FAILED(hr))
	{
		char err[96] = {};
		_snprintf_s(err, sizeof(err), _TRUNCATE,
					"error: failed to enable runtime trap hr=0x%08X", (unsigned)hr);
		return err;
	}

	UINT64 trap_id = 0;
	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	trap_id = process_data->breakpoint_state.next_trap_id++;
	bh_remote_breakpoint_state::trap_entry trap = {};
	trap.id = trap_id;
	trap.address = absolute_address;
	trap.breakpoint = new_trap;
	process_data->breakpoint_state.traps.push_back(trap);
	process_data->breakpoint_state.address = absolute_address;
	process_data->breakpoint_state.trap_enabled = true;
	process_data->breakpoint_state.trap_breakpoint = new_trap;
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	char msg[160] = {};
	_snprintf_s(msg, sizeof(msg), _TRUNCATE,
				"ok: trap added id=%llu at 0x%llX",
				(unsigned long long)trap_id,
				(unsigned long long)absolute_address);
	return msg;
}

std::string bh_remove_trap_by_id(UINT64 trap_id)
{
	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(process, process_data) || !process_data)
	{
		return "error: no active process data available";
	}

	CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint> trap_bp;
	UINT64 trap_address = 0;
	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	auto &traps = process_data->breakpoint_state.traps;
	for (auto it = traps.begin(); it != traps.end(); ++it)
	{
		if (it->id == trap_id)
		{
			trap_bp = it->breakpoint;
			trap_address = it->address;
			traps.erase(it);
			break;
		}
	}

	if (process_data->breakpoint_state.traps.empty())
	{
		process_data->breakpoint_state.trap_enabled = false;
		process_data->breakpoint_state.trap_breakpoint = nullptr;
	}
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	if (!trap_bp)
	{
		return "error: trap id not found";
	}

	(void)trap_bp->Disable();
	trap_bp->Close();

	char msg[160] = {};
	_snprintf_s(msg, sizeof(msg), _TRUNCATE,
				"ok: trap removed id=%llu at 0x%llX",
				(unsigned long long)trap_id,
				(unsigned long long)trap_address);
	return msg;
}

std::string bh_list_traps()
{
	CComPtr<DkmProcess> process;
	CComPtr<bh_agent_remote_process_data> process_data;
	if (!bh_get_active_process_data(process, process_data) || !process_data)
	{
		return "ok: traps empty";
	}

	std::vector<bh_remote_breakpoint_state::trap_entry> traps;
	AcquireSRWLockShared(&process_data->breakpoint_lock);
	traps = process_data->breakpoint_state.traps;
	ReleaseSRWLockShared(&process_data->breakpoint_lock);

	if (traps.empty())
	{
		return "ok: traps empty";
	}

	std::string response = "ok: traps count=" + std::to_string(traps.size());
	for (size_t i = 0; i < traps.size(); ++i)
	{
		char item[192] = {};
		_snprintf_s(item, sizeof(item), _TRUNCATE,
					" | #%u id=%llu addr=0x%llX",
					(unsigned)i,
					(unsigned long long)traps[i].id,
					(unsigned long long)traps[i].address);
		response += item;
		if (!traps[i].symbol_name.empty())
		{
			response += " symbol=";
			response += traps[i].symbol_name;
		}
	}

	return response;
}

std::string bh_continue_process(DkmProcess *process)
{
	if (!process)
	{
		return "error: no process for continue";
	}

	CAutoDkmArray<DkmThread *> threads;
	if (FAILED(process->GetThreads(&threads)))
	{
		bh_agent_logf("engine.remote", "continue failed: GetThreads");
		return "error: continue failed (GetThreads)";
	}

	UINT32 resumed = 0;
	for (UINT32 i = 0; i < threads.Length; ++i)
	{
		DkmThread *thread = threads.Members[i];
		if (!thread)
		{
			continue;
		}

		UINT32 external_count = 0;
		if (SUCCEEDED(thread->Resume(false, &external_count)))
		{
			++resumed;
		}
	}

	bh_agent_logf("engine.remote", "continue requested, resumed_threads=%u",
				  (unsigned)resumed);
	return "ok: continue requested";
}

enum class bh_memory_format
{
	hex,
	ascii,
	u32,
	u64,
	ptr
};

bool bh_try_parse_memory_format(const std::string &text,
								bh_memory_format &format)
{
	if (text.empty() || _stricmp(text.c_str(), "hex") == 0)
	{
		format = bh_memory_format::hex;
		return true;
	}
	if (_stricmp(text.c_str(), "ascii") == 0)
	{
		format = bh_memory_format::ascii;
		return true;
	}
	if (_stricmp(text.c_str(), "u32") == 0)
	{
		format = bh_memory_format::u32;
		return true;
	}
	if (_stricmp(text.c_str(), "u64") == 0)
	{
		format = bh_memory_format::u64;
		return true;
	}
	if (_stricmp(text.c_str(), "ptr") == 0)
	{
		format = bh_memory_format::ptr;
		return true;
	}
	return false;
}

std::string bh_read_memory_command(
	DWORD process_id,
	UINT64 address,
	UINT32 size,
	bh_memory_format format = bh_memory_format::hex)
{
	if (size == 0)
	{
		return "error: size must be > 0";
	}

	HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
								 FALSE, process_id);
	if (!process)
	{
		return "error: OpenProcess failed for read";
	}

	UINT32 element_width = 1;
	const char *format_name = "hex";
	switch (format)
	{
	case bh_memory_format::ascii:
		format_name = "ascii";
		break;
	case bh_memory_format::u32:
		element_width = 4;
		format_name = "u32";
		break;
	case bh_memory_format::u64:
		element_width = 8;
		format_name = "u64";
		break;
	case bh_memory_format::ptr:
#if defined(_M_X64)
	{
		BOOL is_wow64 = FALSE;
		if (!IsWow64Process(process, &is_wow64))
		{
			CloseHandle(process);
			return "error: failed to determine target pointer width";
		}
		element_width = is_wow64 ? 4U : 8U;
		format_name = is_wow64 ? "ptr32" : "ptr64";
		break;
	}
#else
		element_width = 4;
		format_name = "ptr32";
		break;
#endif
	case bh_memory_format::hex:
	default:
		break;
	}

	if ((format == bh_memory_format::u32 ||
		 format == bh_memory_format::u64 ||
		 format == bh_memory_format::ptr) &&
		(size % element_width) != 0)
	{
		CloseHandle(process);
		char error[128] = {};
		_snprintf_s(error, sizeof(error), _TRUNCATE,
					"error: read mem %s size must be a multiple of %u",
					format == bh_memory_format::ptr ? "ptr" : format_name,
					(unsigned)element_width);
		return error;
	}

	if (size > 256)
	{
		size = 256;
	}

	std::vector<BYTE> bytes(size);
	SIZE_T bytes_read = 0;
	BOOL ok = ReadProcessMemory(process, (LPCVOID)(UINT_PTR)address, bytes.data(),
								size, &bytes_read);
	CloseHandle(process);

	if (!ok || bytes_read == 0)
	{
		return "error: ReadProcessMemory failed";
	}

	if ((format == bh_memory_format::u32 ||
		 format == bh_memory_format::u64 ||
		 format == bh_memory_format::ptr) &&
		(bytes_read % element_width) != 0)
	{
		return "error: partial read is not aligned for typed format";
	}

	if (format == bh_memory_format::hex)
	{
		std::string hex;
		hex.reserve((size_t)bytes_read * 3);
		for (SIZE_T i = 0; i < bytes_read; ++i)
		{
			char pair[4] = {};
			_snprintf_s(pair, sizeof(pair), _TRUNCATE, "%02X", (unsigned)bytes[i]);
			if (!hex.empty())
			{
				hex.push_back(' ');
			}
			hex += pair;
		}

		char header[128] = {};
		_snprintf_s(header, sizeof(header), _TRUNCATE,
					"ok: mem 0x%llX (%u bytes): ", (unsigned long long)address,
					(unsigned)bytes_read);
		return std::string(header) + hex;
	}

	std::string formatted;
	if (format == bh_memory_format::ascii)
	{
		formatted.reserve((size_t)bytes_read);
		for (SIZE_T i = 0; i < bytes_read; ++i)
		{
			formatted.push_back(bytes[i] >= 0x20 && bytes[i] <= 0x7E
				? static_cast<char>(bytes[i])
				: '.');
		}
	}
	else
	{
		formatted.reserve((size_t)bytes_read * 3);
		for (SIZE_T offset = 0; offset < bytes_read; offset += element_width)
		{
			UINT64 value = 0;
			for (UINT32 byte_index = 0; byte_index < element_width; ++byte_index)
			{
				value |= static_cast<UINT64>(bytes[offset + byte_index]) <<
					(byte_index * 8);
			}

			char element[24] = {};
			if (element_width == 4)
			{
				_snprintf_s(element, sizeof(element), _TRUNCATE,
							"0x%08X", (unsigned)value);
			}
			else
			{
				_snprintf_s(element, sizeof(element), _TRUNCATE,
							"0x%016llX", (unsigned long long)value);
			}

			if (!formatted.empty())
			{
				formatted.push_back(' ');
			}
			formatted += element;
		}
	}

	char header[128] = {};
	_snprintf_s(header, sizeof(header), _TRUNCATE,
				"ok: mem 0x%llX (%u bytes) %s: ",
				(unsigned long long)address,
				(unsigned)bytes_read,
				format_name);
	return std::string(header) + formatted;
}

std::string bh_read_registers_command(DWORD process_id, DWORD thread_id)
{
	if (process_id == 0 || thread_id == 0)
	{
		return "error: invalid pid/tid for read regs";
	}

	HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
							   THREAD_SUSPEND_RESUME,
							   FALSE, thread_id);
	if (!thread)
	{
		return "error: OpenThread failed for read regs";
	}

	bool did_suspend = false;

#if defined(_M_X64)
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id);
	BOOL is_wow64 = FALSE;
	if (process)
	{
		IsWow64Process(process, &is_wow64);
		CloseHandle(process);
	}

	if (is_wow64)
	{
		WOW64_CONTEXT wow64_ctx = {};
		wow64_ctx.ContextFlags = WOW64_CONTEXT_INTEGER | WOW64_CONTEXT_CONTROL;

		BOOL got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
		if (!got_ctx)
		{
			if (SuspendThread(thread) != (DWORD)-1)
			{
				did_suspend = true;
				got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
			}
		}

		if (!got_ctx)
		{
			if (did_suspend)
			{
				ResumeThread(thread);
			}
			CloseHandle(thread);
			return "error: Wow64GetThreadContext failed";
		}

		if (did_suspend)
		{
			ResumeThread(thread);
		}

		CloseHandle(thread);

		char msg[512] = {};
		_snprintf_s(
			msg, sizeof(msg), _TRUNCATE,
			"ok: regs pid=%u tid=%u arch=x86 eip=0x%08X esp=0x%08X ebp=0x%08X eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X",
			(unsigned)process_id, (unsigned)thread_id, (unsigned)wow64_ctx.Eip,
			(unsigned)wow64_ctx.Esp, (unsigned)wow64_ctx.Ebp,
			(unsigned)wow64_ctx.Eax, (unsigned)wow64_ctx.Ebx,
			(unsigned)wow64_ctx.Ecx, (unsigned)wow64_ctx.Edx,
			(unsigned)wow64_ctx.Esi, (unsigned)wow64_ctx.Edi);
		return msg;
	}
#endif

	CONTEXT context = {};
	context.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;

	BOOL got_context = GetThreadContext(thread, &context);
	if (!got_context)
	{
		if (SuspendThread(thread) != (DWORD)-1)
		{
			did_suspend = true;
			got_context = GetThreadContext(thread, &context);
		}
	}

	if (!got_context)
	{
		if (did_suspend)
		{
			ResumeThread(thread);
		}
		CloseHandle(thread);
		return "error: GetThreadContext failed";
	}

	if (did_suspend)
	{
		ResumeThread(thread);
	}

	CloseHandle(thread);

	char response[1024] = {};
#if defined(_M_X64)
	_snprintf_s(
		response, sizeof(response), _TRUNCATE,
		"ok: regs pid=%u tid=%u arch=x64 rip=0x%llX rsp=0x%llX rbp=0x%llX rax=0x%llX rbx=0x%llX rcx=0x%llX rdx=0x%llX rsi=0x%llX rdi=0x%llX r8=0x%llX r9=0x%llX r10=0x%llX r11=0x%llX r12=0x%llX r13=0x%llX r14=0x%llX r15=0x%llX",
		(unsigned)process_id, (unsigned)thread_id,
		(unsigned long long)context.Rip, (unsigned long long)context.Rsp,
		(unsigned long long)context.Rbp, (unsigned long long)context.Rax,
		(unsigned long long)context.Rbx, (unsigned long long)context.Rcx,
		(unsigned long long)context.Rdx, (unsigned long long)context.Rsi,
		(unsigned long long)context.Rdi, (unsigned long long)context.R8,
		(unsigned long long)context.R9, (unsigned long long)context.R10,
		(unsigned long long)context.R11, (unsigned long long)context.R12,
		(unsigned long long)context.R13, (unsigned long long)context.R14,
		(unsigned long long)context.R15);
#else
	_snprintf_s(
		response, sizeof(response), _TRUNCATE,
		"ok: regs pid=%u tid=%u arch=x86 eip=0x%08X esp=0x%08X ebp=0x%08X eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X esi=0x%08X edi=0x%08X",
		(unsigned)process_id, (unsigned)thread_id, (unsigned)context.Eip,
		(unsigned)context.Esp, (unsigned)context.Ebp, (unsigned)context.Eax,
		(unsigned)context.Ebx, (unsigned)context.Ecx, (unsigned)context.Edx,
		(unsigned)context.Esi, (unsigned)context.Edi);
#endif

	return response;
}

bool bh_get_thread_stack_pointer(DWORD process_id, DWORD thread_id,
								 UINT64 &stack_pointer,
								 std::string &arch_text,
								 std::string &error_text)
{
	stack_pointer = 0;
	arch_text.clear();
	error_text.clear();

	if (process_id == 0 || thread_id == 0)
	{
		error_text = "error: invalid pid/tid for read stack";
		return false;
	}

	HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
							   THREAD_SUSPEND_RESUME,
							   FALSE, thread_id);
	if (!thread)
	{
		error_text = "error: OpenThread failed for read stack";
		return false;
	}

	bool did_suspend = false;
	const bool is_current_process = (process_id == GetCurrentProcessId());

#if defined(_M_X64)
	HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id);
	BOOL is_wow64 = FALSE;
	if (process)
	{
		IsWow64Process(process, &is_wow64);
		CloseHandle(process);
	}

	if (is_wow64)
	{
		WOW64_CONTEXT wow64_ctx = {};
		wow64_ctx.ContextFlags = WOW64_CONTEXT_CONTROL;

		BOOL got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
		if (!got_ctx)
		{
			if (SuspendThread(thread) != (DWORD)-1)
			{
				did_suspend = true;
				got_ctx = Wow64GetThreadContext(thread, &wow64_ctx);
			}
		}

		if (!got_ctx)
		{
			if (did_suspend)
			{
				ResumeThread(thread);
			}
			CloseHandle(thread);
			error_text = "error: Wow64GetThreadContext failed";
			return false;
		}

		if (did_suspend)
		{
			ResumeThread(thread);
		}

		CloseHandle(thread);
		stack_pointer = (UINT64)wow64_ctx.Esp;
		arch_text = "x86";
		return true;
	}
#endif

	CONTEXT context = {};
	context.ContextFlags = CONTEXT_CONTROL;

	BOOL got_context = GetThreadContext(thread, &context);
	if (!got_context)
	{
		if (!is_current_process && SuspendThread(thread) != (DWORD)-1)
		{
			did_suspend = true;
			got_context = GetThreadContext(thread, &context);
		}
	}

	if (!got_context)
	{
		if (did_suspend)
		{
			ResumeThread(thread);
		}
		CloseHandle(thread);
		error_text = "error: GetThreadContext failed";
		return false;
	}

	if (did_suspend)
	{
		ResumeThread(thread);
	}

	CloseHandle(thread);

#if defined(_M_X64)
	stack_pointer = (UINT64)context.Rsp;
	arch_text = "x64";
#else
	stack_pointer = (UINT64)context.Esp;
	arch_text = "x86";
#endif

	return true;
}

std::string bh_read_stack_command(DWORD process_id, DWORD thread_id, UINT32 size)
{
	if (size == 0)
	{
		size = 64;
	}

	if (size > 512)
	{
		size = 512;
	}

	UINT64 stack_pointer = 0;
	std::string arch_text;
	std::string error_text;
	if (!bh_get_thread_stack_pointer(process_id, thread_id, stack_pointer,
							 arch_text, error_text))
	{
		return error_text.empty() ? "error: failed to resolve stack pointer"
							 : error_text;
	}

	UINT64 instruction_pointer = 0;
	if (!bh_get_thread_instruction_pointer(process_id,
									thread_id,
									instruction_pointer,
									error_text))
	{
		return error_text.empty() ? "error: failed to resolve instruction pointer"
							 : error_text;
	}

	std::string mem_result = bh_read_memory_command(process_id, stack_pointer, size);
	if (mem_result.rfind("ok:", 0) != 0)
	{
		return mem_result;
	}

	char header[224] = {};
	_snprintf_s(header, sizeof(header), _TRUNCATE,
				"ok: stack pid=%u tid=%u arch=%s ip=0x%llX sp=0x%llX | ",
				(unsigned)process_id, (unsigned)thread_id, arch_text.c_str(),
				(unsigned long long)instruction_pointer,
				(unsigned long long)stack_pointer);
	return std::string(header) + mem_result.substr(4);
}

struct bh_thread_list_entry
{
	UINT32 thread_id = 0;
	UINT64 instruction_pointer = 0;
	UINT64 native_start_address = 0;
	bool is_main_thread = false;
	DkmThread *thread = nullptr;
};

std::string bh_get_thread_name(DkmNativeRuntimeInstance *runtime_instance,
							   DkmThread *thread)
{
	if (!runtime_instance || !thread)
	{
		return "<unnamed>";
	}

	CComPtr<DkmString> thread_name;
	if (FAILED(runtime_instance->GetThreadName(thread, &thread_name)) ||
		!thread_name)
	{
		return "<unnamed>";
	}

	const wchar_t *name_text = DkmString::Value(thread_name);
	if (!name_text || !name_text[0])
	{
		return "<unnamed>";
	}

	char name_utf8[512] = {};
	if (WideCharToMultiByte(CP_UTF8,
						0,
						name_text,
						-1,
						name_utf8,
						(int)sizeof(name_utf8),
						nullptr,
						nullptr) <= 0)
	{
		return "<unnamed>";
	}

	std::string result = name_utf8;
	for (char &ch : result)
	{
		if (ch == '|' || ch == '\r' || ch == '\n' || ch == '\t')
		{
			ch = ' ';
		}
	}
	return result;
}

std::string bh_list_threads_command(const bh_break_session_state &trap_session)
{
	CComPtr<DkmProcess> process;
	if (FAILED(DkmProcess::FindProcess(trap_session.process_unique_id, &process)) ||
		!process)
	{
		return "error: cannot resolve process for list threads";
	}

	CAutoDkmArray<DkmThread *> threads;
	if (FAILED(process->GetThreads(&threads)))
	{
		return "error: failed to enumerate threads";
	}

	std::vector<bh_thread_list_entry> entries;
	entries.reserve(threads.Length);
	for (UINT32 i = 0; i < threads.Length; ++i)
	{
		DkmThread *thread = threads.Members[i];
		if (!thread)
		{
			continue;
		}

		const DkmThread::System *system = thread->SystemPart();
		if (!system || system->Id == 0)
		{
			continue;
		}

		bh_thread_list_entry entry = {};
		entry.thread_id = system->Id;
		entry.native_start_address = thread->NativeStartAddress();
		entry.is_main_thread = thread->IsMainThread();
		entry.thread = thread;
		std::string ignored_error;
		(void)bh_get_thread_instruction_pointer(trap_session.process_id,
										   entry.thread_id,
										   entry.instruction_pointer,
										   ignored_error);
		entries.push_back(entry);
	}

	std::sort(entries.begin(), entries.end(),
			  [](const bh_thread_list_entry &left,
				 const bh_thread_list_entry &right)
			  { return left.thread_id < right.thread_id; });

	bool selected_found = false;
	for (const bh_thread_list_entry &entry : entries)
	{
		if (entry.thread_id == trap_session.thread_id)
		{
			selected_found = true;
			break;
		}
	}
	if (!selected_found)
	{
		return "error: selected thread is no longer available";
	}

	CComPtr<DkmNativeRuntimeInstance> runtime_instance;
	(void)process->GetNativeRuntimeInstance(&runtime_instance);

	char header[160] = {};
	_snprintf_s(header,
				  sizeof(header),
				  _TRUNCATE,
				  "threads: pid=%u count=%u selected=%u | num marker tid ip start main name",
				  (unsigned)trap_session.process_id,
				  (unsigned)entries.size(),
				  (unsigned)trap_session.thread_id);
	std::string response = header;

	for (size_t i = 0; i < entries.size(); ++i)
	{
		const bh_thread_list_entry &entry = entries[i];
		const std::string name = bh_get_thread_name(runtime_instance, entry.thread);
		char row[896] = {};
		_snprintf_s(row,
					  sizeof(row),
					  _TRUNCATE,
					  " | #%u %c tid=%u ip=0x%llX start=0x%llX main=%s name=%s",
					  (unsigned)i,
					  entry.thread_id == trap_session.thread_id ? '*' : '-',
					  (unsigned)entry.thread_id,
					  (unsigned long long)entry.instruction_pointer,
					  (unsigned long long)entry.native_start_address,
					  entry.is_main_thread ? "true" : "false",
					  name.c_str());
		response += row;
	}

	return response;
}

std::string bh_select_thread_command(const bh_break_session_state &trap_session,
									 UINT32 thread_id)
{
	CComPtr<DkmProcess> process;
	if (FAILED(DkmProcess::FindProcess(trap_session.process_unique_id, &process)) ||
		!process)
	{
		return "error: cannot resolve process for select thread";
	}

	CComPtr<DkmThread> selected_thread;
	if (FAILED(process->FindSystemThread(thread_id, &selected_thread)) ||
		!selected_thread)
	{
		return "error: thread id not found in selected process";
	}

	UINT64 instruction_pointer = 0;
	std::string error_text;
	if (!bh_get_thread_instruction_pointer(trap_session.process_id,
									 thread_id,
									 instruction_pointer,
									 error_text))
	{
		return "error: failed to read selected thread context: " + error_text;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return "error: failed to access selected process state";
	}

	AcquireSRWLockExclusive(&process_data->trap_session_lock);
	if (!process_data->trap_session.active ||
		process_data->trap_session.process_id != trap_session.process_id ||
		process_data->trap_session.generation != trap_session.generation)
	{
		ReleaseSRWLockExclusive(&process_data->trap_session_lock);
		return "error: break session changed during thread selection";
	}
	process_data->trap_session.thread_id = thread_id;
	process_data->trap_session.instruction_pointer = instruction_pointer;
	ReleaseSRWLockExclusive(&process_data->trap_session_lock);

	char response[128] = {};
	_snprintf_s(response,
				  sizeof(response),
				  _TRUNCATE,
				  "ok: selected thread tid=%u ip=0x%llX",
				  (unsigned)thread_id,
				  (unsigned long long)instruction_pointer);
	return response;
}

std::string bh_handle_script_command(DkmProcess *command_process, const char *message)
{
	if (!message || !message[0])
	{
		return "error: empty command";
	}

	bh_agent_logf("engine.remote", "script message: %s", message);

	if (_stricmp(message, "set bp") == 0 || _stricmp(message, "clear bp") == 0)
	{
		return "error: trap/breakpoint commands removed";
	}

	bool needs_process = false;
		enum class command_type
	{
		none,
		status,
		modules,
		set_target_pid,
		trap_add_symbol,
		trap_add_address,
		trap_list,
		trap_remove,
		trap_clear,
		queue_continue,
		queue_list,
		queue_clear,
		queue_run,
		trap_resume,
		capture_break,
		resolve_symbol,
		cont,
		async_break,
		read_mem,
		read_stack,
		read_regs,
		list_threads,
		select_thread,
		where
	};

	command_type command = command_type::none;
	if (_stricmp(message, "status") == 0)
	{
		command = command_type::status;
	}
	else if (_stricmp(message, "modules") == 0)
	{
		command = command_type::modules;
	}
	else if (_strnicmp(message, "set target pid ", 15) == 0)
	{
		command = command_type::set_target_pid;
	}
	else if (_strnicmp(message, "trap add symbol ", 16) == 0)
	{
		command = command_type::trap_add_symbol;
	}
	else if (_strnicmp(message, "set trap symbol ", 16) == 0)
	{
		command = command_type::trap_add_symbol;
	}
	else if (_strnicmp(message, "trap add ", 9) == 0)
	{
		command = command_type::trap_add_address;
	}
	else if (_strnicmp(message, "set trap ", 9) == 0)
	{
		command = command_type::trap_add_address;
	}
	else if (_stricmp(message, "trap list") == 0)
	{
		command = command_type::trap_list;
	}
	else if (_strnicmp(message, "trap remove ", 12) == 0)
	{
		command = command_type::trap_remove;
	}
	else if (_stricmp(message, "trap clear") == 0)
	{
		command = command_type::trap_clear;
	}
	else if (_stricmp(message, "queue continue") == 0)
	{
		command = command_type::queue_continue;
	}
	else if (_stricmp(message, "queue list") == 0)
	{
		command = command_type::queue_list;
	}
	else if (_stricmp(message, "queue clear") == 0)
	{
		command = command_type::queue_clear;
	}
	else if (_stricmp(message, "queue run") == 0)
	{
		command = command_type::queue_run;
	}
	else if (_stricmp(message, "trap resume") == 0)
	{
		command = command_type::trap_resume;
	}
	else if (_stricmp(message, "capture break") == 0)
	{
		command = command_type::capture_break;
		needs_process = true;
	}
	else if (_strnicmp(message, "resolve symbol ", 15) == 0)
	{
		command = command_type::resolve_symbol;
	}
	else if (_stricmp(message, "continue") == 0)
	{
		command = command_type::cont;
		needs_process = true;
	}
	else if (_stricmp(message, "async break") == 0 ||
			 _stricmp(message, "break") == 0)
	{
		command = command_type::async_break;
		needs_process = true;
	}
	else if (_stricmp(message, "read mem") == 0 ||
			 _strnicmp(message, "read mem ", 9) == 0)
	{
		command = command_type::read_mem;
	}
	else if (_stricmp(message, "read stack") == 0 ||
			 _strnicmp(message, "read stack ", 11) == 0 ||
			 _stricmp(message, "stack") == 0 ||
			 _strnicmp(message, "stack ", 6) == 0)
	{
		command = command_type::read_stack;
	}
	else if (_stricmp(message, "read regs") == 0)
	{
		command = command_type::read_regs;
	}
	else if (_stricmp(message, "list threads") == 0)
	{
		command = command_type::list_threads;
	}
	else if (_strnicmp(message, "select thread ", 14) == 0)
	{
		command = command_type::select_thread;
	}
	else if (_stricmp(message, "where") == 0 ||
			 _strnicmp(message, "where ", 6) == 0)
	{
		command = command_type::where;
	}
	else
	{
		return "error: unknown command";
	}

	bh_pending_exception_state pending_exception = {};
	const bool allow_with_pending_exception =
		command == command_type::status ||
		command == command_type::capture_break ||
		command == command_type::read_regs ||
		command == command_type::read_stack ||
		command == command_type::read_mem ||
		command == command_type::list_threads ||
		command == command_type::select_thread ||
		command == command_type::where;

	if (!allow_with_pending_exception &&
		bh_consume_pending_exception_for_process(command_process, pending_exception))
	{
		char msg[384] = {};
		_snprintf_s(
			msg,
			sizeof(msg),
			_TRUNCATE,
			"error: exception thrown pid=%u thread=0x%llX code=0x%08X stage=%u name=%s",
			(unsigned)pending_exception.process_id,
			(unsigned long long)pending_exception.thread_marker,
			(unsigned)pending_exception.code,
			(unsigned)pending_exception.stage,
			pending_exception.name.empty() ? "<none>" : pending_exception.name.c_str());
		return msg;
	}

	bh_break_session_state trap_session = bh_get_break_session_snapshot(command_process);

	bool requires_trap_session =
		command == command_type::read_regs || command == command_type::read_stack ||
		command == command_type::list_threads ||
		command == command_type::select_thread || command == command_type::where;

	if (requires_trap_session && !trap_session.active)
	{
		return "error: no active trap session (wait for trap hit)";
	}

	DkmProcess *process = nullptr;
	if (needs_process)
	{
		if (trap_session.active)
		{
			if (FAILED(DkmProcess::FindProcess(trap_session.process_unique_id,
											   &process)) ||
				!process)
			{
				bh_agent_logf("engine.remote",
							  "command failed: cannot resolve DkmProcess for pid=%u",
							  (unsigned)trap_session.process_id);
				return "error: cannot resolve DkmProcess";
			}
		}
		else
		{
			if (command != command_type::cont &&
				command != command_type::async_break &&
				command != command_type::capture_break)
			{
				return "error: command requires trap session";
			}

			bh_remote_breakpoint_state snapshot = {};
			CComPtr<DkmProcess> active_process;
			CComPtr<bh_agent_remote_process_data> process_data;
			if (bh_get_active_process_data(active_process, process_data) &&
				process_data)
			{
				AcquireSRWLockShared(&process_data->breakpoint_lock);
				snapshot = process_data->breakpoint_state;
				ReleaseSRWLockShared(&process_data->breakpoint_lock);
			}

			if (!active_process)
			{
				return "error: continue failed (target process not available)";
			}

			process = active_process;
		}
	}

	std::string response = "ok";
	switch (command)
	{
	case command_type::set_target_pid: {
		const char *payload = message + 15;
		UINT64 pid64 = 0;
		if (!bh_try_parse_u64(payload, pid64) || pid64 == 0 ||
			pid64 > 0xFFFFFFFFULL)
		{
			response = "error: set target pid parse failed";
			break;
		}

		CComPtr<DkmProcess> selected_process;
		CAutoDkmArray<DkmProcess *> processes;
		if (FAILED(DkmProcess::GetProcesses(&processes)))
		{
			response = "error: unable to enumerate debug processes";
			break;
		}

		for (UINT32 i = 0; i < processes.Length; ++i)
		{
			DkmProcess *candidate = processes.Members[i];
			if (!candidate)
			{
				continue;
			}

			const DkmProcess::Live *live = candidate->LivePart();
			if (live && live->Id == (DWORD)pid64)
			{
				selected_process = candidate;
				break;
			}
		}

		if (!selected_process)
		{
			response = "error: target pid not found in debug process list";
			break;
		}

		CComPtr<bh_agent_remote_process_data> process_data;
		if (FAILED(bh_agent_remote_process_data::GetInstance(selected_process,
															 &process_data)) ||
			!process_data)
		{
			response = "error: failed to initialize process data for pid";
			break;
		}

		if (!bh_refresh_target_module_for_process(selected_process, process_data))
		{
			response = "error: pid selected but target module is not loaded";
			break;
		}

		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (FAILED(bh_agent_remote_connection_data::GetInstance(
					selected_process->Connection(), &connection_data)) ||
			!connection_data)
		{
			response = "error: failed to initialize connection data for pid";
			break;
		}

		AcquireSRWLockExclusive(&connection_data->state_lock);
		connection_data->target_pid_selected = true;
		connection_data->target_pid = (DWORD)pid64;
		connection_data->tracked_target_pid_valid = true;
		connection_data->tracked_target_pid = (DWORD)pid64;
		connection_data->active_process_unique_id = selected_process->UniqueId();
		connection_data->has_active_process = true;
		ReleaseSRWLockExclusive(&connection_data->state_lock);

		char msg[96] = {};
		_snprintf_s(msg, sizeof(msg), _TRUNCATE, "ok: target pid set to %u",
					(unsigned)pid64);
		response = msg;
		break;
	}
	case command_type::trap_add_symbol: {
		const char *symbol_name = message + 16;
		while (*symbol_name == ' ' || *symbol_name == '\t')
		{
			++symbol_name;
		}

		if (!*symbol_name)
		{
			response = "error: missing symbol name";
			break;
		}

		response = bh_set_trap_by_symbol(symbol_name);
		break;
	}
	case command_type::trap_add_address: {
		const char *payload = message + 9;
		UINT64 trap_address = 0;
		if (!bh_try_parse_u64(payload, trap_address))
		{
			response = "error: trap add parse failed";
			break;
		}

		response = bh_set_trap_at_address(trap_address);
		break;
	}
	case command_type::trap_list:
		response = bh_list_traps();
		break;
	case command_type::trap_remove: {
		const char *payload = message + 12;
		UINT64 trap_id = 0;
		if (!bh_try_parse_u64(payload, trap_id) || trap_id == 0)
		{
			response = "error: trap remove parse failed";
			break;
		}

		response = bh_remove_trap_by_id(trap_id);
		break;
	}
	case command_type::trap_clear:
		bh_clear_breakpoint_if_set();
		response = "ok: traps cleared";
		break;
	case command_type::queue_continue: {
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (!bh_get_connection_data_for_active_process(connection_data) ||
			!connection_data)
		{
			response = "error: no active process for queue";
			break;
		}

		bh_pending_action action = {};
		action.type = bh_pending_action::action_type::continue_exec;
		UINT64 queued_id = bh_enqueue_pending_action(connection_data, action);
		char queued_msg[128] = {};
		_snprintf_s(queued_msg, sizeof(queued_msg), _TRUNCATE,
					"ok: queued pending action #%llu (continue)",
					(unsigned long long)queued_id);
		response = queued_msg;
		break;
	}
	case command_type::queue_list: {
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (!bh_get_connection_data_for_active_process(connection_data) ||
			!connection_data)
		{
			response = "ok: pending queue empty";
			break;
		}

		response = bh_pending_queue_status_text(connection_data);
		break;
	}
	case command_type::queue_clear: {
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (!bh_get_connection_data_for_active_process(connection_data) ||
			!connection_data)
		{
			response = "ok: pending queue cleared (0)";
			break;
		}

		UINT32 cleared = 0;
		AcquireSRWLockExclusive(&connection_data->pending_lock);
		cleared = (UINT32)connection_data->pending_actions.size();
		connection_data->pending_actions.clear();
		ReleaseSRWLockExclusive(&connection_data->pending_lock);

		char msg[80] = {};
		_snprintf_s(msg, sizeof(msg), _TRUNCATE,
					"ok: pending queue cleared (%u)", (unsigned)cleared);
		response = msg;
		break;
	}
	case command_type::queue_run: {
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (!bh_get_connection_data_for_active_process(connection_data) ||
			!connection_data)
		{
			response = "error: no active process for queue";
			break;
		}

		response = bh_try_apply_pending_actions(connection_data);
		break;
	}
	case command_type::status:
	{
		if (trap_session.active && trap_session.instruction_pointer == 0 &&
			trap_session.process_id != 0 && trap_session.thread_id != 0)
		{
			UINT64 instruction_pointer = 0;
			std::string error_text;
			if (bh_get_thread_instruction_pointer(trap_session.process_id,
								 trap_session.thread_id,
								 instruction_pointer,
								 error_text) &&
				instruction_pointer != 0)
			{
				trap_session.instruction_pointer = instruction_pointer;
				if (trap_session.trap_address == 0)
				{
					trap_session.trap_address = instruction_pointer;
				}
			}
		}

		bh_pending_exception_state pending_exception_status = {};
		const bool has_pending_exception =
			bh_get_pending_exception_for_process(command_process, pending_exception_status);
		const bool include_pending_exception =
			has_pending_exception && trap_session.active;

		if (trap_session.active)
		{
			const char *reason_text = bh_break_reason_to_text(trap_session.reason);
			char msg[420] = {};
			if (include_pending_exception)
			{
				_snprintf_s(
					msg,
					sizeof(msg),
					_TRUNCATE,
					"ok: paused break pid=%u tid=%u ip=0x%llX reason=%s | pendingException code=0x%08X stage=%u pid=%u thread=0x%llX name=%s",
					(unsigned)trap_session.process_id,
					(unsigned)trap_session.thread_id,
					(unsigned long long)trap_session.instruction_pointer,
					reason_text,
					(unsigned)pending_exception_status.code,
					(unsigned)pending_exception_status.stage,
					(unsigned)pending_exception_status.process_id,
					(unsigned long long)pending_exception_status.thread_marker,
					pending_exception_status.name.empty() ? "<none>" : pending_exception_status.name.c_str());
			}
			else
			{
				_snprintf_s(msg, sizeof(msg), _TRUNCATE,
							"ok: paused break pid=%u tid=%u ip=0x%llX reason=%s",
							(unsigned)trap_session.process_id,
							(unsigned)trap_session.thread_id,
							(unsigned long long)trap_session.instruction_pointer,
							reason_text);
			}
			response = msg;
		}
		else
		{
			bool target_pid_selected = false;
			DWORD target_pid = 0;
			CComPtr<bh_agent_remote_connection_data> connection_data;
			if (bh_get_connection_data_from_any_process(connection_data) &&
				connection_data)
			{
				AcquireSRWLockShared(&connection_data->state_lock);
				target_pid_selected = connection_data->target_pid_selected;
				target_pid = connection_data->target_pid;
				ReleaseSRWLockShared(&connection_data->state_lock);
			}

			if (target_pid_selected)
			{
				char msg[160] = {};
				_snprintf_s(msg, sizeof(msg), _TRUNCATE,
						"ok: running (no active break session, target pid=%u)",
						(unsigned)target_pid);
				response = msg;
			}
			else
			{
				response = "ok: running (no active break session)";
			}
		}
		break;
	}
	case command_type::modules:
	{
		CComPtr<DkmProcess> module_process;
		if (trap_session.active)
		{
			if (FAILED(DkmProcess::FindProcess(trap_session.process_unique_id,
								   &module_process)) ||
				!module_process)
			{
				response = "error: cannot resolve DkmProcess for modules";
				break;
			}
		}
		else
		{
			CComPtr<bh_agent_remote_process_data> process_data;
			if (!bh_get_active_process_data(module_process, process_data) ||
				!module_process)
			{
				response = "error: no active process for modules";
				break;
			}
		}

		CComPtr<DkmNativeRuntimeInstance> runtime_instance;
		if (FAILED(module_process->GetNativeRuntimeInstance(&runtime_instance)) ||
			!runtime_instance)
		{
			response = "error: no native runtime for modules";
			break;
		}

		CAutoDkmArray<DkmModuleInstance *> modules;
		if (FAILED(runtime_instance->GetModuleInstances(&modules)))
		{
			response = "error: failed to enumerate modules";
			break;
		}

		DWORD process_id = 0;
		const DkmProcess::Live *live = module_process->LivePart();
		if (live)
		{
			process_id = live->Id;
		}

		UINT32 native_count = 0;
		for (UINT32 i = 0; i < modules.Length; ++i)
		{
			if (modules.Members[i] &&
				DkmNativeModuleInstance::TryCast(modules.Members[i]))
			{
				++native_count;
			}
		}

		char header[128] = {};
		_snprintf_s(header, sizeof(header), _TRUNCATE,
					"modules: pid=%u count=%u | num base name",
					(unsigned)process_id,
					(unsigned)native_count);
		response = header;

		const UINT32 max_items = 1000;
		UINT32 emitted = 0;
		for (UINT32 i = 0; i < modules.Length; ++i)
		{
			DkmModuleInstance *module_instance = modules.Members[i];
			if (!module_instance)
			{
				continue;
			}

			DkmNativeModuleInstance *native_module =
				DkmNativeModuleInstance::TryCast(module_instance);
			if (!native_module)
			{
				continue;
			}

			if (emitted >= max_items)
			{
				response += " | ...";
				break;
			}

			char item[160] = {};
			_snprintf_s(item, sizeof(item), _TRUNCATE,
						" | #%u 0x%llX ",
						(unsigned)emitted,
						(unsigned long long)native_module->BaseAddress());
			response += item;

			DkmString *module_name = module_instance->Name();
			const wchar_t *module_text = DkmString::Value(module_name);
			if (module_text && module_text[0])
			{
				char module_utf8[260] = {};
				if (WideCharToMultiByte(CP_UTF8, 0, module_text, -1, module_utf8,
								 (int)sizeof(module_utf8), nullptr, nullptr) > 0)
				{
					response += module_utf8;
				}
				else
				{
					response += "<unknown>";
				}
			}
			else
			{
				response += "<unknown>";
			}

			++emitted;
		}

		break;
	}
	case command_type::trap_resume:
		bh_clear_break_session();
		response = "ok: trap session cleared";
		break;
	case command_type::capture_break:
		if (bh_capture_break_session_from_process(
				process,
				bh_break_session_state::reason_type::unknown))
		{
			response = "ok: captured break session";
		}
		else
		{
			response = "error: unable to capture break session";
		}
		break;
	case command_type::resolve_symbol: {
		const char *symbol_name = message + 15;
		while (*symbol_name == ' ' || *symbol_name == '\t')
		{
			++symbol_name;
		}

		if (!*symbol_name)
		{
			response = "error: missing symbol name";
			break;
		}

		UINT64 absolute_address = 0;
		std::string resolve_result =
			bh_resolve_symbol_address(symbol_name, absolute_address);
		if (resolve_result.rfind("ok", 0) != 0)
		{
			response = resolve_result;
			break;
		}

		char resolved[256] = {};
		_snprintf_s(resolved, sizeof(resolved), _TRUNCATE, "ok: symbol %s = 0x%llX",
					symbol_name, (unsigned long long)absolute_address);
		response = resolved;
		break;
	}
	case command_type::cont:
		response = bh_continue_process(process);
		if (response.rfind("ok:", 0) == 0)
		{
			bh_clear_break_session();
		}
		break;
	case command_type::async_break: {
		HRESULT hr = process->AsyncBreak(false);
		if (FAILED(hr))
		{
			char err[96] = {};
			_snprintf_s(err, sizeof(err), _TRUNCATE,
						"error: async break failed hr=0x%08X", (unsigned)hr);
			response = err;
		}
		else
		{
			response = "ok: async break requested";
		}
		break;
	}
	case command_type::read_mem: {
		const char *payload = message + 9;
		while (*payload == ' ' || *payload == '\t')
		{
			++payload;
		}

		const char *split = payload;
		while (*split && *split != ' ' && *split != '\t')
		{
			++split;
		}

		if (split == payload)
		{
			response = "error: read mem usage: read mem <address> <size> [hex|ascii|u32|u64|ptr]";
			break;
		}

		std::string address_text(payload, (size_t)(split - payload));
		while (*split == ' ' || *split == '\t')
		{
			++split;
		}

		if (!*split)
		{
			response = "error: read mem usage: read mem <address> <size> [hex|ascii|u32|u64|ptr]";
			break;
		}

		const char *size_start = split;
		while (*split && *split != ' ' && *split != '\t')
		{
			++split;
		}
		std::string size_text(size_start, (size_t)(split - size_start));

		while (*split == ' ' || *split == '\t')
		{
			++split;
		}

		std::string format_text;
		if (*split)
		{
			const char *format_start = split;
			while (*split && *split != ' ' && *split != '\t')
			{
				++split;
			}
			format_text.assign(format_start, (size_t)(split - format_start));
			while (*split == ' ' || *split == '\t')
			{
				++split;
			}
			if (*split)
			{
				response = "error: read mem usage: read mem <address> <size> [hex|ascii|u32|u64|ptr]";
				break;
			}
		}

		UINT64 address = 0;
		UINT64 size64 = 0;
		if (!bh_try_parse_u64(address_text.c_str(), address) ||
			!bh_try_parse_u64(size_text.c_str(), size64))
		{
			response = "error: read mem parse failed";
			break;
		}
		if (size64 > 0xFFFFFFFFULL)
		{
			response = "error: read mem size is too large";
			break;
		}

		bh_memory_format format = bh_memory_format::hex;
		if (!bh_try_parse_memory_format(format_text, format))
		{
			response = "error: read mem format must be hex, ascii, u32, u64, or ptr";
			break;
		}

		DWORD process_id_for_read = 0;
		if (trap_session.active)
		{
			process_id_for_read = trap_session.process_id;
		}
		else
		{
			bh_remote_breakpoint_state snapshot = {};
			CComPtr<DkmProcess> active_process;
			CComPtr<bh_agent_remote_process_data> process_data;
			if (bh_get_active_process_data(active_process, process_data) &&
				process_data)
			{
				AcquireSRWLockShared(&process_data->breakpoint_lock);
				snapshot = process_data->breakpoint_state;
				ReleaseSRWLockShared(&process_data->breakpoint_lock);
			}

			process_id_for_read = snapshot.process_id;
			if (process_id_for_read == 0 && active_process)
			{
				const DkmProcess::Live *live = active_process->LivePart();
				if (live)
				{
					process_id_for_read = live->Id;
				}
			}
		}

		if (process_id_for_read == 0)
		{
			response = "error: no target process for read mem";
			break;
		}

		response = bh_read_memory_command(
			process_id_for_read, address, (UINT32)size64, format);
		break;
	}
	case command_type::read_stack: {
		UINT32 size = 64;
		const char *payload = nullptr;
		if (_strnicmp(message, "read stack ", 11) == 0)
		{
			payload = message + 11;
		}
		else if (_strnicmp(message, "stack ", 6) == 0)
		{
			payload = message + 6;
		}

		if (payload)
		{
			UINT64 size64 = 0;
			while (*payload == ' ' || *payload == '\t')
			{
				++payload;
			}

			if (*payload)
			{
				if (!bh_try_parse_u64(payload, size64))
				{
					response = "error: stack parse failed";
					break;
				}
				size = (UINT32)size64;
			}
		}

		response = bh_read_stack_command(trap_session.process_id,
									 trap_session.thread_id, size);
		break;
	}
	case command_type::read_regs:
		response = bh_read_registers_command(trap_session.process_id,
									 trap_session.thread_id);
		break;
	case command_type::list_threads:
		response = bh_list_threads_command(trap_session);
		break;
	case command_type::select_thread:
	{
		const char *payload = message + 14;
		UINT64 thread_id64 = 0;
		if (!bh_try_parse_u64(payload, thread_id64) || thread_id64 == 0 ||
			thread_id64 > 0xFFFFFFFFULL)
		{
			response = "error: select thread usage: select thread <tid>";
			break;
		}

		response = bh_select_thread_command(trap_session, (UINT32)thread_id64);
		break;
	}
	case command_type::where:
	{
		UINT32 requested_frames = 16;
		if (_strnicmp(message, "where ", 6) == 0)
		{
			const char *payload = message + 6;
			while (*payload == ' ' || *payload == '\t')
			{
				++payload;
			}

			if (*payload)
			{
				UINT64 value64 = 0;
				if (!bh_try_parse_u64(payload, value64))
				{
					response = "error: where parse failed";
					break;
				}
				requested_frames = (UINT32)value64;
			}
		}

		if (requested_frames == 0)
		{
			requested_frames = 16;
		}

		if (requested_frames > 128)
		{
			requested_frames = 128;
		}

		CComPtr<DkmProcess> where_process;
		if (FAILED(DkmProcess::FindProcess(trap_session.process_unique_id,
										  &where_process)) ||
			!where_process)
		{
			response = "error: cannot resolve process for where";
			break;
		}

		CAutoDkmArray<DkmThread *> threads;
		if (FAILED(where_process->GetThreads(&threads)))
		{
			response = "error: failed to enumerate threads for where";
			break;
		}

		CComPtr<DkmThread> where_thread;
		for (UINT32 i = 0; i < threads.Length; ++i)
		{
			DkmThread *candidate = threads.Members[i];
			if (!candidate)
			{
				continue;
			}

			const DkmThread::System *system = candidate->SystemPart();
			if (system && system->Id == trap_session.thread_id)
			{
				where_thread = candidate;
				break;
			}
		}

		if (!where_thread)
		{
			response = "error: where thread not found";
			break;
		}

		CComPtr<CallStack::DkmStackWalkContext> walk_context;
		HRESULT hr = CallStack::DkmStackWalkContext::Create(
			where_thread,
			nullptr,
			DkmDataItem::Null(),
			&walk_context);
		if (FAILED(hr) || !walk_context)
		{
			response = "error: failed to create stack walk context";
			break;
		}

		char header[128] = {};
		_snprintf_s(header,
					sizeof(header),
					_TRUNCATE,
					"where: pid=%u tid=%u requested=%u | num ip sp symbol",
					(unsigned)trap_session.process_id,
					(unsigned)trap_session.thread_id,
					(unsigned)requested_frames);
		response = header;

		UINT32 emitted = 0;
		bool end_of_stack = false;
		while (emitted < requested_frames && !end_of_stack)
		{
			UINT32 remaining = requested_frames - emitted;
			UINT32 request_chunk = remaining > 32 ? 32 : remaining;

			CAutoDkmArray<CallStack::DkmStackWalkFrame *> frames;
			hr = walk_context->RuntimeWalkNextFrames(request_chunk, &frames,
												 &end_of_stack);
			if (FAILED(hr))
			{
				response = "error: stack walk failed";
				break;
			}

			if (frames.Length == 0)
			{
				break;
			}

			for (UINT32 i = 0; i < frames.Length && emitted < requested_frames; ++i)
			{
				CallStack::DkmStackWalkFrame *frame = frames.Members[i];
				if (!frame)
				{
					continue;
				}

				UINT64 ip = 0;
				std::string symbol_text = "<unknown>";
				std::string source_text;

				DkmInstructionAddress *instruction_address =
					frame->InstructionAddress();
				if (instruction_address)
				{
					const DkmInstructionAddress::CPUInstruction *cpu =
						instruction_address->CPUInstructionPart();
					if (cpu)
					{
						ip = cpu->InstructionPointer;
					}

					DkmNativeInstructionAddress *native_address =
						DkmNativeInstructionAddress::TryCast(instruction_address);
					if (native_address)
					{
						DkmNativeModuleInstance *module_instance =
							native_address->ModuleInstance();
						const wchar_t *module_text = nullptr;
						char module_utf8[260] = {};
						UINT64 absolute_address = 0;
						if (module_instance && ip == 0)
						{
							ip = module_instance->BaseAddress() + native_address->RVA();
						}
						absolute_address = ip;
						if (module_instance && absolute_address == 0)
						{
							absolute_address = module_instance->BaseAddress() + native_address->RVA();
						}

						DkmString *module_name = module_instance ? module_instance->Name() : nullptr;
						module_text = DkmString::Value(module_name);
						if (module_text && module_text[0])
						{
							if (WideCharToMultiByte(CP_UTF8,
											 0,
											 module_text,
											 -1,
											 module_utf8,
											 (int)sizeof(module_utf8),
											 nullptr,
											 nullptr) > 0)
							{
								if (absolute_address != 0)
								{
									(void)bh_resolve_address_symbol_text(where_process,
																module_text,
																absolute_address,
																symbol_text);
								}

								if (symbol_text.empty() || symbol_text == "<unknown>")
								{
									char module_symbol[320] = {};
									_snprintf_s(module_symbol,
												sizeof(module_symbol),
												_TRUNCATE,
												"%s+0x%X",
												module_utf8,
												(unsigned)native_address->RVA());
									symbol_text = module_symbol;
								}
							}
						}
					}

					CComPtr<Symbols::DkmInstructionSymbol> instruction_symbol;
					if (SUCCEEDED(instruction_address->GetSymbol(&instruction_symbol)) &&
						instruction_symbol)
					{
						bool start_of_line = false;
						CComPtr<Symbols::DkmSourcePosition> source_position;
						if (SUCCEEDED(instruction_symbol->GetSourcePositionCallback(
									   static_cast<Symbols::DkmSourcePositionFlags_t>(0),
									   nullptr,
									   &start_of_line,
									   &source_position)) &&
							source_position)
						{
							DkmString *document_name = source_position->DocumentName();
							const wchar_t *document_text = DkmString::Value(document_name);
							if (document_text && document_text[0])
							{
								char document_utf8[512] = {};
								if (WideCharToMultiByte(CP_UTF8,
												 0,
												 document_text,
												 -1,
												 document_utf8,
												 (int)sizeof(document_utf8),
												 nullptr,
												 nullptr) > 0)
								{
									const char *leaf = document_utf8;
									const char *last_backslash = strrchr(document_utf8, '\\');
									const char *last_slash = strrchr(document_utf8, '/');
									if (last_backslash && (!last_slash || last_backslash > last_slash))
									{
										leaf = last_backslash + 1;
									}
									else if (last_slash)
									{
										leaf = last_slash + 1;
									}

									const Symbols::DkmTextSpan &span = source_position->TextSpan();
									char location[640] = {};
									if (span.StartColumn != 0)
									{
										_snprintf_s(location,
													sizeof(location),
													_TRUNCATE,
													"%s:%u:%u",
													leaf,
													(unsigned)span.StartLine,
													(unsigned)span.StartColumn);
									}
									else
									{
										_snprintf_s(location,
													sizeof(location),
													_TRUNCATE,
													"%s:%u",
													leaf,
													(unsigned)span.StartLine);
									}
									source_text = location;
								}
							}
						}

						(void)start_of_line;
					}
				}

				DkmString *frame_description = frame->Description();
				const wchar_t *description_text = DkmString::Value(frame_description);
				if (description_text && description_text[0])
				{
					char description_utf8[512] = {};
					if (WideCharToMultiByte(CP_UTF8,
									 0,
									 description_text,
									 -1,
									 description_utf8,
									 (int)sizeof(description_utf8),
									 nullptr,
									 nullptr) > 0)
					{
						symbol_text = description_utf8;
					}
				}

				if (!source_text.empty())
				{
					if (symbol_text.empty() || symbol_text == "<unknown>")
					{
						symbol_text = source_text;
					}
					else
					{
						symbol_text += " @ ";
						symbol_text += source_text;
					}
				}

				char item[896] = {};
				_snprintf_s(item,
								sizeof(item),
								_TRUNCATE,
								" | #%u 0x%llX 0x%llX %s",
								(unsigned)emitted,
								(unsigned long long)ip,
								(unsigned long long)frame->FrameBase(),
								symbol_text.c_str());
				response += item;
				++emitted;
			}
		}

		if (response.rfind("error:", 0) != 0)
		{
			char tail[48] = {};
			_snprintf_s(tail,
						sizeof(tail),
						_TRUNCATE,
						" | end=%u count=%u",
						(unsigned)(end_of_stack ? 1 : 0),
						(unsigned)emitted);
			response += tail;
		}

		break;
	}
	default:
		break;
	}

	if (process)
	{
		process->Release();
	}

	return response;
}

} // namespace

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::OnModuleInstanceLoad(
	DkmModuleInstance *moduleInstance, DkmWorkList *workList,
	DkmEventDescriptorS *eventDescriptor)
{
	(void)workList;
	(void)eventDescriptor;

	if (!moduleInstance)
	{
		return S_OK;
	}

	DkmProcess *process = moduleInstance->Process();
	if (!process)
	{
		return S_OK;
	}

	DkmString *module_name = moduleInstance->Name();
	const wchar_t *module_text = DkmString::Value(module_name);
	char module_utf8[260] = {};
	if (module_text && module_text[0])
	{
		WideCharToMultiByte(CP_UTF8, 0, module_text, -1, module_utf8,
							(int)sizeof(module_utf8), nullptr, nullptr);
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return S_OK;
	}

	if (bh_refresh_target_module_for_process(process, process_data))
	{
		CComPtr<bh_agent_remote_connection_data> connection_data;
		if (SUCCEEDED(bh_agent_remote_connection_data::GetInstance(
					process->Connection(), &connection_data)) &&
			connection_data)
		{
			AcquireSRWLockExclusive(&connection_data->state_lock);
			connection_data->active_process_unique_id = process->UniqueId();
			connection_data->has_active_process = true;
			ReleaseSRWLockExclusive(&connection_data->state_lock);

			std::string pending_result =
				bh_try_apply_pending_actions(connection_data);
			if (pending_result.rfind("ok: applied ", 0) == 0)
			{
				if (pending_result.find("applied 0 ") == std::string::npos)
				{
					bh_agent_logf("engine.remote",
								  "OnModuleInstanceLoad pending apply: %s",
								  pending_result.c_str());
				}
			}
			else
			{
				bh_agent_logf("engine.remote",
							  "OnModuleInstanceLoad pending apply not ready: %s",
							  pending_result.c_str());
			}
		}
	}

	return S_OK;
}

HRESULT STDMETHODCALLTYPE
bh_agent_remote_component::OnProcessContinue(DkmProcess *pProcess)
{
	if (pProcess)
	{
		bh_clear_break_session_for_process(pProcess);
	}

	bh_agent_logf("engine.remote", "OnProcessContinue: cleared break session state");
	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::SendLower(
	DkmCustomMessage *pCustomMessage, DkmCustomMessage **ppReplyMessage)
{
	if (!pCustomMessage)
	{
		return E_POINTER;
	}

	UINT32 message_code = pCustomMessage->MessageCode();
	bh_agent_logf("engine.remote", "message=%u", (unsigned)message_code);

	if (message_code != bh_agent_message_from_script)
	{
		return S_OK;
	}

	DkmVariant *parameter = pCustomMessage->Parameter1();
	if (!parameter)
	{
		return E_INVALIDARG;
	}

	const VARIANT &value = parameter->Value();
	if (value.vt != VT_BSTR || !value.bstrVal)
	{
		return E_INVALIDARG;
	}

	char utf8[1024] = {};
	int converted = WideCharToMultiByte(
		CP_UTF8, 0, value.bstrVal, -1, utf8, (int)sizeof(utf8), nullptr, nullptr);
	if (converted <= 0)
	{
		return E_FAIL;
	}

	bh_agent_logf("engine.remote", "script message: %s", utf8);

	DkmProcess *command_process = pCustomMessage->Process();
	std::string command_response = bh_handle_script_command(command_process, utf8);
	if (command_response.empty())
	{
		command_response = "ok";
	}

	int response_wlen = MultiByteToWideChar(CP_UTF8, 0, command_response.c_str(),
		-1, nullptr, 0);
	if (response_wlen <= 0)
	{
		return E_FAIL;
	}

	std::wstring response_wide;
	response_wide.resize((size_t)response_wlen);
	if (MultiByteToWideChar(CP_UTF8, 0, command_response.c_str(), -1,
		&response_wide[0], response_wlen) <= 0)
	{
		return E_FAIL;
	}

	CComPtr<DkmVariant> response_variant;
	HRESULT hr = DkmVariant::Create(response_wide.c_str(), &response_variant);
	if (FAILED(hr) || !response_variant)
	{
		return FAILED(hr) ? hr : E_FAIL;
	}

	if (ppReplyMessage)
	{
		CComPtr<DkmCustomMessage> reply;
		DkmProcess *process = pCustomMessage->Process();
		if (!process)
		{
			return E_FAIL;
		}

		hr = DkmCustomMessage::Create(
			process->Connection(),
			process,
			bh_guid_agent_message_to_remote,
			bh_agent_message_from_script,
			response_variant,
			nullptr,
			&reply);
		if (FAILED(hr) || !reply)
		{
			return FAILED(hr) ? hr : E_FAIL;
		}

		*ppReplyMessage = reply.Detach();
	}

	return S_OK;
}

HRESULT STDMETHODCALLTYPE
bh_agent_remote_component::OnRuntimeBreakpointReceived(
	Breakpoints::DkmRuntimeBreakpoint *pRuntimeBreakpoint, DkmThread *pThread,
	bool HasException, DkmEventDescriptorS *pEventDescriptor)
{
	if (!pRuntimeBreakpoint || !pThread || !pEventDescriptor)
	{
		return E_INVALIDARG;
	}

	const GUID &source_id = pRuntimeBreakpoint->SourceId();
	const bool is_agent_trap = (source_id == bh_guid_agent_trap_breakpoint);

	DkmProcess *process = pRuntimeBreakpoint->Process();
	if (!process)
	{
		return E_FAIL;
	}

	DWORD process_id = 0;
	const DkmProcess::Live *live = process->LivePart();
	if (live)
	{
		process_id = live->Id;
	}

	DWORD tracked_target_pid = 0;
	if (process_id != 0 &&
		bh_get_tracked_target_pid_for_process(process, tracked_target_pid) &&
		tracked_target_pid != 0 && process_id != tracked_target_pid)
	{
		bh_agent_logf("engine.remote",
					  "OnRuntimeBreakpointReceived filtered pid=%u trackedPid=%u",
					  process_id,
					  (unsigned)tracked_target_pid);
		return S_OK;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return E_FAIL;
	}

	UINT64 trap_address = 0;
	CComPtr<Breakpoints::DkmRuntimeInstructionBreakpoint> instruction_breakpoint(
		Breakpoints::DkmRuntimeInstructionBreakpoint::TryCast(pRuntimeBreakpoint));
	if (instruction_breakpoint)
	{
		DkmNativeInstructionAddress *instruction_address =
			DkmNativeInstructionAddress::TryCast(
				instruction_breakpoint->InstructionAddress());
		if (instruction_address)
		{
			auto cpu = instruction_address->CPUInstructionPart();
			if (cpu)
			{
				trap_address = cpu->InstructionPointer;
			}
			else
			{
				DkmNativeModuleInstance *module_instance =
					instruction_address->ModuleInstance();
				if (module_instance)
				{
					trap_address =
						module_instance->BaseAddress() + instruction_address->RVA();
				}
			}
		}
	}

	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	if (process_id != 0)
	{
		process_data->breakpoint_state.process_id = process_id;
	}
	process_data->breakpoint_state.process_unique_id = process->UniqueId();
	process_data->breakpoint_state.has_target = true;
	if (trap_address != 0)
	{
		process_data->breakpoint_state.address = trap_address;
	}
	process_data->breakpoint_state.trap_enabled = !process_data->breakpoint_state.traps.empty();
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	if (trap_address == 0)
	{
		AcquireSRWLockShared(&process_data->breakpoint_lock);
		trap_address = process_data->breakpoint_state.address;
		ReleaseSRWLockShared(&process_data->breakpoint_lock);
	}

	bh_set_break_session(
		process,
		pThread,
		trap_address,
		trap_address,
		HasException,
		is_agent_trap ? bh_break_session_state::reason_type::trap
					 : bh_break_session_state::reason_type::breakpoint);
	if (is_agent_trap)
	{
		pEventDescriptor->Suppress();

		// Stop on the trapping thread itself. An AsyncBreak lets the suppressed
		// event resume the process first, so the thread runs past the trap and
		// OnProcessContinue clears the trap session before the break completes.
		HRESULT stop_hr = pThread->OnEmbeddedBreakpointHit(
			instruction_breakpoint ? instruction_breakpoint->InstructionAddress() : nullptr,
			false);
		if (SUCCEEDED(stop_hr))
		{
			bh_agent_logf("engine.remote",
						  "trap stop raised on thread (OnEmbeddedBreakpointHit)");
		}
		else
		{
			bh_agent_logf("engine.remote",
						  "OnEmbeddedBreakpointHit failed hr=0x%08X; falling back to AsyncBreak",
						  (unsigned)stop_hr);
			bh_request_async_break(process, "OnRuntimeBreakpointReceived");
		}
	}

	if (HasException)
	{
		bh_agent_logf("engine.remote",
					  "OnRuntimeBreakpointReceived observed exception at trap=0x%llX",
					  (unsigned long long)trap_address);
	}

	bh_agent_logf("engine.remote",
			  "OnRuntimeBreakpointReceived trap=0x%llX exception=%u sourceMatch=%u",
				  (unsigned long long)trap_address,
				  (unsigned)(HasException ? 1 : 0),
				  (unsigned)(is_agent_trap ? 1 : 0));

	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::OnStepCompleteReceived(
	Stepping::DkmStepper *pStepper,
	DkmThread *pThread,
	bool HasException,
	DkmEventDescriptorS *pEventDescriptor)
{
	if (!pEventDescriptor)
	{
		return E_INVALIDARG;
	}

	DkmThread *thread = pThread;
	if (!thread && pStepper)
	{
		thread = pStepper->Thread();
	}
	if (!thread)
	{
		return S_OK;
	}

	bh_set_break_session_from_thread(thread, HasException,
						  bh_break_session_state::reason_type::step);
	bh_agent_logf("engine.remote", "OnStepCompleteReceived hasException=%u",
				  (unsigned)(HasException ? 1 : 0));
	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::OnSingleStepCompleteReceived(
	Stepping::DkmSingleStepRequest *pSingleStepRequest,
	DkmEventDescriptorS *pEventDescriptor)
{
	if (!pSingleStepRequest || !pEventDescriptor)
	{
		return E_INVALIDARG;
	}

	DkmThread *thread = pSingleStepRequest->Thread();
	if (thread)
	{
		bh_set_break_session_from_thread(thread, false,
						  bh_break_session_state::reason_type::step);
	}

	bh_agent_logf("engine.remote", "OnSingleStepCompleteReceived");
	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::OnAsyncBreakCompleteReceived(
	DkmProcess *pProcess,
	DkmAsyncBreakStatus_t Status,
	DkmThread *pThread,
	DkmEventDescriptorS *pEventDescriptor)
{
	if (!pProcess || !pEventDescriptor)
	{
		return E_INVALIDARG;
	}

	DWORD process_id = 0;
	const DkmProcess::Live *live = pProcess->LivePart();
	if (live)
	{
		process_id = live->Id;
	}

	DWORD tracked_target_pid = 0;
	if (process_id != 0 &&
		bh_get_tracked_target_pid_for_process(pProcess, tracked_target_pid) &&
		tracked_target_pid != 0 && process_id != tracked_target_pid)
	{
		bh_agent_logf("engine.remote",
					  "OnAsyncBreakCompleteReceived filtered pid=%u trackedPid=%u",
					  process_id,
					  (unsigned)tracked_target_pid);
		return S_OK;
	}

	bh_break_session_state existing_session =
		bh_get_break_session_snapshot(pProcess);
	if (existing_session.active)
	{
		bh_agent_logf("engine.remote",
					  "OnAsyncBreakCompleteReceived preserved existing reason=%s pid=%u thread=%p",
					  bh_break_reason_to_text(existing_session.reason),
					  (unsigned)process_id,
					  (void *)pThread);
		return S_OK;
	}

	if (pThread)
	{
		bh_set_break_session_from_thread(
			pThread,
			false,
			bh_break_session_state::reason_type::async_break);
	}
	else
	{
		(void)bh_capture_break_session_from_process(
			pProcess,
			bh_break_session_state::reason_type::async_break);
	}

	bh_agent_logf("engine.remote",
				  "OnAsyncBreakCompleteReceived pid=%u status=%u thread=%p",
				  (unsigned)process_id,
				  (unsigned)Status,
				  (void *)pThread);

	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::OnDebugMonitorException(
	Exceptions::DkmExceptionInformation *pException,
	DkmWorkList *pWorkList,
	DkmEventDescriptorS *pEventDescriptor)
{
	(void)pWorkList;
	if (!pException || !pEventDescriptor)
	{
		return E_INVALIDARG;
	}

	unsigned int process_id = 0;
	DkmProcess *process = pException->Process();
	if (process && process->LivePart())
	{
		process_id = process->LivePart()->Id;
	}

	DWORD tracked_target_pid = 0;
	if (process_id != 0 &&
		bh_get_tracked_target_pid_for_process(process, tracked_target_pid) &&
		tracked_target_pid != 0 && process_id != tracked_target_pid)
	{
		bh_agent_logf("engine.remote",
					  "OnDebugMonitorException filtered pid=%u trackedPid=%u",
					  process_id,
					  (unsigned)tracked_target_pid);
		return S_OK;
	}

	const void *thread_ptr = pException->Thread();
	const wchar_t *exception_name = L"";
	DkmString *name = pException->Name();
	if (name)
	{
		exception_name = DkmString::Value(name);
	}

	UINT32 code = pException->Code();
	UINT32 stage = static_cast<UINT32>(pException->ProcessingStage());
	UINT64 instruction_pointer = 0;

	if (bh_process_is_devenv(process_id))
	{
		bh_agent_logf("engine.remote",
					  "OnDebugMonitorException skipped host pid=%u thread=%p code=0x%08X stage=%u",
					  process_id,
					  thread_ptr,
					  (unsigned)code,
					  (unsigned)stage);
		return S_OK;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (process &&
		SUCCEEDED(bh_agent_remote_process_data::GetInstance(process, &process_data)) &&
		process_data)
	{
		bh_pending_exception_state pending = {};
		pending.pending = true;
		pending.process_id = process_id;
		pending.thread_marker = static_cast<UINT64>(reinterpret_cast<UINT_PTR>(thread_ptr));
		pending.code = code;
		pending.stage = stage;
		pending.tick_ms = GetTickCount64();
		if (exception_name)
		{
			char name_utf8[260] = {};
			if (WideCharToMultiByte(CP_UTF8, 0, exception_name, -1, name_utf8,
									 (int)sizeof(name_utf8), nullptr, nullptr) > 0)
			{
				pending.name = name_utf8;
			}
		}

		AcquireSRWLockExclusive(&process_data->exception_lock);
		process_data->pending_exception = pending;
		ReleaseSRWLockExclusive(&process_data->exception_lock);
	}

	DkmNativeInstructionAddress *instruction_address =
		DkmNativeInstructionAddress::TryCast(pException->InstructionAddress());
	if (instruction_address)
	{
		auto cpu = instruction_address->CPUInstructionPart();
		if (cpu)
		{
			instruction_pointer = cpu->InstructionPointer;
		}
		else
		{
			DkmNativeModuleInstance *module_instance = instruction_address->ModuleInstance();
			if (module_instance)
			{
				instruction_pointer = module_instance->BaseAddress() + instruction_address->RVA();
			}
		}
	}
	DkmThread *exception_thread = pException->Thread();
	if (process && exception_thread)
	{
		bh_set_break_session(
			process,
			exception_thread,
			instruction_pointer,
			instruction_pointer,
			true,
			bh_break_session_state::reason_type::exception);
	}

	bh_agent_logf("engine.remote",
				  "OnDebugMonitorException pid=%u thread=%p code=0x%08X stage=%u ip=0x%llX name='%S'",
				  process_id,
				  thread_ptr,
				  (unsigned)code,
				  (unsigned)stage,
				  (unsigned long long)instruction_pointer,
				  exception_name ? exception_name : L"");

	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::EnableRuntimeBreakpoint(
	Breakpoints::DkmRuntimeBreakpoint *pRuntimeBreakpoint)
{
	if (!pRuntimeBreakpoint)
	{
		return E_INVALIDARG;
	}

	const GUID &source_id = pRuntimeBreakpoint->SourceId();
	bh_agent_logf("engine.remote", "EnableRuntimeBreakpoint source_match=%u",
				  (unsigned)(source_id == bh_guid_agent_trap_breakpoint ? 1 : 0));
	if (source_id != bh_guid_agent_trap_breakpoint)
	{
		return S_OK;
	}

	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::TestRuntimeBreakpoint(
	Breakpoints::DkmRuntimeBreakpoint *pRuntimeBreakpoint)
{
	if (!pRuntimeBreakpoint)
	{
		return E_INVALIDARG;
	}

	const GUID &source_id = pRuntimeBreakpoint->SourceId();
	if (source_id != bh_guid_agent_trap_breakpoint)
	{
		return S_OK;
	}

	return S_OK;
}

HRESULT STDMETHODCALLTYPE bh_agent_remote_component::DisableRuntimeBreakpoint(
	Breakpoints::DkmRuntimeBreakpoint *pRuntimeBreakpoint)
{
	if (!pRuntimeBreakpoint)
	{
		return E_INVALIDARG;
	}

	const GUID &source_id = pRuntimeBreakpoint->SourceId();
	if (source_id != bh_guid_agent_trap_breakpoint)
	{
		return E_NOTIMPL;
	}

	DkmProcess *process = pRuntimeBreakpoint->Process();
	if (!process)
	{
		return E_FAIL;
	}

	CComPtr<bh_agent_remote_process_data> process_data;
	if (FAILED(
			bh_agent_remote_process_data::GetInstance(process, &process_data)) ||
		!process_data)
	{
		return E_FAIL;
	}

	AcquireSRWLockExclusive(&process_data->breakpoint_lock);
	auto &traps = process_data->breakpoint_state.traps;
	for (auto it = traps.begin(); it != traps.end(); ++it)
	{
		if (it->breakpoint == pRuntimeBreakpoint)
		{
			traps.erase(it);
			break;
		}
	}
	if (process_data->breakpoint_state.trap_breakpoint == pRuntimeBreakpoint)
	{
		process_data->breakpoint_state.trap_breakpoint = nullptr;
	}
	process_data->breakpoint_state.trap_enabled = !traps.empty();
	if (!process_data->breakpoint_state.trap_enabled)
	{
		process_data->breakpoint_state.address = 0;
	}
	ReleaseSRWLockExclusive(&process_data->breakpoint_lock);

	return S_OK;
}
