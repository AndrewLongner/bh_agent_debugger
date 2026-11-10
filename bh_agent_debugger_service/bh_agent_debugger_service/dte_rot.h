#pragma once

#include <string>
#include <vector>

namespace bh_agent_dte
{
    struct dte_instance_info
    {
        std::string moniker_name;
        std::string registry_root;
        std::string solution_path;
        std::string debug_mode;
        unsigned int process_id;
    };

    struct startup_project_arguments
    {
        std::string project_unique_name;
        std::string configuration_identity;
        std::string arguments;
        std::string user_file_path;
        std::string user_file_contents;
        bool user_file_existed = false;
    };

    bool enumerate_dte_instances(std::vector<dte_instance_info>& instances, std::string& error_text);
    bool has_instance_for_suffix(const std::string& suffix, bool& exists, std::string& error_text);
    bool get_instance_pid_for_suffix(const std::string& suffix, unsigned int& process_id, std::string& error_text);
    bool get_instance_pid_for_solution_hint(const std::string& solution_hint, unsigned int& process_id, std::string& error_text);
    bool launch_devenv(const std::string& solution_path, const std::string& suffix, std::string& error_text);
    bool execute_command_for_suffix(const std::string& suffix, const std::string& command, std::string& error_text, unsigned int timeout_ms = 15000);
    bool get_debug_mode_for_suffix(const std::string& suffix, std::string& mode, std::string& error_text);
    bool execute_command_for_pid(unsigned int process_id, const std::string& command, std::string& error_text, unsigned int timeout_ms = 15000);
    bool open_profile_report_for_pid(unsigned int process_id, const std::wstring& path, std::string& error_text);
    bool capture_startup_project_arguments_for_pid(unsigned int process_id, startup_project_arguments& state, std::string& error_text);
    bool set_startup_project_arguments_for_pid(unsigned int process_id, const startup_project_arguments& state, const std::string& arguments, std::string& error_text);
    bool restore_startup_project_user_file(const startup_project_arguments& state, std::string& error_text);
    bool attach_process_for_pid(unsigned int process_id, unsigned int target_process_id, std::string& error_text, unsigned int timeout_ms = 15000);
    bool wait_for_debugged_process_for_pid(unsigned int process_id, unsigned int timeout_ms, std::string& error_text);
    bool get_debug_mode_for_pid(unsigned int process_id, std::string& mode, std::string& error_text);
    bool get_build_status_for_pid(unsigned int process_id, std::string& status_text, std::string& error_text);
    bool try_set_default_startup_project_for_pid(unsigned int process_id, std::string& configured_path, std::string& error_text);
}
