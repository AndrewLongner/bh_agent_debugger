#include "named_pipe_transport.h"
#include "bh_log.h"
#include "dte_rot.h"
#include "bh_vs_profiler.h"
#include "../../bh_agent_start_arguments.h"
#include "../../bh_agent_version.h"
#include "../../bh_agent_pipe_security.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace bh_agent_transport
{
enum class package_request_failure
{
	none,
	timeout,
	broken_pipe,
	transport,
};

bool send_request(
	const std::wstring &pipe_name,
	const std::string &request,
	std::string &response,
	unsigned int connect_timeout_ms);
bool send_request(
	const std::wstring &pipe_name,
	const std::string &request,
	std::string &response,
	unsigned int connect_timeout_ms,
	DWORD &last_error,
	package_request_failure &failure);
} // namespace bh_agent_transport

namespace
{
std::string escape_json(const std::string &input)
{
	std::string out;
	out.reserve(input.size() + 16);
	for (char ch : input)
	{
		switch (ch)
		{
		case '\\':
			out += "\\\\";
			break;
		case '"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			out.push_back(ch);
			break;
		}
	}

	return out;
}

std::wstring make_pipe_path(const std::wstring &pipe_name)
{
	return L"\\\\.\\pipe\\" + pipe_name;
}

std::string probe_package_endpoint(const std::string &pipe_name);
std::string build_default_pipe_for_target_id(const std::string &target_id);

std::string narrow_ascii(const std::wstring &text)
{
	std::string out;
	out.reserve(text.size());
	for (wchar_t ch : text)
	{
		out.push_back(static_cast<char>(ch));
	}

	return out;
}

void skip_ws(const std::string &text, size_t &pos)
{
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0)
	{
		++pos;
	}
}

bool find_top_level_json_member_value(const std::string &text, const std::string &key, size_t &value_pos)
{
	value_pos = std::string::npos;
	unsigned int object_depth = 0;
	bool in_string = false;
	bool escaping = false;
	std::string string_value;
	for (size_t pos = 0; pos < text.size(); ++pos)
	{
		const char ch = text[pos];
		if (in_string)
		{
			if (escaping)
			{
				string_value.push_back(ch);
				escaping = false;
				continue;
			}
			if (ch == '\\')
			{
				escaping = true;
				continue;
			}
			if (ch != '"')
			{
				string_value.push_back(ch);
				continue;
			}

			in_string = false;
			size_t next = pos + 1;
			skip_ws(text, next);
			if (object_depth == 1 && next < text.size() && text[next] == ':' && string_value == key)
			{
				value_pos = next + 1;
				skip_ws(text, value_pos);
				return true;
			}
			continue;
		}

		if (ch == '"')
		{
			in_string = true;
			string_value.clear();
		}
		else if (ch == '{')
		{
			++object_depth;
		}
		else if (ch == '}' && object_depth != 0)
		{
			--object_depth;
		}
	}
	return false;
}

bool has_top_level_json_member(const std::string &text, const std::string &key)
{
	size_t value_pos = std::string::npos;
	return find_top_level_json_member_value(text, key, value_pos);
}

bool extract_json_string(const std::string &text, const std::string &key, std::string &value)
{
	size_t colon_pos = std::string::npos;
	if (!find_top_level_json_member_value(text, key, colon_pos))
	{
		return false;
	}
	if (colon_pos >= text.size() || text[colon_pos] != '"')
	{
		return false;
	}

	++colon_pos;
	std::string out;
	bool escaping = false;
	while (colon_pos < text.size())
	{
		const char ch = text[colon_pos++];
		if (escaping)
		{
			switch (ch)
			{
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			case '\\':
				out.push_back('\\');
				break;
			case '"':
				out.push_back('"');
				break;
			default:
				out.push_back(ch);
				break;
			}

			escaping = false;
			continue;
		}

		if (ch == '\\')
		{
			escaping = true;
			continue;
		}

		if (ch == '"')
		{
			value = out;
			return true;
		}

		out.push_back(ch);
	}

	return false;
}

bool extract_json_uint(const std::string &text, const std::string &key, unsigned int &value)
{
	const std::string token = std::string("\"") + key + "\"";
	const size_t key_pos = text.find(token);
	if (key_pos == std::string::npos)
	{
		return false;
	}

	size_t colon_pos = text.find(':', key_pos + token.size());
	if (colon_pos == std::string::npos)
	{
		return false;
	}

	++colon_pos;
	skip_ws(text, colon_pos);
	if (colon_pos >= text.size() || !std::isdigit(static_cast<unsigned char>(text[colon_pos])))
	{
		return false;
	}

	unsigned int parsed = 0;
	while (colon_pos < text.size() && std::isdigit(static_cast<unsigned char>(text[colon_pos])) != 0)
	{
		parsed = (parsed * 10U) + static_cast<unsigned int>(text[colon_pos] - '0');
		++colon_pos;
	}

	value = parsed;
	return true;
}

bool extract_json_bool(const std::string &text, const std::string &key, bool &value)
{
	const std::string token = std::string("\"") + key + "\"";
	const size_t key_pos = text.find(token);
	if (key_pos == std::string::npos)
	{
		return false;
	}

	size_t colon_pos = text.find(':', key_pos + token.size());
	if (colon_pos == std::string::npos)
	{
		return false;
	}

	++colon_pos;
	skip_ws(text, colon_pos);
	if (text.compare(colon_pos, 4, "true") == 0)
	{
		value = true;
		return true;
	}

	if (text.compare(colon_pos, 5, "false") == 0)
	{
		value = false;
		return true;
	}

	return false;
}

std::string build_ok_response(const std::string &id, const std::string &text)
{
	return std::string("{\"protocolVersion\":1,\"id\":\"") + escape_json(id) +
		   "\",\"ok\":true,\"result\":{\"text\":\"" + escape_json(text) + "\"}}";
}

std::string build_error_response(const std::string &id, const std::string &code, const std::string &text)
{
	return std::string("{\"protocolVersion\":1,\"id\":\"") + escape_json(id) +
		   "\",\"ok\":false,\"error\":{\"code\":\"" + escape_json(code) +
		   "\",\"message\":\"" + escape_json(text) + "\"}}";
}

unsigned long long now_millis()
{
	return static_cast<unsigned long long>(GetTickCount64());
}

void log_event(const std::string &text)
{
	bh_log::write("service", text);
}

std::string to_lower_ascii(const std::string &text)
{
	std::string value = text;
	for (char &ch : value)
	{
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}

	return value;
}

struct target_endpoint
{
	std::string target_id;
	std::string pipe_name;
	bool is_static;
	unsigned long long last_seen_ms;
	std::string package_version;
};

void prune_stale_targets(std::vector<target_endpoint> &targets, unsigned long long now_ms, unsigned long long stale_after_ms)
{
	const size_t before = targets.size();
	targets.erase(
		std::remove_if(
			targets.begin(),
			targets.end(),
			[&](const target_endpoint &entry) {
				if (entry.is_static)
				{
					return false;
				}

				if (now_ms <= entry.last_seen_ms)
				{
					return false;
				}

				return ((now_ms - entry.last_seen_ms) > stale_after_ms);
			}),
		targets.end());

	if (targets.size() != before)
	{
		log_event("prune stale targets removed=" + std::to_string(before - targets.size()));
	}
}

int find_target_index_by_id(const std::vector<target_endpoint> &targets, const std::string &target_id)
{
	for (size_t i = 0; i < targets.size(); ++i)
	{
		if (targets[i].target_id == target_id)
		{
			return static_cast<int>(i);
		}
	}

	return -1;
}

int find_target_index_by_pipe_name(const std::vector<target_endpoint> &targets, const std::string &pipe_name)
{
	for (size_t i = 0; i < targets.size(); ++i)
	{
		if (targets[i].pipe_name == pipe_name)
		{
			return static_cast<int>(i);
		}
	}

	return -1;
}

std::string build_target_id_from_pid(unsigned int process_id)
{
	if (process_id == 0)
	{
		return "";
	}

	return std::string("vs_") + std::to_string(process_id);
}

bool try_infer_target_for_suffix(
	const std::string &suffix,
	std::string &target_id,
	std::string &pipe_name,
	std::string &error_text)
{
	target_id.clear();
	pipe_name.clear();
	error_text.clear();

	unsigned int process_id = 0;
	if (!bh_agent_dte::get_instance_pid_for_suffix(suffix, process_id, error_text))
	{
		return false;
	}

	target_id = build_target_id_from_pid(process_id);
	if (target_id.empty())
	{
		error_text = "resolved PID was empty";
		return false;
	}

	pipe_name = build_default_pipe_for_target_id(target_id);
	return true;
}

void upsert_target(std::vector<target_endpoint> &targets, const std::string &target_id, const std::string &pipe_name, bool is_static)
{
	if (target_id.empty() || pipe_name.empty())
	{
		return;
	}

	const int index = find_target_index_by_id(targets, target_id);
	if (index >= 0)
	{
		targets[static_cast<size_t>(index)].pipe_name = pipe_name;
		targets[static_cast<size_t>(index)].last_seen_ms = now_millis();
		targets[static_cast<size_t>(index)].is_static = is_static;
		return;
	}

	targets.push_back(target_endpoint{target_id, pipe_name, is_static, now_millis()});
}

int find_single_non_static_target_index(const std::vector<target_endpoint> &targets)
{
	int index = -1;
	for (size_t i = 0; i < targets.size(); ++i)
	{
		if (targets[i].is_static)
		{
			continue;
		}

		if (index >= 0)
		{
			return -1;
		}

		index = static_cast<int>(i);
	}

	return index;
}

// Versions are reported verbatim only when they are short dotted numbers.
std::string sanitize_version_text(const std::string &value)
{
	if (value.empty())
	{
		return std::string();
	}

	if (value.size() > 32)
	{
		return "invalid";
	}

	for (const char ch : value)
	{
		if ((ch < '0' || ch > '9') && ch != '.')
		{
			return "invalid";
		}
	}

	return value;
}

std::string version_or_unreported(const std::string &value)
{
	return value.empty() ? std::string("unreported") : value;
}

std::string build_targets_list_text(const std::vector<target_endpoint> &targets)
{
	if (targets.empty())
	{
		return "targets none";
	}

	std::string out = "targets count=" + std::to_string(targets.size());
	for (size_t i = 0; i < targets.size(); ++i)
	{
		unsigned long long age_sec = 0;
		const unsigned long long now_ms = now_millis();
		if (now_ms > targets[i].last_seen_ms)
		{
			age_sec = (now_ms - targets[i].last_seen_ms) / 1000ULL;
		}

		out += " | #" + std::to_string(i) + " id='" + targets[i].target_id + "' pipe='" + targets[i].pipe_name + "' ageSec=" + std::to_string(age_sec) +
			   " vsix=" + version_or_unreported(targets[i].package_version);
	}

	return out;
}

std::string build_target_list_entry_text(const std::vector<target_endpoint> &targets, const std::string &target_id)
{
	const int index = find_target_index_by_id(targets, target_id);
	if (index < 0)
	{
		return std::string();
	}

	const size_t item_index = static_cast<size_t>(index);
	const unsigned long long now_ms = now_millis();
	unsigned long long age_sec = 0;
	if (now_ms > targets[item_index].last_seen_ms)
	{
		age_sec = (now_ms - targets[item_index].last_seen_ms) / 1000ULL;
	}

	return "#" + std::to_string(item_index) + " id='" + targets[item_index].target_id + "' pipe='" + targets[item_index].pipe_name + "' ageSec=" + std::to_string(age_sec) +
		   " vsix=" + version_or_unreported(targets[item_index].package_version);
}

std::string build_default_pipe_for_target_id(const std::string &target_id)
{
	if (target_id.empty() || target_id == "default")
	{
		return "bh_agent_debugger_agent";
	}

	return std::string("bh_agent_debugger_agent_") + target_id;
}

std::string build_dte_list_text()
{
	std::vector<bh_agent_dte::dte_instance_info> instances;
	std::string error_text;
	if (!bh_agent_dte::enumerate_dte_instances(instances, error_text))
	{
		return "dte error: " + error_text;
	}

	if (instances.empty())
	{
		return "dte count=0";
	}

	std::string out = "dte count=" + std::to_string(instances.size());
	for (size_t i = 0; i < instances.size(); ++i)
	{
		const bh_agent_dte::dte_instance_info &item = instances[i];
		const std::string solution_text = item.solution_path.empty() ? "<none>" : item.solution_path;
		out += " | #" + std::to_string(i) + " pid=" + std::to_string(item.process_id) +
			   " mode=" + item.debug_mode +
			   " root='" + item.registry_root +
			   "' solution='" + solution_text + "'";
	}

	return out;
}

std::string build_health_summary_text(const std::vector<target_endpoint> &targets, const std::string &client_version)
{
	std::string out = "health";
	out += std::string(" | versions service=") + bh_agent_version::version +
		   " cli=" + version_or_unreported(client_version) +
		   " features=" + bh_agent_version::features;
	out += " | " + build_targets_list_text(targets);
	out += " | " + build_dte_list_text();

	if (targets.empty())
	{
		out += " | probes none";
		return out;
	}

	for (size_t i = 0; i < targets.size(); ++i)
	{
		const std::string probe = probe_package_endpoint(targets[i].pipe_name);
		out += " | " + probe;
	}

	return out;
}

bool read_line(HANDLE handle, std::string &text)
{
	text.clear();
	char ch = 0;
	DWORD bytes_read = 0;

	while (true)
	{
		if (!ReadFile(handle, &ch, 1, &bytes_read, nullptr))
		{
			return false;
		}

		if (bytes_read == 0)
		{
			return false;
		}

		if (ch == '\n')
		{
			return true;
		}

		if (ch != '\r')
		{
			text.push_back(ch);
		}
	}
}

bool write_line(HANDLE handle, const std::string &text)
{
	std::string payload = text;
	payload.push_back('\n');

	DWORD bytes_written = 0;
	return WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr) != FALSE;
}

bool wait_for_package_endpoint(const std::string &pipe_name, unsigned int timeout_ms)
{
	if (pipe_name.empty())
	{
		return false;
	}

	const unsigned long long start_ms = now_millis();
	while ((now_millis() - start_ms) <= static_cast<unsigned long long>(timeout_ms))
	{
		std::string ignored_response;
		if (bh_agent_transport::send_request(
				std::wstring(pipe_name.begin(), pipe_name.end()),
				"status",
				ignored_response,
				1000))
		{
			return true;
		}

		Sleep(200);
	}

	return false;
}

bool is_broken_pipe_error(DWORD error)
{
	return error == ERROR_BROKEN_PIPE ||
		   error == ERROR_NO_DATA ||
		   error == ERROR_PIPE_NOT_CONNECTED;
}

bool is_stale_pipe_error(DWORD error)
{
	return is_broken_pipe_error(error) ||
		   error == ERROR_FILE_NOT_FOUND ||
		   error == ERROR_PATH_NOT_FOUND;
}

unsigned int remaining_timeout_ms(unsigned long long start_ms, unsigned int total_timeout_ms)
{
	const unsigned long long elapsed = now_millis() - start_ms;
	if (elapsed >= static_cast<unsigned long long>(total_timeout_ms))
	{
		return 0;
	}

	return static_cast<unsigned int>(static_cast<unsigned long long>(total_timeout_ms) - elapsed);
}

bool wait_for_overlapped_result(
	HANDLE handle,
	OVERLAPPED &overlapped,
	unsigned int timeout_ms,
	DWORD &bytes_transferred,
	DWORD &last_error,
	bool &timed_out)
{
	timed_out = false;
	const DWORD wait_result = WaitForSingleObject(overlapped.hEvent, timeout_ms);
	if (wait_result == WAIT_TIMEOUT)
	{
		timed_out = true;
		last_error = WAIT_TIMEOUT;
		CancelIo(handle);
		return false;
	}

	if (wait_result != WAIT_OBJECT_0)
	{
		last_error = GetLastError();
		return false;
	}

	if (!GetOverlappedResult(handle, &overlapped, &bytes_transferred, FALSE))
	{
		last_error = GetLastError();
		return false;
	}

	return true;
}

bool write_line_with_timeout(
	HANDLE handle,
	const std::string &text,
	unsigned int timeout_ms,
	DWORD &last_error,
	bool &timed_out)
{
	timed_out = false;
	last_error = ERROR_SUCCESS;
	std::string payload = text;
	payload.push_back('\n');

	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!overlapped.hEvent)
	{
		last_error = GetLastError();
		return false;
	}

	const unsigned long long start_ms = now_millis();
	size_t total_written = 0;
	while (total_written < payload.size())
	{
		const unsigned int remaining_timeout = remaining_timeout_ms(start_ms, timeout_ms);
		if (remaining_timeout == 0)
		{
			timed_out = true;
			last_error = WAIT_TIMEOUT;
			CloseHandle(overlapped.hEvent);
			return false;
		}

		ResetEvent(overlapped.hEvent);
		DWORD bytes_written = 0;
		const BOOL write_ok = WriteFile(
			handle,
			payload.data() + total_written,
			static_cast<DWORD>(payload.size() - total_written),
			&bytes_written,
			&overlapped);
		if (!write_ok)
		{
			const DWORD write_error = GetLastError();
			if (write_error != ERROR_IO_PENDING)
			{
				last_error = write_error;
				CloseHandle(overlapped.hEvent);
				return false;
			}

			if (!wait_for_overlapped_result(handle, overlapped, remaining_timeout, bytes_written, last_error, timed_out))
			{
				CloseHandle(overlapped.hEvent);
				return false;
			}
		}

		if (bytes_written == 0)
		{
			last_error = ERROR_NO_DATA;
			CloseHandle(overlapped.hEvent);
			return false;
		}

		total_written += static_cast<size_t>(bytes_written);
	}

	CloseHandle(overlapped.hEvent);
	return true;
}

bool read_line_with_timeout(
	HANDLE handle,
	std::string &text,
	unsigned int timeout_ms,
	DWORD &last_error,
	bool &timed_out)
{
	text.clear();
	timed_out = false;
	last_error = ERROR_SUCCESS;

	OVERLAPPED overlapped = {};
	overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!overlapped.hEvent)
	{
		last_error = GetLastError();
		return false;
	}

	const unsigned long long start_ms = now_millis();
	char ch = 0;
	while (true)
	{
		const unsigned int remaining_timeout = remaining_timeout_ms(start_ms, timeout_ms);
		if (remaining_timeout == 0)
		{
			timed_out = true;
			last_error = WAIT_TIMEOUT;
			CloseHandle(overlapped.hEvent);
			return false;
		}

		ResetEvent(overlapped.hEvent);
		DWORD bytes_read = 0;
		const BOOL read_ok = ReadFile(handle, &ch, 1, &bytes_read, &overlapped);
		if (!read_ok)
		{
			const DWORD read_error = GetLastError();
			if (read_error != ERROR_IO_PENDING)
			{
				last_error = read_error;
				CloseHandle(overlapped.hEvent);
				return false;
			}

			if (!wait_for_overlapped_result(handle, overlapped, remaining_timeout, bytes_read, last_error, timed_out))
			{
				CloseHandle(overlapped.hEvent);
				return false;
			}
		}

		if (bytes_read == 0)
		{
			last_error = ERROR_NO_DATA;
			CloseHandle(overlapped.hEvent);
			return false;
		}

		if (ch == '\n')
		{
			CloseHandle(overlapped.hEvent);
			return true;
		}

		if (ch != '\r')
		{
			text.push_back(ch);
		}
	}
}

std::string probe_package_endpoint(const std::string &pipe_name)
{
	if (pipe_name.empty())
	{
		return "probe pipe=<empty> ok=false reason=empty";
	}

	const std::wstring pipe_path = make_pipe_path(std::wstring(pipe_name.begin(), pipe_name.end()));
	const BOOL wait_ok = WaitNamedPipeW(pipe_path.c_str(), 1000);
	if (!wait_ok)
	{
		const DWORD err = GetLastError();
		return "probe pipe='" + pipe_name + "' ok=false stage=WaitNamedPipe err=" + std::to_string(err);
	}

	HANDLE handle = CreateFileW(
		pipe_path.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0,
		nullptr,
		OPEN_EXISTING,
		0,
		nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		const DWORD err = GetLastError();
		return "probe pipe='" + pipe_name + "' ok=false stage=CreateFile err=" + std::to_string(err);
	}

	const bool owned = bh_agent_pipe_security::server_owned_by_current_user(handle);
	CloseHandle(handle);
	if (!owned)
	{
		return "probe pipe='" + pipe_name + "' ok=false stage=owner reason=not_current_user";
	}
	return "probe pipe='" + pipe_name + "' ok=true";
}
} // namespace

namespace bh_agent_transport
{
bool run_service_loop(const std::wstring &pipe_name, const std::wstring &default_target_pipe_name)
{
	const std::wstring pipe_path = make_pipe_path(pipe_name);
	const unsigned long long stale_after_ms = 120000ULL;
	std::vector<target_endpoint> targets;
	std::unordered_map<std::string, unsigned int> hint_pid_cache;
	// Requests are serialized by this loop; capture has its own bounded lifetime.
	bh_vs_profiler::session cpu_profile;

	// Current-user-only pipe; remote clients rejected. Never fall back to default security.
	bh_agent_pipe_security::server_attributes pipe_security;
	if (!pipe_security.get())
	{
		log_event("service not started: could not build current-user pipe security");
		return false;
	}

	log_event("service start pipe='" + narrow_ascii(pipe_name) + "' defaultTargetPipe='" + narrow_ascii(default_target_pipe_name) + "' security=current-user,reject-remote");
	while (true)
	{
		HANDLE pipe_handle = CreateNamedPipeW(
			pipe_path.c_str(),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | bh_agent_pipe_security::server_pipe_mode_flags,
			PIPE_UNLIMITED_INSTANCES,
			64 * 1024,
			64 * 1024,
			0,
			pipe_security.get());

		if (pipe_handle == INVALID_HANDLE_VALUE)
		{
			log_event("CreateNamedPipeW failed err=" + std::to_string(GetLastError()));
			Sleep(100);
			continue;
		}

		const BOOL connected = ConnectNamedPipe(pipe_handle, nullptr)
								   ? TRUE
								   : (GetLastError() == ERROR_PIPE_CONNECTED);

		if (!connected)
		{
			CloseHandle(pipe_handle);
			continue;
		}

		std::string request;
		if (read_line(pipe_handle, request))
		{
			std::string id = "";
			(void)extract_json_string(request, "id", id);

			std::string method;
			if (!extract_json_string(request, "method", method))
			{
				log_event("request rejected id='" + id + "' reason=missing_method");
				const std::string response = build_error_response(id, "invalid_request", "missing method");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			prune_stale_targets(targets, now_millis(), stale_after_ms);
			const bool is_register_heartbeat = (id == "pkg-register" && method == "targets.register");
			if (!is_register_heartbeat)
			{
				log_event("request id='" + id + "' method='" + method + "'");
			}

			if (method == "profile.command")
			{
				std::string message;
				std::string text;
				if (!extract_json_string(request, "message", message))
					text = "error: profile message is required";
				else if (request.find("\"pipeName\"") != std::string::npos)
					text = "error: profile package pipe routing is not supported";
				else if (message == "profile open")
				{
					std::string target_id, error, mode;
					DWORD vs_pid = 0;
					std::wstring report;
					if (!extract_json_string(request, "targetId", target_id) || target_id.rfind("vs_", 0) != 0 ||
						!bh_vs_profiler::detail::decimal(target_id.substr(3), vs_pid))
						text = "error: profile open requires an explicit vs_<pid> target; refresh DTE first";
					else if (!cpu_profile.report_path(report))
						text = "error: profile has no completed report";
					else if (!bh_agent_dte::get_debug_mode_for_pid(vs_pid, mode, error))
						text = "error: profile DTE selection failed: " + error;
					else if (mode != "design")
						text = "error: profile open requires VS design mode; leave the active debug session unchanged";
					else if (!bh_agent_dte::open_profile_report_for_pid(vs_pid, report, error))
						text = "error: " + error;
					else text = "ok: profile report opened target=" + target_id + " output=\"" + bh_vs_profiler::detail::utf8(report) + "\"";
				}
				else if (request.find("\"targetId\"") != std::string::npos)
					text = "error: profile capture requires an explicit debuggee PID, not debugger target routing";
				else
				{
					// Optional caller-selected destination; validated by the session.
					std::string output;
					if (has_top_level_json_member(request, "output") && (!extract_json_string(request, "output", output) || output.empty()))
						text = "error: profile output must be a non-empty string";
					else
						text = cpu_profile.command(message, output);
				}
				const std::string response = text.rfind("ok:", 0) == 0
					? build_ok_response(id, text) : build_error_response(id, "profile_error", text);
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "targets.list")
			{
				const std::string response = build_ok_response(id, build_targets_list_text(targets));
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.startHint")
			{
				unsigned int timeout_ms = 15000;
				(void)extract_json_uint(request, "timeoutMs", timeout_ms);
				if (timeout_ms < 1000)
				{
					timeout_ms = 1000;
				}

				std::string solution_hint;
				if (!extract_json_string(request, "solutionHint", solution_hint) || solution_hint.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing solutionHint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string launch_arguments;
				const bool arguments_present = has_top_level_json_member(request, "arguments");
				const bool has_launch_arguments = extract_json_string(request, "arguments", launch_arguments);
				if (arguments_present && !has_launch_arguments)
				{
					const std::string response = build_error_response(id, "invalid_request", "arguments must be a JSON string");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string dte_error;
				unsigned int process_id = 0;
				const std::string hint_key = to_lower_ascii(solution_hint);
				auto cached_it = hint_pid_cache.find(hint_key);
				if (cached_it != hint_pid_cache.end())
				{
					std::string cached_mode;
					if (bh_agent_dte::get_debug_mode_for_pid(cached_it->second, cached_mode, dte_error))
					{
						process_id = cached_it->second;
					}
					else
					{
						hint_pid_cache.erase(cached_it);
					}
				}

				const unsigned long long lookup_start = now_millis();
				while (process_id == 0 && (now_millis() - lookup_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					if (bh_agent_dte::get_instance_pid_for_solution_hint(solution_hint, process_id, dte_error) && process_id != 0)
					{
						hint_pid_cache[hint_key] = process_id;
						break;
					}

					process_id = 0;
					Sleep(250);
				}

				if (process_id == 0)
				{
					const std::string response = build_error_response(id, "target_not_found", "Timed out waiting for Visual Studio instance for solution hint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string initial_mode;
				const bool has_initial_mode = bh_agent_dte::get_debug_mode_for_pid(process_id, initial_mode, dte_error);
				if (has_launch_arguments && (!has_initial_mode || initial_mode != "design"))
				{
					const std::string detail = has_initial_mode
						? "cannot apply launch arguments while debugger is in " + initial_mode + " mode"
						: "cannot verify debugger design mode: " + dte_error;
					const std::string response = build_error_response(id, "already_debugging", detail);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string encoded_launch_arguments;
				if (has_launch_arguments && !bh_startup_arguments::try_encode(launch_arguments, encoded_launch_arguments))
				{
					const std::string response = build_error_response(id, "invalid_request", "arguments must be valid UTF-8 without NUL and no larger than 16384 bytes");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string target_id = build_target_id_from_pid(process_id);
				const std::string package_pipe = build_default_pipe_for_target_id(target_id);
				upsert_target(targets, target_id, package_pipe, false);
				const std::string target_entry = build_target_list_entry_text(targets, target_id);

				bool dte_start_attempted = false;
				bool dte_retry_attempted = false;
				bool started_via_dte = false;
				std::string dte_start_route;
				if (has_initial_mode &&
					(initial_mode == "run" || initial_mode == "break"))
				{
					started_via_dte = true;
					dte_start_route = "via=dte-already";
				}

				else if (!has_launch_arguments && bh_agent_dte::execute_command_for_pid(process_id, "Debug.Start", dte_error, timeout_ms))
				{
					dte_start_attempted = true;
					bool running_fallback = false;
					const unsigned long long mode_start = now_millis();
					while ((now_millis() - mode_start) <= static_cast<unsigned long long>(timeout_ms))
					{
						std::string mode;
						if (bh_agent_dte::get_debug_mode_for_pid(process_id, mode, dte_error) && (mode == "run" || mode == "break"))
						{
							running_fallback = true;
							break;
						}

						Sleep(250);
					}

					if (running_fallback)
					{
						started_via_dte = true;
						dte_start_route = "via=dte";
					}

					if (!started_via_dte)
					{
						dte_retry_attempted = true;
						std::string retry_error;
						(void)bh_agent_dte::execute_command_for_pid(process_id, "Debug.StopDebugging", retry_error, timeout_ms);

						const unsigned long long design_start = now_millis();
						while ((now_millis() - design_start) <= static_cast<unsigned long long>(timeout_ms / 2U))
						{
							std::string mode;
							if (bh_agent_dte::get_debug_mode_for_pid(process_id, mode, retry_error) && mode == "design")
							{
								break;
							}

							Sleep(200);
						}

						if (bh_agent_dte::execute_command_for_pid(process_id, "Debug.Start", retry_error, timeout_ms))
						{
							dte_start_attempted = true;
							bool retry_running = false;
							const unsigned long long retry_mode_start = now_millis();
							while ((now_millis() - retry_mode_start) <= static_cast<unsigned long long>(timeout_ms))
							{
								std::string mode;
								if (bh_agent_dte::get_debug_mode_for_pid(process_id, mode, retry_error) && (mode == "run" || mode == "break"))
								{
									retry_running = true;
									break;
								}

								Sleep(250);
							}

							if (retry_running)
							{
								started_via_dte = true;
								dte_start_route = "via=dte-retry";
							}
						}
					}
				}

				if (!wait_for_package_endpoint(package_pipe, timeout_ms))
				{
					if (started_via_dte)
					{
						const std::string response = build_error_response(id, "transport_error", "package endpoint not ready");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					if (dte_start_attempted)
					{
						std::string build_status;
						std::string build_error;
						std::string message = "debugger did not enter run/break mode";
						if (dte_retry_attempted)
						{
							message += " | dteRetry=true";
						}
						if (bh_agent_dte::get_build_status_for_pid(process_id, build_status, build_error))
						{
							message += " | " + build_status;
						}
						else if (!build_error.empty())
						{
							message += " | buildStatusError=" + build_error;
						}

						const std::string response = build_error_response(id, "backend_error", message);
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					const std::string response = build_error_response(id, "transport_error", "package endpoint not ready");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (started_via_dte)
				{
					std::string message = "debug.startHint ok endpointReady=true " + dte_start_route;
					if (!target_entry.empty())
					{
						message += " | " + target_entry;
					}
					const std::string response = build_ok_response(id, message);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string package_request = has_launch_arguments
					? std::string(bh_startup_arguments::command_prefix) + encoded_launch_arguments
					: "start debugging";
				std::string package_response;
				if (!send_request(std::wstring(package_pipe.begin(), package_pipe.end()), package_request, package_response, timeout_ms))
				{
					const std::string response = build_error_response(id, "transport_error", "failed to reach package endpoint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (has_launch_arguments && package_response.rfind("ok:", 0) != 0)
				{
					const std::string response = build_error_response(id, "backend_error", "transient launch failed: " + package_response);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				bool running = false;
				std::string last_status_response;
				const unsigned long long status_start = now_millis();
				while ((now_millis() - status_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					std::string status_response;
					if (send_request(std::wstring(package_pipe.begin(), package_pipe.end()), "status", status_response, 1000))
					{
						last_status_response = status_response;
						if (status_response.find("vs run mode") != std::string::npos || status_response.find("vs break mode") != std::string::npos)
						{
							running = true;
							break;
						}
					}

					Sleep(250);
				}

				if (!running)
				{
					std::string message = "debugger did not enter run/break mode";
					std::string build_status;
					std::string build_error;
					if (bh_agent_dte::get_build_status_for_pid(process_id, build_status, build_error))
					{
						message += " | " + build_status;
					}
					else if (!build_error.empty())
					{
						message += " | buildStatusError=" + build_error;
					}
					if (!package_response.empty())
					{
						message += " | startResponse=" + package_response;
					}
					if (!last_status_response.empty())
					{
						message += " | lastStatus=" + last_status_response;
					}

					const std::string response = build_error_response(id, "backend_error", message);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string message = has_launch_arguments
					? "debug.startHint ok endpointReady=true via=transient"
					: "debug.startHint ok endpointReady=true";
				if (!target_entry.empty())
				{
					message += " | " + target_entry;
				}
				const std::string response = build_ok_response(id, message);
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.attachPid")
			{
				unsigned int timeout_ms = 15000;
				(void)extract_json_uint(request, "timeoutMs", timeout_ms);
				if (timeout_ms < 1000) timeout_ms = 1000;

				std::string solution_hint;
				unsigned int target_process_id = 0;
				if (!extract_json_string(request, "solutionHint", solution_hint) || solution_hint.empty() ||
					!extract_json_uint(request, "processId", target_process_id) || target_process_id == 0)
				{
					const std::string response = build_error_response(id, "invalid_request", "solutionHint and processId are required");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string dte_error;
				unsigned int process_id = 0;
				const unsigned long long lookup_start = now_millis();
				while (process_id == 0 && (now_millis() - lookup_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					if (bh_agent_dte::get_instance_pid_for_solution_hint(solution_hint, process_id, dte_error) && process_id != 0) break;
					process_id = 0;
					Sleep(250);
				}

				if (process_id == 0)
				{
					const std::string response = build_error_response(id, "target_not_found", "Timed out waiting for Visual Studio instance for solution hint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!bh_agent_dte::attach_process_for_pid(process_id, target_process_id, dte_error, timeout_ms))
				{
					const std::string response = build_error_response(id, "backend_error", "attach failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				bool attached = false;
				const unsigned long long mode_start = now_millis();
				while ((now_millis() - mode_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					std::string mode;
					if (bh_agent_dte::get_debug_mode_for_pid(process_id, mode, dte_error) && (mode == "run" || mode == "break"))
					{
						attached = true;
						break;
					}
					Sleep(250);
				}

				if (!attached)
				{
					const std::string response = build_error_response(id, "backend_error", "debugger did not enter run/break mode after attach");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string target_id = build_target_id_from_pid(process_id);
				const std::string package_pipe = build_default_pipe_for_target_id(target_id);
				upsert_target(targets, target_id, package_pipe, false);
				if (!wait_for_package_endpoint(package_pipe, timeout_ms))
				{
					const std::string response = build_error_response(id, "transport_error", "package endpoint not ready after attach");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string message = "debug.attachPid ok processId=" + std::to_string(target_process_id) + " endpointReady=true";
				const std::string target_entry = build_target_list_entry_text(targets, target_id);
				if (!target_entry.empty()) message += " | " + target_entry;
				const std::string response = build_ok_response(id, message);
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.stopHint")
			{
				unsigned int timeout_ms = 15000;
				(void)extract_json_uint(request, "timeoutMs", timeout_ms);
				if (timeout_ms < 1000)
				{
					timeout_ms = 1000;
				}

				std::string solution_hint;
				if (!extract_json_string(request, "solutionHint", solution_hint) || solution_hint.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing solutionHint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string dte_error;
				unsigned int process_id = 0;
				const unsigned long long lookup_start = now_millis();
				while ((now_millis() - lookup_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					if (bh_agent_dte::get_instance_pid_for_solution_hint(solution_hint, process_id, dte_error) && process_id != 0)
					{
						break;
					}

					process_id = 0;
					Sleep(250);
				}

				if (process_id == 0)
				{
					const std::string response = build_error_response(id, "target_not_found", "Timed out waiting for Visual Studio instance for solution hint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string initial_mode;
				if (bh_agent_dte::get_debug_mode_for_pid(process_id, initial_mode, dte_error) && initial_mode == "design")
				{
					bh_log::write("service.dte", "debug.stopHint no-op pid=" + std::to_string(process_id) + " already in design mode");
					const std::string response = build_ok_response(id, "debug.stopHint ok alreadyStopped=true via=dte");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string target_id = build_target_id_from_pid(process_id);
				const std::string package_pipe = build_default_pipe_for_target_id(target_id);
				upsert_target(targets, target_id, package_pipe, false);

				if (bh_agent_dte::execute_command_for_pid(process_id, "Debug.StopDebugging", dte_error, timeout_ms))
				{
					bool stopped_fallback = false;
					const unsigned long long mode_start = now_millis();
					while ((now_millis() - mode_start) <= static_cast<unsigned long long>(timeout_ms))
					{
						std::string mode;
						if (bh_agent_dte::get_debug_mode_for_pid(process_id, mode, dte_error) && mode == "design")
						{
							stopped_fallback = true;
							break;
						}

						Sleep(250);
					}

					if (stopped_fallback)
					{
						const std::string response = build_ok_response(id, "debug.stopHint ok endpointReady=false via=dte");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}
				}

				if (!wait_for_package_endpoint(package_pipe, timeout_ms))
				{
					const std::string response = build_error_response(id, "transport_error", "package endpoint not ready");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string package_response;
				if (!send_request(std::wstring(package_pipe.begin(), package_pipe.end()), "stop debugging", package_response, timeout_ms))
				{
					const std::string response = build_error_response(id, "transport_error", "failed to reach package endpoint");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (package_response.rfind("error:", 0) == 0)
				{
					const std::string response = build_error_response(id, "backend_error", package_response);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				bool stopped = false;
				std::string last_status_response;
				const unsigned long long status_start = now_millis();
				while ((now_millis() - status_start) <= static_cast<unsigned long long>(timeout_ms))
				{
					std::string status_response;
					if (send_request(std::wstring(package_pipe.begin(), package_pipe.end()), "status", status_response, 1000))
					{
						last_status_response = status_response;
						if (status_response.find("vs design mode") != std::string::npos)
						{
							stopped = true;
							break;
						}
					}

					Sleep(250);
				}

				if (!stopped)
				{
					std::string message = "debugger did not return to design mode";
					if (!package_response.empty())
					{
						message += " | stopResponse=" + package_response;
					}
					if (!last_status_response.empty())
					{
						message += " | lastStatus=" + last_status_response;
					}

					const std::string response = build_error_response(id, "backend_error", message);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string response = build_ok_response(id, "debug.stopHint ok");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.startMain")
			{
				unsigned int readiness_timeout_ms = 60000;
				(void)extract_json_uint(request, "timeoutMs", readiness_timeout_ms);
				if (readiness_timeout_ms < 15000)
				{
					readiness_timeout_ms = 15000;
				}

				std::string solution_path;
				(void)extract_json_string(request, "solution", solution_path);

				bool exists = false;
				std::string dte_error;
				if (!bh_agent_dte::has_instance_for_suffix("", exists, dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "dte check failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string pre_mode;
				std::string inferred_target_id_main;
				std::string inferred_pipe_main;
				std::string infer_error_main;
				if (try_infer_target_for_suffix("", inferred_target_id_main, inferred_pipe_main, infer_error_main))
				{
					upsert_target(targets, inferred_target_id_main, inferred_pipe_main, false);
				}

				if (bh_agent_dte::get_debug_mode_for_suffix("", pre_mode, dte_error) && (pre_mode == "run" || pre_mode == "break"))
				{
					std::string package_pipe_ready = inferred_pipe_main;
					if (package_pipe_ready.empty() && !inferred_target_id_main.empty())
					{
						const int route_main = find_target_index_by_id(targets, inferred_target_id_main);
						if (route_main >= 0)
						{
							package_pipe_ready = targets[static_cast<size_t>(route_main)].pipe_name;
						}
					}

					if (package_pipe_ready.empty())
					{
						const int single_non_static = find_single_non_static_target_index(targets);
						if (single_non_static >= 0)
						{
							package_pipe_ready = targets[static_cast<size_t>(single_non_static)].pipe_name;
						}
						else if (!targets.empty())
						{
							package_pipe_ready = targets[0].pipe_name;
						}
					}

					if (!wait_for_package_endpoint(package_pipe_ready, readiness_timeout_ms))
					{
						const std::string response = build_ok_response(id, "debug.startMain already running endpointReady=false");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					const std::string response = build_ok_response(id, "debug.startMain already running endpointReady=true");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!exists)
				{
					if (!bh_agent_dte::launch_devenv(solution_path, "", dte_error))
					{
						const std::string response = build_error_response(id, "backend_error", "launch failed: " + dte_error);
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					Sleep(2000);
				}

				if (!bh_agent_dte::execute_command_for_suffix("", "Debug.Start", dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "Debug.Start failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string mode;
				bool running = false;
				for (int i = 0; i < 60; ++i)
				{
					if (bh_agent_dte::get_debug_mode_for_suffix("", mode, dte_error) && (mode == "run" || mode == "break"))
					{
						running = true;
						break;
					}

					Sleep(250);
				}

				if (!running)
				{
					const std::string response = build_error_response(id, "backend_error", "debugger did not enter run/break mode");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (inferred_pipe_main.empty())
				{
					std::string infer_error_after_start;
					if (try_infer_target_for_suffix("", inferred_target_id_main, inferred_pipe_main, infer_error_after_start))
					{
						upsert_target(targets, inferred_target_id_main, inferred_pipe_main, false);
					}
				}

				std::string package_pipe = inferred_pipe_main;
				if (package_pipe.empty() && !inferred_target_id_main.empty())
				{
					const int route_main = find_target_index_by_id(targets, inferred_target_id_main);
					if (route_main >= 0)
					{
						package_pipe = targets[static_cast<size_t>(route_main)].pipe_name;
					}
				}

				if (package_pipe.empty())
				{
					const int single_non_static = find_single_non_static_target_index(targets);
					if (single_non_static >= 0)
					{
						package_pipe = targets[static_cast<size_t>(single_non_static)].pipe_name;
					}
					else if (!targets.empty())
					{
						package_pipe = targets[0].pipe_name;
					}
				}

				const bool endpoint_ready = wait_for_package_endpoint(package_pipe, readiness_timeout_ms);
				const std::string response = build_ok_response(
					id,
					endpoint_ready ? "debug.startMain ok endpointReady=true" : "debug.startMain ok endpointReady=false");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.stopMain")
			{
				std::string mode;
				std::string dte_error;
				if (bh_agent_dte::get_debug_mode_for_suffix("", mode, dte_error) && mode == "design")
				{
					const std::string response = build_ok_response(id, "debug.stopMain already design");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!bh_agent_dte::execute_command_for_suffix("", "Debug.StopDebugging", dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "Debug.StopDebugging failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				bool stopped = false;
				for (int i = 0; i < 60; ++i)
				{
					if (bh_agent_dte::get_debug_mode_for_suffix("", mode, dte_error) && mode == "design")
					{
						stopped = true;
						break;
					}

					Sleep(250);
				}

				if (!stopped)
				{
					const std::string response = build_error_response(id, "backend_error", "debugger did not return to design mode");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string response = build_ok_response(id, "debug.stopMain ok");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.startExp")
			{
				unsigned int readiness_timeout_ms = 60000;
				(void)extract_json_uint(request, "timeoutMs", readiness_timeout_ms);
				if (readiness_timeout_ms < 15000)
				{
					readiness_timeout_ms = 15000;
				}

				std::string suffix;
				if (!extract_json_string(request, "suffix", suffix) || suffix.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing suffix");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string solution_path;
				(void)extract_json_string(request, "solution", solution_path);

				bool force_launch = false;
				(void)extract_json_bool(request, "forceLaunch", force_launch);

				bool exists = false;
				std::string dte_error;
				if (!bh_agent_dte::has_instance_for_suffix(suffix, exists, dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "dte check failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!exists)
				{
					if (!force_launch)
					{
						const std::string response = build_error_response(id, "target_not_found", "experimental instance not running (use forceLaunch)");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					if (!bh_agent_dte::launch_devenv(solution_path, suffix, dte_error))
					{
						const std::string response = build_error_response(id, "backend_error", "launch failed: " + dte_error);
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					Sleep(2500);
				}

				std::string inferred_target_id_exp;
				std::string inferred_pipe_exp;
				std::string infer_error_exp;
				if (try_infer_target_for_suffix(suffix, inferred_target_id_exp, inferred_pipe_exp, infer_error_exp))
				{
					upsert_target(targets, inferred_target_id_exp, inferred_pipe_exp, false);
				}

				std::string pre_mode;
				if (bh_agent_dte::get_debug_mode_for_suffix(suffix, pre_mode, dte_error) && (pre_mode == "run" || pre_mode == "break"))
				{
					std::string package_pipe_ready = inferred_pipe_exp;
					if (package_pipe_ready.empty() && !inferred_target_id_exp.empty())
					{
						const int route_index_ready = find_target_index_by_id(targets, inferred_target_id_exp);
						if (route_index_ready >= 0)
						{
							package_pipe_ready = targets[static_cast<size_t>(route_index_ready)].pipe_name;
						}
					}

					if (package_pipe_ready.empty())
					{
						const int legacy_route = find_target_index_by_id(targets, suffix);
						if (legacy_route >= 0)
						{
							package_pipe_ready = targets[static_cast<size_t>(legacy_route)].pipe_name;
						}
					}

					if (package_pipe_ready.empty())
					{
						const int single_non_static = find_single_non_static_target_index(targets);
						if (single_non_static >= 0)
						{
							package_pipe_ready = targets[static_cast<size_t>(single_non_static)].pipe_name;
						}
						else if (!targets.empty())
						{
							package_pipe_ready = targets[0].pipe_name;
						}
					}

					if (!wait_for_package_endpoint(package_pipe_ready, readiness_timeout_ms))
					{
						const std::string response = build_ok_response(id, "debug.startExp already running endpointReady=false");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					const std::string response = build_ok_response(id, "debug.startExp already running endpointReady=true");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!bh_agent_dte::execute_command_for_suffix(suffix, "Debug.Start", dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "Debug.Start failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string mode;
				bool running = false;
				for (int i = 0; i < 80; ++i)
				{
					if (bh_agent_dte::get_debug_mode_for_suffix(suffix, mode, dte_error) && (mode == "run" || mode == "break"))
					{
						running = true;
						break;
					}

					Sleep(250);
				}

				if (!running)
				{
					const std::string response = build_error_response(id, "backend_error", "experimental debugger did not enter run/break mode");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (inferred_pipe_exp.empty())
				{
					std::string infer_error_after_start;
					if (try_infer_target_for_suffix(suffix, inferred_target_id_exp, inferred_pipe_exp, infer_error_after_start))
					{
						upsert_target(targets, inferred_target_id_exp, inferred_pipe_exp, false);
					}
				}

				std::string package_pipe = inferred_pipe_exp;
				if (package_pipe.empty() && !inferred_target_id_exp.empty())
				{
					const int route_index = find_target_index_by_id(targets, inferred_target_id_exp);
					if (route_index >= 0)
					{
						package_pipe = targets[static_cast<size_t>(route_index)].pipe_name;
					}
				}

				if (package_pipe.empty())
				{
					const int legacy_route = find_target_index_by_id(targets, suffix);
					if (legacy_route >= 0)
					{
						package_pipe = targets[static_cast<size_t>(legacy_route)].pipe_name;
					}
				}

				if (package_pipe.empty())
				{
					const int single_non_static = find_single_non_static_target_index(targets);
					if (single_non_static >= 0)
					{
						package_pipe = targets[static_cast<size_t>(single_non_static)].pipe_name;
					}
					else if (!targets.empty())
					{
						package_pipe = targets[0].pipe_name;
					}
				}

				const bool endpoint_ready = wait_for_package_endpoint(package_pipe, readiness_timeout_ms);
				const std::string response = build_ok_response(
					id,
					endpoint_ready ? "debug.startExp ok endpointReady=true" : "debug.startExp ok endpointReady=false");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "debug.stopExp")
			{
				std::string suffix;
				if (!extract_json_string(request, "suffix", suffix) || suffix.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing suffix");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string mode;
				std::string dte_error;
				if (bh_agent_dte::get_debug_mode_for_suffix(suffix, mode, dte_error) && mode == "design")
				{
					const std::string response = build_ok_response(id, "debug.stopExp already design");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				if (!bh_agent_dte::execute_command_for_suffix(suffix, "Debug.StopDebugging", dte_error))
				{
					const std::string response = build_error_response(id, "backend_error", "Debug.StopDebugging failed: " + dte_error);
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				bool stopped = false;
				for (int i = 0; i < 80; ++i)
				{
					if (bh_agent_dte::get_debug_mode_for_suffix(suffix, mode, dte_error) && mode == "design")
					{
						stopped = true;
						break;
					}

					Sleep(250);
				}

				if (!stopped)
				{
					const std::string response = build_error_response(id, "backend_error", "experimental debugger did not return to design mode");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const std::string response = build_ok_response(id, "debug.stopExp ok");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "dte.list")
			{
				const std::string response = build_ok_response(id, build_dte_list_text());
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "health.summary" || method == "diag.health")
			{
				std::string client_version;
				(void)extract_json_string(request, "clientVersion", client_version);
				const std::string response = build_ok_response(id, build_health_summary_text(targets, sanitize_version_text(client_version)));
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "targets.register")
			{
				std::string target_id;
				if (!extract_json_string(request, "targetId", target_id) || target_id.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing targetId");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				std::string target_pipe;
				if (!extract_json_string(request, "pipeName", target_pipe) || target_pipe.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing pipeName");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const int index = find_target_index_by_id(targets, target_id);
				std::string package_version;
				(void)extract_json_string(request, "packageVersion", package_version);
				package_version = sanitize_version_text(package_version);
				bool changed = false;
				if (index >= 0)
				{
					changed = (targets[static_cast<size_t>(index)].pipe_name != target_pipe) ||
							  (targets[static_cast<size_t>(index)].package_version != package_version);
					targets[static_cast<size_t>(index)].package_version = package_version;
					targets[static_cast<size_t>(index)].pipe_name = target_pipe;
					targets[static_cast<size_t>(index)].last_seen_ms = now_millis();
				}
				else
				{
					targets.push_back(target_endpoint{target_id, target_pipe, false, now_millis(), package_version});
					changed = true;
				}

				if (changed)
				{
					log_event("target register id='" + target_id + "' pipe='" + target_pipe + "' vsix=" + version_or_unreported(package_version));
				}

				const std::string response = build_ok_response(id, "target registered");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "targets.unregister")
			{
				std::string target_id;
				if (!extract_json_string(request, "targetId", target_id) || target_id.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing targetId");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const size_t before = targets.size();
				targets.erase(
					std::remove_if(
						targets.begin(),
						targets.end(),
						[&](const target_endpoint &entry) {
							return (!entry.is_static && entry.target_id == target_id);
						}),
					targets.end());

				const bool removed = (targets.size() < before);
				log_event("target unregister id='" + target_id + "' removed=" + std::string(removed ? "true" : "false"));
				const std::string response = removed
												 ? build_ok_response(id, "target unregistered")
												 : build_error_response(id, "target_not_found", "targetId not found");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "targets.heartbeat")
			{
				std::string target_id;
				if (!extract_json_string(request, "targetId", target_id) || target_id.empty())
				{
					const std::string response = build_error_response(id, "invalid_request", "missing targetId");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				const int index = find_target_index_by_id(targets, target_id);
				if (index < 0)
				{
					const std::string response = build_error_response(id, "target_not_found", "targetId not found");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				targets[static_cast<size_t>(index)].last_seen_ms = now_millis();
				const std::string response = build_ok_response(id, "heartbeat ok");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method == "targets.probe")
			{
				std::string target_pipe;
				std::string target_id;
				(void)extract_json_string(request, "targetId", target_id);
				if (!target_id.empty())
				{
					const int index = find_target_index_by_id(targets, target_id);
					if (index < 0)
					{
						const std::string response = build_error_response(id, "target_not_found", "targetId not found");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}

					target_pipe = targets[static_cast<size_t>(index)].pipe_name;
				}
				else
				{
					(void)extract_json_string(request, "pipeName", target_pipe);
					if (target_pipe.empty())
					{
						const int single_non_static = find_single_non_static_target_index(targets);
						if (single_non_static >= 0)
						{
							target_pipe = targets[static_cast<size_t>(single_non_static)].pipe_name;
						}
						else if (targets.empty())
						{
							const std::string response = build_error_response(id, "target_not_found", "no targets available");
							(void)write_line(pipe_handle, response);
							FlushFileBuffers(pipe_handle);
							DisconnectNamedPipe(pipe_handle);
							CloseHandle(pipe_handle);
							continue;
						}
						else
						{
							const std::string response = build_error_response(id, "ambiguous_target", "multiple targets available; specify targetId");
							(void)write_line(pipe_handle, response);
							FlushFileBuffers(pipe_handle);
							DisconnectNamedPipe(pipe_handle);
							CloseHandle(pipe_handle);
							continue;
						}
					}
				}

				const std::string probe = probe_package_endpoint(target_pipe);
				const std::string response = build_ok_response(id, probe);
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			if (method != "send")
			{
				const std::string response = build_error_response(id, "unsupported_method", "supported methods: profile.command, send, targets.list, targets.register, targets.unregister, targets.heartbeat, targets.probe, dte.list, health.summary, debug.startHint, debug.attachPid, debug.stopHint, debug.startMain, debug.stopMain, debug.startExp, debug.stopExp");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			std::string message;
			if (!extract_json_string(request, "message", message))
			{
				const std::string response = build_error_response(id, "invalid_request", "missing params.message");
				(void)write_line(pipe_handle, response);
				FlushFileBuffers(pipe_handle);
				DisconnectNamedPipe(pipe_handle);
				CloseHandle(pipe_handle);
				continue;
			}

			std::string target_pipe_utf8;
			std::string target_id;
			(void)extract_json_string(request, "targetId", target_id);
			if (!target_id.empty())
			{
				const int index = find_target_index_by_id(targets, target_id);
				if (index < 0)
				{
					const std::string response = build_error_response(id, "target_not_found", "targetId not found");
					(void)write_line(pipe_handle, response);
					FlushFileBuffers(pipe_handle);
					DisconnectNamedPipe(pipe_handle);
					CloseHandle(pipe_handle);
					continue;
				}

				target_pipe_utf8 = targets[static_cast<size_t>(index)].pipe_name;
				targets[static_cast<size_t>(index)].last_seen_ms = now_millis();
			}
			else
			{
				if (!extract_json_string(request, "pipeName", target_pipe_utf8) || target_pipe_utf8.empty())
				{
					const int single_non_static = find_single_non_static_target_index(targets);
					if (single_non_static >= 0)
					{
						target_pipe_utf8 = targets[static_cast<size_t>(single_non_static)].pipe_name;
						targets[static_cast<size_t>(single_non_static)].last_seen_ms = now_millis();
					}
					else if (targets.empty())
					{
						const std::string response = build_error_response(id, "target_not_found", "no targets available");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}
					else
					{
						const std::string response = build_error_response(id, "ambiguous_target", "multiple targets available; specify targetId");
						(void)write_line(pipe_handle, response);
						FlushFileBuffers(pipe_handle);
						DisconnectNamedPipe(pipe_handle);
						CloseHandle(pipe_handle);
						continue;
					}
				}
			}

			unsigned int timeout_ms = 5000;
			(void)extract_json_uint(request, "timeoutMs", timeout_ms);

			std::string backend_response;
			DWORD transport_error = ERROR_SUCCESS;
			package_request_failure failure = package_request_failure::none;
			const bool sent = send_request(
				std::wstring(target_pipe_utf8.begin(), target_pipe_utf8.end()),
				message,
				backend_response,
				timeout_ms,
				transport_error,
				failure);

			std::string response;
			if (!sent)
			{
				log_event("send failed id='" + id + "' targetPipe='" + target_pipe_utf8 + "' err=" + std::to_string(transport_error));
				if (failure == package_request_failure::timeout)
				{
					response = build_error_response(id, "timeout", "package endpoint request timed out");
				}
				else
				{
					response = build_error_response(id, "transport_error", "failed to reach package endpoint");
				}

				if (is_stale_pipe_error(transport_error))
				{
					const int stale_index = find_target_index_by_pipe_name(targets, target_pipe_utf8);
					if (stale_index >= 0 && !targets[static_cast<size_t>(stale_index)].is_static)
					{
						log_event("target removed stale id='" + targets[static_cast<size_t>(stale_index)].target_id + "' pipe='" + target_pipe_utf8 + "'");
						targets.erase(targets.begin() + stale_index);
					}
				}
			}
			else
			{
				log_event("send ok id='" + id + "' targetPipe='" + target_pipe_utf8 + "'");
				if (backend_response.rfind("error:", 0) == 0)
				{
					response = build_error_response(id, "backend_error", backend_response);
				}
				else
				{
					response = build_ok_response(id, backend_response);
				}
			}

			(void)write_line(pipe_handle, response);
		}

		FlushFileBuffers(pipe_handle);
		DisconnectNamedPipe(pipe_handle);
		CloseHandle(pipe_handle);
	}

	return false;
}

bool send_request(
	const std::wstring &pipe_name,
	const std::string &request,
	std::string &response,
	unsigned int connect_timeout_ms,
	DWORD &last_error,
	package_request_failure &failure)
{
	last_error = ERROR_SUCCESS;
	failure = package_request_failure::none;
	const std::wstring pipe_path = make_pipe_path(pipe_name);
	const unsigned int attempts = 3;
	const unsigned int wait_each = (connect_timeout_ms < attempts) ? 1U : (connect_timeout_ms / attempts);

	for (unsigned int attempt = 0; attempt < attempts; ++attempt)
	{
		if (!WaitNamedPipeW(pipe_path.c_str(), wait_each))
		{
			last_error = GetLastError();
			if (is_broken_pipe_error(last_error))
			{
				failure = package_request_failure::broken_pipe;
				return false;
			}
			Sleep(50);
			continue;
		}

		HANDLE handle = CreateFileW(
			pipe_path.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,
			nullptr);

		if (handle == INVALID_HANDLE_VALUE)
		{
			last_error = GetLastError();
			if (is_broken_pipe_error(last_error))
			{
				failure = package_request_failure::broken_pipe;
				return false;
			}
			Sleep(50);
			continue;
		}

		if (!bh_agent_pipe_security::server_owned_by_current_user(handle))
		{
			CloseHandle(handle);
			last_error = ERROR_ACCESS_DENIED;
			failure = package_request_failure::transport;
			log_event("package pipe rejected: not owned by current user pipe='" + narrow_ascii(pipe_name) + "'");
			return false;
		}

		bool timed_out = false;
		bool ok = write_line_with_timeout(handle, request, connect_timeout_ms, last_error, timed_out);
		if (!ok)
		{
			if (timed_out)
			{
				failure = package_request_failure::timeout;
			}
			else if (is_broken_pipe_error(last_error))
			{
				failure = package_request_failure::broken_pipe;
			}
		}

		if (ok)
		{
			timed_out = false;
			ok = read_line_with_timeout(handle, response, connect_timeout_ms, last_error, timed_out);
			if (!ok)
			{
				if (timed_out)
				{
					failure = package_request_failure::timeout;
				}
				else if (is_broken_pipe_error(last_error))
				{
					failure = package_request_failure::broken_pipe;
				}
			}
		}

		CloseHandle(handle);
		if (ok)
		{
			return true;
		}

		if (failure == package_request_failure::timeout ||
			failure == package_request_failure::broken_pipe)
		{
			return false;
		}

		Sleep(50);
	}

	if (failure == package_request_failure::none)
	{
		failure = package_request_failure::transport;
	}

	return false;
}

bool send_request(
	const std::wstring &pipe_name,
	const std::string &request,
	std::string &response,
	unsigned int connect_timeout_ms)
{
	DWORD ignored_error = ERROR_SUCCESS;
	package_request_failure ignored_failure = package_request_failure::none;
	return send_request(
		pipe_name,
		request,
		response,
		connect_timeout_ms,
		ignored_error,
		ignored_failure);
}
} // namespace bh_agent_transport
