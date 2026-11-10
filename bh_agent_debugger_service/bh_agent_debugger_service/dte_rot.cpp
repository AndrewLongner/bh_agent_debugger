#include "dte_rot.h"
#include "bh_log.h"

#include <windows.h>

#include <objbase.h>
#include <oleauto.h>

#include <cstring>
#include <string>
#include <vector>
#include <thread>

#include "../../bh_vs_installations.h"


namespace
{
    std::string narrow_utf8(const std::wstring& text)
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

        std::string out(static_cast<size_t>(size), '\0');
        (void)WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
        return out;
    }

    std::wstring widen_utf16(const std::string& text)
    {
        if (text.empty())
        {
            return L"";
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0)
        {
            return L"";
        }

        std::wstring out(static_cast<size_t>(size), L'\0');
        (void)MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
        return out;
    }

    bool get_dispid(IDispatch* dispatch, const wchar_t* name, DISPID& dispid)
    {
        LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
        const HRESULT hr = dispatch->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &dispid);
        return SUCCEEDED(hr);
    }

    bool invoke_propget(IDispatch* dispatch, const wchar_t* name, VARIANT& result)
    {
        VariantInit(&result);

        DISPID dispid = DISPID_UNKNOWN;
        if (!get_dispid(dispatch, name, dispid))
        {
            return false;
        }

        DISPPARAMS params{};
        const HRESULT hr = dispatch->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_PROPERTYGET,
            &params,
            &result,
            nullptr,
            nullptr);

        return SUCCEEDED(hr);
    }

    bool get_dispatch_property(IDispatch* dispatch, const wchar_t* name, IDispatch** out_dispatch)
    {
        if (out_dispatch == nullptr)
        {
            return false;
        }

        *out_dispatch = nullptr;

        VARIANT value{};
        if (!invoke_propget(dispatch, name, value))
        {
            return false;
        }

        IDispatch* nested = nullptr;
        if (value.vt == VT_DISPATCH && value.pdispVal != nullptr)
        {
            nested = value.pdispVal;
            nested->AddRef();
        }
        else if (value.vt == VT_UNKNOWN && value.punkVal != nullptr)
        {
            (void)value.punkVal->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&nested));
        }

        VariantClear(&value);

        if (nested == nullptr)
        {
            return false;
        }

        *out_dispatch = nested;
        return true;
    }

    bool invoke_method(IDispatch* dispatch, const wchar_t* name, VARIANT* args, unsigned int arg_count, VARIANT* result)
    {
        DISPID dispid = DISPID_UNKNOWN;
        if (!get_dispid(dispatch, name, dispid))
        {
            return false;
        }

        DISPPARAMS params{};
        params.rgvarg = args;
        params.cArgs = arg_count;

        VARIANT local_result{};
        VariantInit(&local_result);
        VARIANT* target_result = (result == nullptr) ? &local_result : result;
        const HRESULT hr = dispatch->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_METHOD,
            &params,
            target_result,
            nullptr,
            nullptr);

        if (result == nullptr)
        {
            VariantClear(&local_result);
        }

        return SUCCEEDED(hr);
    }

    bool put_string_property(IDispatch* dispatch, const wchar_t* name, const std::string& value)
    {
        DISPID dispid = DISPID_UNKNOWN;
        if (dispatch == nullptr || !get_dispid(dispatch, name, dispid))
        {
            return false;
        }

        const std::wstring wide = widen_utf16(value);
        VARIANT argument{};
        VariantInit(&argument);
        argument.vt = VT_BSTR;
        argument.bstrVal = SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
        DISPID named_argument = DISPID_PROPERTYPUT;
        DISPPARAMS params{};
        params.rgvarg = &argument;
        params.rgdispidNamedArgs = &named_argument;
        params.cArgs = 1;
        params.cNamedArgs = 1;
        const HRESULT hr = dispatch->Invoke(
            dispid,
            IID_NULL,
            LOCALE_USER_DEFAULT,
            DISPATCH_PROPERTYPUT,
            &params,
            nullptr,
            nullptr,
            nullptr);
        VariantClear(&argument);
        return SUCCEEDED(hr);
    }

    bool get_string_property(IDispatch* dispatch, const wchar_t* name, std::string& out);

    bool get_collection_item(IDispatch* collection, const VARIANT& index, IDispatch** out_dispatch)
    {
        if (collection == nullptr || out_dispatch == nullptr)
        {
            return false;
        }

        *out_dispatch = nullptr;
        VARIANT argument{};
        VariantInit(&argument);
        if (FAILED(VariantCopy(&argument, const_cast<VARIANT*>(&index))))
        {
            return false;
        }

        VARIANT result{};
        VariantInit(&result);
        const bool invoked = invoke_method(collection, L"Item", &argument, 1, &result);
        VariantClear(&argument);
        if (!invoked)
        {
            VariantClear(&result);
            return false;
        }

        IDispatch* item = nullptr;
        if (result.vt == VT_DISPATCH && result.pdispVal != nullptr)
        {
            item = result.pdispVal;
            item->AddRef();
        }
        else if (result.vt == VT_UNKNOWN && result.punkVal != nullptr)
        {
            (void)result.punkVal->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&item));
        }

        VariantClear(&result);
        if (item == nullptr)
        {
            return false;
        }

        *out_dispatch = item;
        return true;
    }

    bool get_first_startup_project(IDispatch* solution, std::string& startup_project)
    {
        startup_project.clear();
        IDispatch* solution_build = nullptr;
        if (!get_dispatch_property(solution, L"SolutionBuild", &solution_build) || solution_build == nullptr)
        {
            return false;
        }

        VARIANT value{};
        const bool got_value = invoke_propget(solution_build, L"StartupProjects", value);
        solution_build->Release();
        if (!got_value)
        {
            return false;
        }

        bool ok = false;
        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
        {
            startup_project = narrow_utf8(std::wstring(value.bstrVal, SysStringLen(value.bstrVal)));
            ok = !startup_project.empty();
        }
        else if ((value.vt & VT_ARRAY) != 0 && value.parray != nullptr)
        {
            LONG lower = 0;
            LONG upper = -1;
            if (SUCCEEDED(SafeArrayGetLBound(value.parray, 1, &lower)) &&
                SUCCEEDED(SafeArrayGetUBound(value.parray, 1, &upper)) && lower <= upper)
            {
                const VARTYPE element_type = static_cast<VARTYPE>(value.vt & VT_TYPEMASK);
                if (element_type == VT_BSTR)
                {
                    BSTR item = nullptr;
                    if (SUCCEEDED(SafeArrayGetElement(value.parray, &lower, &item)) && item != nullptr)
                    {
                        startup_project = narrow_utf8(std::wstring(item, SysStringLen(item)));
                        SysFreeString(item);
                        ok = !startup_project.empty();
                    }
                }
                else if (element_type == VT_VARIANT)
                {
                    VARIANT item{};
                    VariantInit(&item);
                    if (SUCCEEDED(SafeArrayGetElement(value.parray, &lower, &item)) &&
                        item.vt == VT_BSTR && item.bstrVal != nullptr)
                    {
                        startup_project = narrow_utf8(std::wstring(item.bstrVal, SysStringLen(item.bstrVal)));
                        ok = !startup_project.empty();
                    }
                    VariantClear(&item);
                }
            }
        }

        VariantClear(&value);
        return ok;
    }

    bool get_project_for_unique_name(IDispatch* solution, const std::string& unique_name, IDispatch** project)
    {
        const std::wstring wide_name = widen_utf16(unique_name);
        VARIANT index{};
        VariantInit(&index);
        index.vt = VT_BSTR;
        index.bstrVal = SysAllocStringLen(wide_name.c_str(), static_cast<UINT>(wide_name.size()));
        bool ok = get_collection_item(solution, index, project);
        if (!ok)
        {
            IDispatch* projects = nullptr;
            if (get_dispatch_property(solution, L"Projects", &projects) && projects != nullptr)
            {
                ok = get_collection_item(projects, index, project);
                projects->Release();
            }
        }
        VariantClear(&index);
        return ok;
    }

    bool set_project_debug_arguments(IDispatch* project, const std::string& configuration_identity, const std::string& arguments)
    {
        if (configuration_identity.rfind("vc:", 0) == 0)
        {
            IDispatch* project_object = nullptr;
            IDispatch* configurations = nullptr;
            IDispatch* configuration = nullptr;
            IDispatch* debug_settings = nullptr;
            bool ok = false;
            if (get_dispatch_property(project, L"Object", &project_object) && project_object != nullptr &&
                get_dispatch_property(project_object, L"Configurations", &configurations) && configurations != nullptr)
            {
                const std::wstring configuration_name = widen_utf16(configuration_identity.substr(3));
                VARIANT index{};
                VariantInit(&index);
                index.vt = VT_BSTR;
                index.bstrVal = SysAllocStringLen(configuration_name.c_str(), static_cast<UINT>(configuration_name.size()));
                if (get_collection_item(configurations, index, &configuration) && configuration != nullptr &&
                    get_dispatch_property(configuration, L"DebugSettings", &debug_settings) && debug_settings != nullptr)
                {
                    ok = put_string_property(debug_settings, L"CommandArguments", arguments);
                }
                VariantClear(&index);
            }

            if (debug_settings != nullptr) debug_settings->Release();
            if (configuration != nullptr) configuration->Release();
            if (configurations != nullptr) configurations->Release();
            if (project_object != nullptr) project_object->Release();
            return ok;
        }

        if (configuration_identity.rfind("env:", 0) != 0) return false;
        const std::string configuration_and_platform = configuration_identity.substr(4);
        const size_t separator = configuration_and_platform.rfind('|');
        if (separator == std::string::npos) return false;

        IDispatch* configuration_manager = nullptr;
        IDispatch* configuration = nullptr;
        IDispatch* properties = nullptr;
        if (!get_dispatch_property(project, L"ConfigurationManager", &configuration_manager) || configuration_manager == nullptr)
        {
            return false;
        }

        const std::wstring configuration_name = widen_utf16(configuration_and_platform.substr(0, separator));
        const std::wstring platform_name = widen_utf16(configuration_and_platform.substr(separator + 1));
        VARIANT item_args[2]{};
        VariantInit(&item_args[0]);
        VariantInit(&item_args[1]);
        item_args[0].vt = VT_BSTR;
        item_args[0].bstrVal = SysAllocStringLen(platform_name.c_str(), static_cast<UINT>(platform_name.size()));
        item_args[1].vt = VT_BSTR;
        item_args[1].bstrVal = SysAllocStringLen(configuration_name.c_str(), static_cast<UINT>(configuration_name.size()));
        VARIANT item_result{};
        VariantInit(&item_result);
        if (invoke_method(configuration_manager, L"Item", item_args, 2, &item_result) && item_result.vt == VT_DISPATCH && item_result.pdispVal != nullptr)
        {
            configuration = item_result.pdispVal;
            configuration->AddRef();
        }
        VariantClear(&item_result);
        VariantClear(&item_args[0]);
        VariantClear(&item_args[1]);

        if (configuration == nullptr || !get_dispatch_property(configuration, L"Properties", &properties) || properties == nullptr)
        {
            if (configuration != nullptr) configuration->Release();
            configuration_manager->Release();
            return false;
        }

        const wchar_t* property_names[] = { L"LocalDebuggerCommandArguments", L"StartArguments", L"CommandArguments" };
        bool ok = false;
        for (const wchar_t* property_name : property_names)
        {
            VARIANT index{};
            VariantInit(&index);
            index.vt = VT_BSTR;
            index.bstrVal = SysAllocString(property_name);
            IDispatch* property = nullptr;
            if (get_collection_item(properties, index, &property) && property != nullptr)
            {
                ok = put_string_property(property, L"Value", arguments);
                property->Release();
            }
            VariantClear(&index);
            if (ok) break;
        }

        properties->Release();
        configuration->Release();
        configuration_manager->Release();
        return ok;
    }

    bool get_project_debug_arguments(IDispatch* project, std::string& configuration_identity, std::string& arguments)
    {
        configuration_identity.clear();
        arguments.clear();
        IDispatch* project_object = nullptr;
        IDispatch* configuration = nullptr;
        IDispatch* debug_settings = nullptr;
        if (get_dispatch_property(project, L"Object", &project_object) && project_object != nullptr)
        {
            if (get_dispatch_property(project_object, L"ActiveConfiguration", &configuration) && configuration != nullptr &&
                get_dispatch_property(configuration, L"DebugSettings", &debug_settings) && debug_settings != nullptr)
            {
                std::string configuration_name;
                const bool ok = get_string_property(configuration, L"Name", configuration_name) &&
                    !configuration_name.empty() &&
                    get_string_property(debug_settings, L"CommandArguments", arguments);
                debug_settings->Release();
                configuration->Release();
                project_object->Release();
                if (ok)
                {
                    configuration_identity = "vc:" + configuration_name;
                    return true;
                }
            }
            else
            {
                if (debug_settings != nullptr) debug_settings->Release();
                if (configuration != nullptr) configuration->Release();
                project_object->Release();
            }
        }

        IDispatch* configuration_manager = nullptr;
        IDispatch* active_configuration = nullptr;
        IDispatch* properties = nullptr;
        if (!get_dispatch_property(project, L"ConfigurationManager", &configuration_manager) || configuration_manager == nullptr ||
            !get_dispatch_property(configuration_manager, L"ActiveConfiguration", &active_configuration) || active_configuration == nullptr ||
            !get_dispatch_property(active_configuration, L"Properties", &properties) || properties == nullptr)
        {
            if (properties != nullptr) properties->Release();
            if (active_configuration != nullptr) active_configuration->Release();
            if (configuration_manager != nullptr) configuration_manager->Release();
            return false;
        }

        const wchar_t* property_names[] = { L"LocalDebuggerCommandArguments", L"StartArguments", L"CommandArguments" };
        std::string configuration_name;
        std::string platform_name;
        if (!get_string_property(active_configuration, L"ConfigurationName", configuration_name) ||
            !get_string_property(active_configuration, L"PlatformName", platform_name) ||
            configuration_name.empty() || platform_name.empty())
        {
            properties->Release();
            active_configuration->Release();
            configuration_manager->Release();
            return false;
        }

        bool ok = false;
        for (const wchar_t* property_name : property_names)
        {
            VARIANT index{};
            VariantInit(&index);
            index.vt = VT_BSTR;
            index.bstrVal = SysAllocString(property_name);
            IDispatch* property = nullptr;
            if (get_collection_item(properties, index, &property) && property != nullptr)
            {
                ok = get_string_property(property, L"Value", arguments);
                property->Release();
            }
            VariantClear(&index);
            if (ok) break;
        }

        properties->Release();
        active_configuration->Release();
        configuration_manager->Release();
        if (ok) configuration_identity = "env:" + configuration_name + "|" + platform_name;
        return ok;
    }

    bool capture_file_contents(const std::string& path, bool& existed, std::string& contents)
    {
        existed = false;
        contents.clear();
        const std::wstring wide_path = widen_utf16(path);
        const DWORD attributes = GetFileAttributesW(wide_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
        }

        existed = true;
        HANDLE file = CreateFileW(
            wide_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 && size.QuadPart <= 16LL * 1024LL * 1024LL;
        if (ok && size.QuadPart > 0)
        {
            contents.resize(static_cast<size_t>(size.QuadPart));
            DWORD bytes_read = 0;
            ok = ReadFile(file, &contents[0], static_cast<DWORD>(contents.size()), &bytes_read, nullptr) != FALSE &&
                bytes_read == contents.size();
        }
        CloseHandle(file);
        return ok;
    }

    bool restore_file_contents(const std::string& path, bool existed, const std::string& contents)
    {
        const std::wstring wide_path = widen_utf16(path);
        if (!existed)
        {
            if (DeleteFileW(wide_path.c_str()) != FALSE) return true;
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }

        HANDLE file = CreateFileW(
            wide_path.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        bool ok = true;
        if (!contents.empty())
        {
            DWORD bytes_written = 0;
            ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &bytes_written, nullptr) != FALSE &&
                bytes_written == contents.size();
        }
        if (ok) ok = FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        return ok;
    }

    bool get_string_property(IDispatch* dispatch, const wchar_t* name, std::string& out)
    {
        VARIANT value{};
        if (!invoke_propget(dispatch, name, value))
        {
            return false;
        }

        bool ok = false;
        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
        {
            out = narrow_utf8(std::wstring(value.bstrVal, SysStringLen(value.bstrVal)));
            ok = true;
        }

        VariantClear(&value);
        return ok;
    }

    bool get_bool_property(IDispatch* dispatch, const wchar_t* name, bool& out)
    {
        VARIANT value{};
        if (!invoke_propget(dispatch, name, value))
        {
            return false;
        }

        bool ok = false;
        if (value.vt == VT_BOOL)
        {
            out = (value.boolVal == VARIANT_TRUE);
            ok = true;
        }

        VariantClear(&value);
        return ok;
    }

    bool get_int_property(IDispatch* dispatch, const wchar_t* name, int& out)
    {
        VARIANT value{};
        if (!invoke_propget(dispatch, name, value))
        {
            return false;
        }

        bool ok = false;
        if (value.vt == VT_I4 || value.vt == VT_INT)
        {
            out = value.lVal;
            ok = true;
        }

        VariantClear(&value);
        return ok;
    }

    std::string map_debug_mode(int mode)
    {
        if (mode == 1)
        {
            return "design";
        }

        if (mode == 2)
        {
            return "break";
        }

        if (mode == 3)
        {
            return "run";
        }

        return "unknown";
    }

    unsigned int parse_pid_from_moniker(const std::wstring& moniker)
    {
        const size_t pos = moniker.rfind(L':');
        if (pos == std::wstring::npos || (pos + 1) >= moniker.size())
        {
            return 0;
        }

        unsigned int value = 0;
        for (size_t i = pos + 1; i < moniker.size(); ++i)
        {
            const wchar_t ch = moniker[i];
            if (ch < L'0' || ch > L'9')
            {
                return 0;
            }

            value = (value * 10U) + static_cast<unsigned int>(ch - L'0');
        }

        return value;
    }

    std::string get_suffix_from_registry_root(const std::string& registry_root)
    {
        // "...\VisualStudio\17.0_<8-char instance id><suffix>" (VS 2022) or "18.0_..." (VS 2026).
        static const char* const markers[] = { "17.0_", "18.0_" };
        for (const char* marker : markers)
        {
            const size_t marker_pos = registry_root.find(marker);
            if (marker_pos == std::string::npos)
            {
                continue;
            }

            const size_t suffix_pos = marker_pos + std::strlen(marker) + 8;
            if (suffix_pos >= registry_root.size())
            {
                return "";
            }

            return registry_root.substr(suffix_pos);
        }

        return "";
    }

    bool suffix_matches(const std::string& registry_root, const std::string& suffix)
    {
        const std::string found_suffix = get_suffix_from_registry_root(registry_root);
        if (suffix.empty())
        {
            return found_suffix.empty();
        }

        return (_stricmp(found_suffix.c_str(), suffix.c_str()) == 0);
    }

    std::string to_lower_ascii(std::string text)
    {
        for (char& ch : text)
        {
            if (ch >= 'A' && ch <= 'Z')
            {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }

        return text;
    }

    std::string get_filename_no_ext(const std::string& path)
    {
        if (path.empty())
        {
            return "";
        }

        size_t slash_pos = path.find_last_of("\\/");
        size_t name_start = (slash_pos == std::string::npos) ? 0 : (slash_pos + 1);
        std::string file_name = path.substr(name_start);
        size_t dot_pos = file_name.find_last_of('.');
        if (dot_pos != std::string::npos)
        {
            file_name = file_name.substr(0, dot_pos);
        }

        return file_name;
    }

    bool bind_dte_for_suffix(const std::string& suffix, IDispatch** out_dispatch)
    {
        if (out_dispatch == nullptr)
        {
            return false;
        }

        *out_dispatch = nullptr;

        IRunningObjectTable* rot = nullptr;
        HRESULT hr = GetRunningObjectTable(0, &rot);
        if (FAILED(hr) || rot == nullptr)
        {
            return false;
        }

        IEnumMoniker* enum_moniker = nullptr;
        hr = rot->EnumRunning(&enum_moniker);
        if (FAILED(hr) || enum_moniker == nullptr)
        {
            rot->Release();
            return false;
        }

        IBindCtx* bind_ctx = nullptr;
        hr = CreateBindCtx(0, &bind_ctx);
        if (FAILED(hr) || bind_ctx == nullptr)
        {
            enum_moniker->Release();
            rot->Release();
            return false;
        }

        IMoniker* moniker = nullptr;
        bool found = false;
        while (!found && enum_moniker->Next(1, &moniker, nullptr) == S_OK)
        {
            LPOLESTR display_name = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(bind_ctx, nullptr, &display_name)) && display_name != nullptr)
            {
                const std::wstring moniker_name(display_name);
                CoTaskMemFree(display_name);

                if (moniker_name.find(L"VisualStudio.DTE") != std::wstring::npos)
                {
                    IUnknown* object = nullptr;
                    if (SUCCEEDED(rot->GetObject(moniker, &object)) && object != nullptr)
                    {
                        IDispatch* dispatch = nullptr;
                        if (SUCCEEDED(object->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dispatch))) && dispatch != nullptr)
                        {
                            std::string registry_root;
                            if (get_string_property(dispatch, L"RegistryRoot", registry_root) && suffix_matches(registry_root, suffix))
                            {
                                *out_dispatch = dispatch;
                                found = true;
                            }
                            else
                            {
                                dispatch->Release();
                            }
                        }

                        object->Release();
                    }
                }
            }

            moniker->Release();
        }

        bind_ctx->Release();
        enum_moniker->Release();
        rot->Release();
        return found;
    }

    bool bind_dte_for_pid(unsigned int process_id, IDispatch** out_dispatch)
    {
        if (out_dispatch == nullptr || process_id == 0)
        {
            return false;
        }

        *out_dispatch = nullptr;

        IRunningObjectTable* rot = nullptr;
        HRESULT hr = GetRunningObjectTable(0, &rot);
        if (FAILED(hr) || rot == nullptr)
        {
            return false;
        }

        IEnumMoniker* enum_moniker = nullptr;
        hr = rot->EnumRunning(&enum_moniker);
        if (FAILED(hr) || enum_moniker == nullptr)
        {
            rot->Release();
            return false;
        }

        IBindCtx* bind_ctx = nullptr;
        hr = CreateBindCtx(0, &bind_ctx);
        if (FAILED(hr) || bind_ctx == nullptr)
        {
            enum_moniker->Release();
            rot->Release();
            return false;
        }

        IMoniker* moniker = nullptr;
        bool found = false;
        while (!found && enum_moniker->Next(1, &moniker, nullptr) == S_OK)
        {
            LPOLESTR display_name = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(bind_ctx, nullptr, &display_name)) && display_name != nullptr)
            {
                const std::wstring moniker_name(display_name);
                CoTaskMemFree(display_name);

                if (moniker_name.find(L"VisualStudio.DTE") != std::wstring::npos && parse_pid_from_moniker(moniker_name) == process_id)
                {
                    IUnknown* object = nullptr;
                    if (SUCCEEDED(rot->GetObject(moniker, &object)) && object != nullptr)
                    {
                        IDispatch* dispatch = nullptr;
                        if (SUCCEEDED(object->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dispatch))) && dispatch != nullptr)
                        {
                            *out_dispatch = dispatch;
                            found = true;
                        }

                        object->Release();
                    }
                }
            }

            moniker->Release();
        }

        bind_ctx->Release();
        enum_moniker->Release();
        rot->Release();
        return found;
    }
}

namespace bh_agent_dte
{
    bool enumerate_dte_instances(std::vector<dte_instance_info>& instances, std::string& error_text)
    {
        instances.clear();
        error_text.clear();

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        IRunningObjectTable* rot = nullptr;
        HRESULT hr = GetRunningObjectTable(0, &rot);
        if (FAILED(hr) || rot == nullptr)
        {
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "GetRunningObjectTable failed";
            return false;
        }

        IEnumMoniker* enum_moniker = nullptr;
        hr = rot->EnumRunning(&enum_moniker);
        if (FAILED(hr) || enum_moniker == nullptr)
        {
            rot->Release();
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "EnumRunning failed";
            return false;
        }

        IBindCtx* bind_ctx = nullptr;
        hr = CreateBindCtx(0, &bind_ctx);
        if (FAILED(hr) || bind_ctx == nullptr)
        {
            enum_moniker->Release();
            rot->Release();
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "CreateBindCtx failed";
            return false;
        }

        IMoniker* moniker = nullptr;
        while (enum_moniker->Next(1, &moniker, nullptr) == S_OK)
        {
            LPOLESTR display_name = nullptr;
            if (SUCCEEDED(moniker->GetDisplayName(bind_ctx, nullptr, &display_name)) && display_name != nullptr)
            {
                const std::wstring moniker_name(display_name);
                CoTaskMemFree(display_name);

                if (moniker_name.find(L"VisualStudio.DTE") != std::wstring::npos)
                {
                    IUnknown* object = nullptr;
                    if (SUCCEEDED(rot->GetObject(moniker, &object)) && object != nullptr)
                    {
                        IDispatch* dispatch = nullptr;
                        if (SUCCEEDED(object->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dispatch))) && dispatch != nullptr)
                        {
                            dte_instance_info item{};
                            item.moniker_name = narrow_utf8(moniker_name);
                            item.process_id = parse_pid_from_moniker(moniker_name);
                            item.debug_mode = "unknown";

                            std::string registry_root;
                            if (get_string_property(dispatch, L"RegistryRoot", registry_root))
                            {
                                item.registry_root = registry_root;
                            }

                            IDispatch* solution = nullptr;
                            if (get_dispatch_property(dispatch, L"Solution", &solution) && solution != nullptr)
                            {
                                bool is_open = false;
                                if (get_bool_property(solution, L"IsOpen", is_open) && is_open)
                                {
                                    std::string full_name;
                                    if (get_string_property(solution, L"FullName", full_name))
                                    {
                                        item.solution_path = full_name;
                                    }
                                }

                                solution->Release();
                            }

                            IDispatch* debugger = nullptr;
                            if (get_dispatch_property(dispatch, L"Debugger", &debugger) && debugger != nullptr)
                            {
                                int mode = 0;
                                if (get_int_property(debugger, L"CurrentMode", mode))
                                {
                                    item.debug_mode = map_debug_mode(mode);
                                }

                                debugger->Release();
                            }

                            instances.push_back(item);
                            dispatch->Release();
                        }

                        object->Release();
                    }
                }
            }

            moniker->Release();
        }

        bind_ctx->Release();
        enum_moniker->Release();
        rot->Release();

        if (must_uninitialize)
        {
            CoUninitialize();
        }

        return true;
    }

    bool has_instance_for_suffix(const std::string& suffix, bool& exists, std::string& error_text)
    {
        exists = false;
        std::vector<dte_instance_info> instances;
        if (!enumerate_dte_instances(instances, error_text))
        {
            return false;
        }

        for (const dte_instance_info& item : instances)
        {
            if (suffix_matches(item.registry_root, suffix))
            {
                exists = true;
                break;
            }
        }

        return true;
    }

    bool get_instance_pid_for_suffix(const std::string& suffix, unsigned int& process_id, std::string& error_text)
    {
        process_id = 0;
        std::vector<dte_instance_info> instances;
        if (!enumerate_dte_instances(instances, error_text))
        {
            return false;
        }

        for (const dte_instance_info& item : instances)
        {
            if (!suffix_matches(item.registry_root, suffix))
            {
                continue;
            }

            if (item.process_id == 0)
            {
                error_text = "DTE instance PID unavailable";
                return false;
            }

            process_id = item.process_id;
            return true;
        }

        error_text = "DTE instance not found";
        return false;
    }

    bool get_instance_pid_for_solution_hint(const std::string& solution_hint, unsigned int& process_id, std::string& error_text)
    {
        process_id = 0;
        std::string hint = to_lower_ascii(solution_hint);
        if (hint.empty())
        {
            error_text = "solution hint is empty";
            return false;
        }

        const std::string hint_no_ext = to_lower_ascii(get_filename_no_ext(solution_hint));

        std::vector<dte_instance_info> instances;
        if (!enumerate_dte_instances(instances, error_text))
        {
            return false;
        }

        std::string debug_dump = "hintResolve hint='" + solution_hint + "' instances=" + std::to_string(instances.size());
        for (const dte_instance_info& item : instances)
        {
            debug_dump += " | pid=" + std::to_string(item.process_id) +
                " root='" + item.registry_root +
                "' mode='" + item.debug_mode +
                "' solution='" + item.solution_path + "'";
        }
        bh_log::write("service.dte", debug_dump);

        unsigned int contains_pid = 0;
        for (const dte_instance_info& item : instances)
        {
            if (item.solution_path.empty() || item.process_id == 0)
            {
                continue;
            }

            const std::string path_lower = to_lower_ascii(item.solution_path);
            const std::string solution_name = to_lower_ascii(get_filename_no_ext(item.solution_path));
            if (solution_name == hint || (!hint_no_ext.empty() && solution_name == hint_no_ext))
            {
                process_id = item.process_id;
                return true;
            }

            if (contains_pid == 0 && path_lower.find(hint) != std::string::npos)
            {
                contains_pid = item.process_id;
            }
        }

        if (contains_pid != 0)
        {
            process_id = contains_pid;
            return true;
        }

        error_text = "DTE instance not found";
        bh_log::write("service.dte", "hintResolve miss hint='" + solution_hint + "'");
        return false;
    }

    bool launch_devenv(const std::string& solution_path, const std::string& suffix, std::string& error_text)
    {
        error_text.clear();

        // Launch the newest VS 2022/2026 install, preferring one that already has a profile
        // (hive) for this root suffix, because that is where the extension was deployed.
        const std::wstring suffix_w = widen_utf16(suffix);
        const std::wstring hive_root = bh_vs_installations::local_hive_root();
        std::wstring devenv_path;
        for (const auto& vs : bh_vs_installations::find())
        {
            const std::wstring candidate = bh_vs_installations::devenv_path(vs);
            if (!bh_vs_installations::is_file(candidate))
            {
                continue;
            }

            if (devenv_path.empty())
            {
                devenv_path = candidate;
            }

            if (!suffix_w.empty() && !hive_root.empty() &&
                bh_vs_installations::is_directory(hive_root + L"\\" + bh_vs_installations::hive_name(vs, suffix_w)))
            {
                devenv_path = candidate;
                break;
            }
        }

        if (devenv_path.empty())
        {
            error_text = "devenv.exe not found (no Visual Studio 2022 or 2026 installation)";
            return false;
        }

        bh_log::write("service.dte", "launch devenv='" + narrow_utf8(devenv_path) + "' suffix='" + suffix + "'");

        std::wstring args;
        if (!suffix.empty())
        {
            args += L" /RootSuffix ";
            args += widen_utf16(suffix);
        }

        if (!solution_path.empty())
        {
            args += L" \"";
            args += widen_utf16(solution_path);
            args += L"\"";
        }

        std::wstring cmdline = L"\"" + devenv_path + L"\"" + args;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            error_text = "CreateProcessW failed";
            return false;
        }

        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    bool execute_command_for_suffix(const std::string& suffix, const std::string& command, std::string& error_text, unsigned int timeout_ms)
    {
        error_text.clear();

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        const unsigned long long start_ms = GetTickCount64();
        bool ok = false;
        while ((GetTickCount64() - start_ms) <= static_cast<unsigned long long>(timeout_ms))
        {
            IDispatch* dte = nullptr;
            if (!bind_dte_for_suffix(suffix, &dte) || dte == nullptr)
            {
                Sleep(200);
                continue;
            }

            VARIANT args[2]{};
            VariantInit(&args[0]);
            VariantInit(&args[1]);
            args[0].vt = VT_BSTR;
            args[0].bstrVal = SysAllocString(L"");
            const std::wstring cmd_w = widen_utf16(command);
            args[1].vt = VT_BSTR;
            args[1].bstrVal = SysAllocStringLen(cmd_w.c_str(), static_cast<UINT>(cmd_w.size()));

            ok = invoke_method(dte, L"ExecuteCommand", args, 2, nullptr);

            VariantClear(&args[1]);
            VariantClear(&args[0]);
            dte->Release();

            if (ok)
            {
                break;
            }

            Sleep(200);
        }

        if (must_uninitialize)
        {
            CoUninitialize();
        }

        if (!ok)
        {
            error_text = "ExecuteCommand failed";
        }

        return ok;
    }

    bool get_debug_mode_for_suffix(const std::string& suffix, std::string& mode, std::string& error_text)
    {
        mode = "unknown";
        error_text.clear();

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        IDispatch* dte = nullptr;
        if (!bind_dte_for_suffix(suffix, &dte) || dte == nullptr)
        {
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "DTE instance not found";
            return false;
        }

        IDispatch* debugger = nullptr;
        bool ok = false;
        if (get_dispatch_property(dte, L"Debugger", &debugger) && debugger != nullptr)
        {
            int value = 0;
            if (get_int_property(debugger, L"CurrentMode", value))
            {
                mode = map_debug_mode(value);
                ok = true;
            }

            debugger->Release();
        }

        dte->Release();
        if (must_uninitialize)
        {
            CoUninitialize();
        }

        if (!ok)
        {
            error_text = "failed to query debug mode";
        }

        return ok;
    }

    bool execute_command_for_pid(unsigned int process_id, const std::string& command, std::string& error_text, unsigned int timeout_ms)
    {
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        const unsigned long long start_ms = GetTickCount64();
        bool ok = false;
        while ((GetTickCount64() - start_ms) <= static_cast<unsigned long long>(timeout_ms))
        {
            IDispatch* dte = nullptr;
            if (!bind_dte_for_pid(process_id, &dte) || dte == nullptr)
            {
                Sleep(200);
                continue;
            }

            VARIANT args[2]{};
            VariantInit(&args[0]);
            VariantInit(&args[1]);
            args[0].vt = VT_BSTR;
            args[0].bstrVal = SysAllocString(L"");
            const std::wstring cmd_w = widen_utf16(command);
            args[1].vt = VT_BSTR;
            args[1].bstrVal = SysAllocStringLen(cmd_w.c_str(), static_cast<UINT>(cmd_w.size()));

            ok = invoke_method(dte, L"ExecuteCommand", args, 2, nullptr);

            VariantClear(&args[1]);
            VariantClear(&args[0]);
            dte->Release();

            if (ok)
            {
                break;
            }

            Sleep(200);
        }

        if (must_uninitialize)
        {
            CoUninitialize();
        }

        if (!ok)
        {
            error_text = "ExecuteCommand failed";
        }

        return ok;
    }

    bool open_profile_report_for_pid(unsigned int process_id, const std::wstring& path, std::string& error_text)
    {
        error_text.clear();
        if (!process_id || path.empty()) { error_text = "missing explicit VS PID or report"; return false; }
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) { error_text = "COM initialization failed"; return false; }
        IDispatch* dte = nullptr;
        IDispatch* operations = nullptr;
        bool ok = false;
        if (bind_dte_for_pid(process_id, &dte) && dte && get_dispatch_property(dte, L"ItemOperations", &operations))
        {
            VARIANT args[2] = {};
            args[0].vt = args[1].vt = VT_BSTR;
            args[0].bstrVal = SysAllocString(L"{FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF}"); // vsViewKindAny
            args[1].bstrVal = SysAllocStringLen(path.c_str(), static_cast<UINT>(path.size()));
            if (args[0].bstrVal && args[1].bstrVal) ok = invoke_method(operations, L"OpenFile", args, 2, nullptr);
            VariantClear(&args[0]); VariantClear(&args[1]);
        }
        if (operations) operations->Release();
        if (dte) dte->Release();
        if (SUCCEEDED(init)) CoUninitialize();
        if (!ok) error_text = "DTE report open failed; check the selected VS instance and report path";
        return ok;
    }

    bool capture_startup_project_arguments_for_pid(unsigned int process_id, startup_project_arguments& state, std::string& error_text)
    {
        state = startup_project_arguments{};
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        bool ok = false;
        std::string startup_project;
        IDispatch* dte = nullptr;
        IDispatch* solution = nullptr;
        IDispatch* project = nullptr;
        if (bind_dte_for_pid(process_id, &dte) && dte != nullptr &&
            get_dispatch_property(dte, L"Solution", &solution) && solution != nullptr &&
            get_first_startup_project(solution, startup_project) &&
            get_project_for_unique_name(solution, startup_project, &project) && project != nullptr)
        {
            ok = get_project_debug_arguments(project, state.configuration_identity, state.arguments);
            if (ok)
            {
                std::string project_path;
                ok = get_string_property(project, L"FullName", project_path) && !project_path.empty();
                if (ok)
                {
                    state.project_unique_name = startup_project;
                    state.user_file_path = project_path + ".user";
                    ok = capture_file_contents(state.user_file_path, state.user_file_existed, state.user_file_contents);
                }
            }
        }

        if (project != nullptr) project->Release();
        if (solution != nullptr) solution->Release();
        if (dte != nullptr) dte->Release();
        if (must_uninitialize) CoUninitialize();
        if (!ok)
        {
            error_text = startup_project.empty()
                ? "active startup project not found"
                : "startup project does not expose readable debugger arguments: " + startup_project;
        }
        return ok;
    }

    bool restore_startup_project_user_file(const startup_project_arguments& state, std::string& error_text)
    {
        error_text.clear();
        if (state.user_file_path.empty())
        {
            error_text = "captured startup project user file path is missing";
            return false;
        }

        if (!restore_file_contents(state.user_file_path, state.user_file_existed, state.user_file_contents))
        {
            error_text = "failed to restore startup project user file: " + state.user_file_path;
            return false;
        }
        return true;
    }

    bool set_startup_project_arguments_for_pid(unsigned int process_id, const startup_project_arguments& state, const std::string& arguments, std::string& error_text)
    {
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        bool ok = false;
        IDispatch* dte = nullptr;
        IDispatch* solution = nullptr;
        IDispatch* project = nullptr;
        if (bind_dte_for_pid(process_id, &dte) && dte != nullptr &&
            get_dispatch_property(dte, L"Solution", &solution) && solution != nullptr &&
            !state.project_unique_name.empty() &&
            !state.configuration_identity.empty() &&
            get_project_for_unique_name(solution, state.project_unique_name, &project) && project != nullptr)
        {
            ok = set_project_debug_arguments(project, state.configuration_identity, arguments);
        }

        if (project != nullptr) project->Release();
        if (solution != nullptr) solution->Release();
        if (dte != nullptr) dte->Release();
        if (must_uninitialize) CoUninitialize();
        if (!ok)
        {
            error_text = state.project_unique_name.empty()
                ? "captured startup project is missing"
                : "captured startup configuration does not expose writable debugger arguments: " + state.project_unique_name + " " + state.configuration_identity;
        }
        return ok;
    }

    bool attach_process_for_pid(unsigned int process_id, unsigned int target_process_id, std::string& error_text, unsigned int timeout_ms)
    {
        error_text.clear();
        if (process_id == 0 || target_process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        bool ok = false;
        const unsigned long long start_ms = GetTickCount64();
        while (!ok && (GetTickCount64() - start_ms) <= static_cast<unsigned long long>(timeout_ms))
        {
            IDispatch* dte = nullptr;
            IDispatch* debugger = nullptr;
            IDispatch* processes = nullptr;
            if (bind_dte_for_pid(process_id, &dte) && dte != nullptr &&
                get_dispatch_property(dte, L"Debugger", &debugger) && debugger != nullptr &&
                get_dispatch_property(debugger, L"LocalProcesses", &processes) && processes != nullptr)
            {
                int count = 0;
                if (get_int_property(processes, L"Count", count))
                {
                    for (int item_index = 1; item_index <= count && !ok; ++item_index)
                    {
                        VARIANT index{};
                        VariantInit(&index);
                        index.vt = VT_I4;
                        index.lVal = item_index;
                        IDispatch* process = nullptr;
                        if (get_collection_item(processes, index, &process) && process != nullptr)
                        {
                            int found_process_id = 0;
                            if (get_int_property(process, L"ProcessID", found_process_id) &&
                                static_cast<unsigned int>(found_process_id) == target_process_id)
                            {
                                ok = invoke_method(process, L"Attach", nullptr, 0, nullptr);
                            }
                            process->Release();
                        }
                    }
                }
            }

            if (processes != nullptr) processes->Release();
            if (debugger != nullptr) debugger->Release();
            if (dte != nullptr) dte->Release();
            if (!ok) Sleep(200);
        }

        if (must_uninitialize) CoUninitialize();
        if (!ok) error_text = "target process not found or Attach failed";
        return ok;
    }

    bool wait_for_debugged_process_for_pid(unsigned int process_id, unsigned int timeout_ms, std::string& error_text)
    {
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        bool found = false;
        const unsigned long long start_ms = GetTickCount64();
        while (!found && (GetTickCount64() - start_ms) <= static_cast<unsigned long long>(timeout_ms))
        {
            IDispatch* dte = nullptr;
            IDispatch* debugger = nullptr;
            IDispatch* processes = nullptr;
            if (bind_dte_for_pid(process_id, &dte) && dte != nullptr &&
                get_dispatch_property(dte, L"Debugger", &debugger) && debugger != nullptr &&
                get_dispatch_property(debugger, L"DebuggedProcesses", &processes) && processes != nullptr)
            {
                int count = 0;
                found = get_int_property(processes, L"Count", count) && count > 0;
            }

            if (processes != nullptr) processes->Release();
            if (debugger != nullptr) debugger->Release();
            if (dte != nullptr) dte->Release();
            if (!found) Sleep(100);
        }

        if (must_uninitialize) CoUninitialize();
        if (!found) error_text = "timed out waiting for a debugged process";
        return found;
    }

    bool get_debug_mode_for_pid(unsigned int process_id, std::string& mode, std::string& error_text)
    {
        mode = "unknown";
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        IDispatch* dte = nullptr;
        if (!bind_dte_for_pid(process_id, &dte) || dte == nullptr)
        {
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "DTE instance not found";
            return false;
        }

        IDispatch* debugger = nullptr;
        bool ok = false;
        if (get_dispatch_property(dte, L"Debugger", &debugger) && debugger != nullptr)
        {
            int value = 0;
            if (get_int_property(debugger, L"CurrentMode", value))
            {
                mode = map_debug_mode(value);
                ok = true;
            }

            debugger->Release();
        }

        dte->Release();
        if (must_uninitialize)
        {
            CoUninitialize();
        }

        if (!ok)
        {
            error_text = "failed to query debug mode";
        }

        return ok;
    }

    bool get_build_status_for_pid(unsigned int process_id, std::string& status_text, std::string& error_text)
    {
        status_text.clear();
        error_text.clear();
        if (process_id == 0)
        {
            error_text = "invalid PID";
            return false;
        }

        bool must_uninitialize = false;
        const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(init_hr))
        {
            must_uninitialize = true;
        }
        else if (init_hr != RPC_E_CHANGED_MODE)
        {
            error_text = "CoInitializeEx failed";
            return false;
        }

        IDispatch* dte = nullptr;
        if (!bind_dte_for_pid(process_id, &dte) || dte == nullptr)
        {
            if (must_uninitialize)
            {
                CoUninitialize();
            }

            error_text = "DTE instance not found";
            return false;
        }

        bool ok = false;
        int build_state = 0;
        int last_build_info = 0;

        IDispatch* solution = nullptr;
        if (get_dispatch_property(dte, L"Solution", &solution) && solution != nullptr)
        {
            IDispatch* solution_build = nullptr;
            if (get_dispatch_property(solution, L"SolutionBuild", &solution_build) && solution_build != nullptr)
            {
                int state_value = 0;
                int last_info_value = 0;
                if (get_int_property(solution_build, L"BuildState", state_value) &&
                    get_int_property(solution_build, L"LastBuildInfo", last_info_value))
                {
                    build_state = state_value;
                    last_build_info = last_info_value;
                    ok = true;
                }

                solution_build->Release();
            }

            solution->Release();
        }

        dte->Release();
        if (must_uninitialize)
        {
            CoUninitialize();
        }

        if (!ok)
        {
            error_text = "failed to query build status";
            return false;
        }

        std::string state_text = "unknown";
        if (build_state == 1)
        {
            state_text = "not_started";
        }
        else if (build_state == 2)
        {
            state_text = "in_progress";
        }
        else if (build_state == 3)
        {
            state_text = "done";
        }

        status_text = "buildState=" + state_text + " lastBuildInfo=" + std::to_string(last_build_info);
        return true;
    }
}
