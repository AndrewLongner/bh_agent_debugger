#include "named_pipe_transport.h"
#include "bh_log.h"

#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <vector>

namespace
{
std::wstring to_wide_ascii(const std::string &text)
{
	return std::wstring(text.begin(), text.end());
}

bool read_option_value(int argc, char *argv[], const std::string &key, std::string &value)
{
	for (int i = 1; i < argc - 1; ++i)
	{
		if (key == argv[i])
		{
			value = argv[i + 1];
			return true;
		}
	}

	return false;
}

std::string narrow_utf8(const std::wstring &text)
{
	if (text.empty())
	{
		return "";
	}

	const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
	{
		return "";
	}

	std::string out;
	out.resize(static_cast<size_t>(size));
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
	return out;
}

std::wstring get_singleton_mutex_name(int argc, char *argv[])
{
	std::string pipe_name = "bh_agent_ctl";
	std::string pipe_arg;
	if (argc > 0 && argv != nullptr && read_option_value(argc, argv, "--pipe", pipe_arg) && !pipe_arg.empty())
	{
		pipe_name = pipe_arg;
	}
	if (pipe_name == "bh_agent_ctl")
	{
		return L"Global\\bh_agent_debugger_service_singleton";
	}

	for (char &ch : pipe_name)
	{
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
		{
			ch = '_';
		}
	}

	return L"Global\\bh_agent_debugger_service_singleton_" + to_wide_ascii(pipe_name);
}

void print_usage()
{
	std::cout
		<< "Usage:\n"
		<< "  bh_agent_debugger_service [--pipe <name>] [--target-pipe <name>]\n"
		<< "  bh_agent_debugger_service agentd serve [--pipe <name>] [--target-pipe <name>]\n"
		<< "\n"
		<< "Default pipe: bh_agent_ctl\n"
		<< "Default target pipe: bh_agent_debugger_agent\n";
}

int run_app(int argc, char *argv[])
{
	std::string pipe_name = "bh_agent_ctl";
	std::string target_pipe_name = "bh_agent_debugger_agent";

	std::string pipe_arg;
	if (read_option_value(argc, argv, "--pipe", pipe_arg) && !pipe_arg.empty())
	{
		pipe_name = pipe_arg;
	}

	std::string target_pipe_arg;
	if (read_option_value(argc, argv, "--target-pipe", target_pipe_arg) && !target_pipe_arg.empty())
	{
		target_pipe_name = target_pipe_arg;
	}

	if (argc == 1 || (argc > 1 && argv != nullptr && argv[1] != nullptr && argv[1][0] == '-'))
	{
		bh_log::write("service", "starting service loop pipe='" + pipe_name + "' targetPipe='" + target_pipe_name + "'");
		return bh_agent_transport::run_service_loop(to_wide_ascii(pipe_name), to_wide_ascii(target_pipe_name)) ? 0 : 1;
	}

	if (argc >= 3)
	{
		const std::string mode = argv[1];
		const std::string command = argv[2];
		if (mode == "agentd" && command == "serve")
		{
			bh_log::write("service", "starting service loop pipe='" + pipe_name + "' targetPipe='" + target_pipe_name + "'");
			return bh_agent_transport::run_service_loop(to_wide_ascii(pipe_name), to_wide_ascii(target_pipe_name)) ? 0 : 1;
		}
	}

	print_usage();
	return 1;
}

int run_singleton_app(int argc, char *argv[])
{
	const std::wstring singleton_mutex_name = get_singleton_mutex_name(argc, argv);
	HANDLE singleton_mutex = CreateMutexW(nullptr, FALSE, singleton_mutex_name.c_str());
	if (singleton_mutex == nullptr)
	{
		bh_log::write("service", "singleton mutex create failed");
		return 1;
	}

	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		bh_log::write("service", "singleton mutex already exists, exiting duplicate instance");
		CloseHandle(singleton_mutex);
		return 0;
	}

	const int exit_code = run_app(argc, argv);
	CloseHandle(singleton_mutex);
	return exit_code;
}
} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	int argc = 0;
	LPWSTR *argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv_w || argc <= 0)
	{
		return run_singleton_app(0, nullptr);
	}

	std::vector<std::string> utf8_args;
	utf8_args.reserve(static_cast<size_t>(argc));
	std::vector<char *> argv_a;
	argv_a.reserve(static_cast<size_t>(argc));

	for (int i = 0; i < argc; ++i)
	{
		utf8_args.push_back(narrow_utf8(argv_w[i]));
	}

	for (int i = 0; i < argc; ++i)
	{
		argv_a.push_back(const_cast<char *>(utf8_args[static_cast<size_t>(i)].c_str()));
	}

	LocalFree(argv_w);
	return run_singleton_app(argc, argv_a.data());
}
