#include "bh_agent_local_worker_component.h"
#include "..\bh_agent_log.h"
#include "..\bh_agent_symbol_search_budget.h"
#include <algorithm>
#include <dia2.h>
#include <string>
#include <vector>

namespace
{
const GUID bh_guid_agent_message_to_local_worker = { 0xa5d0071b, 0xd3b8, 0x43b5, { 0x8e, 0xd9, 0xdd, 0xb2, 0x4, 0xf6, 0xe3, 0x3 } };
const UINT32 bh_message_to_local_worker_resolve_symbol = 1;
const UINT32 bh_message_to_local_worker_resolve_address = 2;

bool bh_wide_contains_case_insensitive(const wchar_t* text, const wchar_t* needle)
{
    if (!text || !needle || !needle[0])
    {
        return false;
    }

    const size_t needle_len = wcslen(needle);
    for (const wchar_t* p = text; *p; ++p)
    {
        if (_wcsnicmp(p, needle, needle_len) == 0)
        {
            return true;
        }
    }

    return false;
}

enum class bh_symbol_match_kind
{
    none,
    substring,
    exact
};

enum class bh_symbol_address_status
{
    resolved,
    non_static_data,
    non_code_public,
    missing_address
};

enum class bh_symbol_resolution_status
{
    resolved,
    symbols_unavailable,
    symbol_not_found,
    no_static_address,
    ambiguous,
    search_limited
};

struct bh_symbol_candidate
{
    UINT64 address = 0;
    std::wstring module_name;
    std::wstring compiland_name;
    std::wstring decorated_name;
    std::wstring undecorated_name;
    DWORD location_type = 0;
    bh_symbol_address_status address_status = bh_symbol_address_status::missing_address;
};

struct bh_symbol_resolution_result
{
    bh_symbol_resolution_status status = bh_symbol_resolution_status::symbol_not_found;
    bh_symbol_match_kind match_kind = bh_symbol_match_kind::none;
    HRESULT dia_hr = E_FAIL;
    bool pdb_available = false;
    bool candidate_overflow = false;
    UINT64 address = 0;
    std::vector<bh_symbol_candidate> candidates;
    std::vector<std::wstring> attempted_modules;
    std::vector<std::wstring> unavailable_modules;
    bh_agent_symbol_search::limit search_limit = bh_agent_symbol_search::limit::none;
    UINT64 symbols_examined = 0;
    UINT64 modules_examined = 0;
};

bh_symbol_match_kind bh_classify_symbol_name(
    IDiaSymbol* symbol,
    const wchar_t* symbol_name,
    std::wstring& decorated_name_out,
    std::wstring& undecorated_name_out)
{
    decorated_name_out.clear();
    undecorated_name_out.clear();

    if (!symbol || !symbol_name || !symbol_name[0])
    {
        return bh_symbol_match_kind::none;
    }

    bool exact = false;
    bool substring = false;

    CComBSTR decorated_name;
    if (SUCCEEDED(symbol->get_name(&decorated_name)) && decorated_name && decorated_name[0])
    {
        decorated_name_out = decorated_name;
        exact = _wcsicmp(decorated_name, symbol_name) == 0;
        substring = bh_wide_contains_case_insensitive(decorated_name, symbol_name);
    }

    CComBSTR undecorated_name;
    if (SUCCEEDED(symbol->get_undecoratedName(&undecorated_name)) && undecorated_name && undecorated_name[0])
    {
        undecorated_name_out = undecorated_name;
        exact = exact || _wcsicmp(undecorated_name, symbol_name) == 0;
        substring = substring || bh_wide_contains_case_insensitive(undecorated_name, symbol_name);
    }

    if (exact)
    {
        return bh_symbol_match_kind::exact;
    }

    return substring ? bh_symbol_match_kind::substring : bh_symbol_match_kind::none;
}

bh_symbol_address_status bh_try_get_symbol_address(
    IDiaSymbol* symbol,
    IDiaSession* dia_session,
    DkmNativeModuleInstance* native_module,
    UINT64& absolute_address,
    DWORD& location_type)
{
    absolute_address = 0;
    location_type = 0;

    if (!symbol || !dia_session || !native_module)
    {
        return bh_symbol_address_status::missing_address;
    }

    DWORD sym_tag = SymTagNull;
    if (FAILED(symbol->get_symTag(&sym_tag)))
    {
        return bh_symbol_address_status::missing_address;
    }

    if (sym_tag == SymTagData)
    {
        if (FAILED(symbol->get_locationType(&location_type)) || location_type != LocIsStatic)
        {
            return bh_symbol_address_status::non_static_data;
        }
    }
    else if (sym_tag != SymTagFunction)
    {
        BOOL is_code = TRUE;
        if (SUCCEEDED(symbol->get_code(&is_code)) && !is_code)
        {
            return bh_symbol_address_status::non_code_public;
        }
    }

    const UINT64 module_base = native_module->BaseAddress();
    DWORD rva = 0;
    if (module_base != 0 &&
        SUCCEEDED(symbol->get_relativeVirtualAddress(&rva)) && rva != 0)
    {
        absolute_address = module_base + rva;
        return bh_symbol_address_status::resolved;
    }

    ULONGLONG virtual_address = 0;
    if (FAILED(symbol->get_virtualAddress(&virtual_address)) || virtual_address == 0)
    {
        return bh_symbol_address_status::missing_address;
    }

    ULONGLONG load_address = 0;
    if (module_base != 0 &&
        SUCCEEDED(dia_session->get_loadAddress(&load_address)) &&
        load_address != 0 && virtual_address >= load_address)
    {
        const UINT64 offset = virtual_address - load_address;
        if (offset <= (~0ull - module_base))
        {
            absolute_address = module_base + offset;
            return bh_symbol_address_status::resolved;
        }
    }

    if (module_base != 0 && virtual_address >= module_base)
    {
        absolute_address = virtual_address;
        return bh_symbol_address_status::resolved;
    }

    return bh_symbol_address_status::missing_address;
}

void bh_add_address_candidate(
    std::vector<bh_symbol_candidate>& candidates,
    const bh_symbol_candidate& candidate,
    bool& overflow)
{
    for (const bh_symbol_candidate& existing : candidates)
    {
        if (existing.address == candidate.address)
        {
            return;
        }
    }

    if (candidates.size() < 16)
    {
        candidates.push_back(candidate);
    }
    else
    {
        overflow = true;
    }
}

void bh_add_non_address_candidate(
    std::vector<bh_symbol_candidate>& candidates,
    const bh_symbol_candidate& candidate)
{
    for (const bh_symbol_candidate& existing : candidates)
    {
        if (existing.decorated_name == candidate.decorated_name &&
            existing.undecorated_name == candidate.undecorated_name &&
            existing.compiland_name == candidate.compiland_name &&
            existing.address_status == candidate.address_status &&
            existing.location_type == candidate.location_type)
        {
            return;
        }
    }

    if (candidates.size() < 8)
    {
        candidates.push_back(candidate);
    }
}

HRESULT bh_get_dia_session(DkmNativeModuleInstance* native_module, IDiaSession** dia_session_out)
{
    if (!native_module || !dia_session_out)
    {
        return E_INVALIDARG;
    }

    *dia_session_out = nullptr;

    CComPtr<Symbols::DkmModule> module;
    HRESULT hr = native_module->GetModule(&module);
    if (FAILED(hr) || !module)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<IUnknown> symbol_interface;
    hr = module->GetSymbolInterface(__uuidof(IDiaSession), &symbol_interface);
    if (FAILED(hr) || !symbol_interface)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<IDiaSession> dia_session;
    hr = symbol_interface->QueryInterface(__uuidof(IDiaSession), (void**)&dia_session);
    if (FAILED(hr) || !dia_session)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    *dia_session_out = dia_session.Detach();
    return S_OK;
}

void bh_mark_symbol_search_limited(
    bh_symbol_resolution_result& result,
    bh_agent_symbol_search::budget& search_budget)
{
    result.status = bh_symbol_resolution_status::search_limited;
    result.search_limit = search_budget.limit_reason();
    result.symbols_examined = search_budget.symbols_examined();
    result.modules_examined = search_budget.modules_examined();
}

bh_symbol_resolution_result bh_resolve_symbol_with_dia(
    DkmNativeModuleInstance* native_module,
    const wchar_t* symbol_name,
    bh_agent_symbol_search::budget& search_budget)
{
    bh_symbol_resolution_result result;
    if (!native_module || !symbol_name || !symbol_name[0])
    {
        result.status = bh_symbol_resolution_status::symbols_unavailable;
        result.dia_hr = E_INVALIDARG;
        return result;
    }
    if (!search_budget.try_begin_module())
    {
        bh_mark_symbol_search_limited(result, search_budget);
        return result;
    }

    CComPtr<IDiaSession> dia_session;
    HRESULT hr = bh_get_dia_session(native_module, &dia_session);
    if (FAILED(hr) || !dia_session)
    {
        result.status = bh_symbol_resolution_status::symbols_unavailable;
        result.dia_hr = FAILED(hr) ? hr : E_FAIL;
        return result;
    }

    CComPtr<IDiaSymbol> global_scope;
    hr = dia_session->get_globalScope(&global_scope);
    if (FAILED(hr) || !global_scope)
    {
        result.status = bh_symbol_resolution_status::symbols_unavailable;
        result.dia_hr = FAILED(hr) ? hr : E_FAIL;
        return result;
    }

    result.pdb_available = true;
    result.dia_hr = S_OK;

    std::vector<bh_symbol_candidate> exact_addresses;
    std::vector<bh_symbol_candidate> substring_addresses;
    std::vector<bh_symbol_candidate> exact_without_address;
    std::vector<bh_symbol_candidate> substring_without_address;
    bool exact_overflow = false;
    bool substring_overflow = false;

    auto scan_scope = [&](IDiaSymbol* scope, const std::wstring& compiland_name)
    {
        if (!scope)
        {
            return;
        }

        const enum SymTagEnum tags[] = {
            SymTagFunction,
            SymTagPublicSymbol,
            SymTagData
        };

        for (int tag_index = 0;
             tag_index < (int)(sizeof(tags) / sizeof(tags[0]));
             ++tag_index)
        {
            CComPtr<IDiaEnumSymbols> symbols;
            HRESULT find_hr = scope->findChildren(tags[tag_index], nullptr, nsNone, &symbols);
            if (FAILED(find_hr) || !symbols)
            {
                continue;
            }

            for (;;)
            {
                ULONG fetched = 0;
                CComPtr<IDiaSymbol> symbol;
                HRESULT next_hr = symbols->Next(1, &symbol, &fetched);
                if (FAILED(next_hr) || fetched == 0 || !symbol)
                {
                    break;
                }
                if (!search_budget.try_examine_symbol())
                {
                    return;
                }

                bh_symbol_candidate candidate;
                candidate.compiland_name = compiland_name;
                const bh_symbol_match_kind match_kind = bh_classify_symbol_name(
                    symbol,
                    symbol_name,
                    candidate.decorated_name,
                    candidate.undecorated_name);
                if (match_kind == bh_symbol_match_kind::none)
                {
                    continue;
                }

                candidate.address_status = bh_try_get_symbol_address(
                    symbol,
                    dia_session,
                    native_module,
                    candidate.address,
                    candidate.location_type);

                if (candidate.address_status == bh_symbol_address_status::resolved)
                {
                    if (match_kind == bh_symbol_match_kind::exact)
                    {
                        bh_add_address_candidate(exact_addresses, candidate, exact_overflow);
                    }
                    else
                    {
                        bh_add_address_candidate(
                            substring_addresses,
                            candidate,
                            substring_overflow);
                    }
                }
                else if (match_kind == bh_symbol_match_kind::exact)
                {
                    bh_add_non_address_candidate(exact_without_address, candidate);
                }
                else
                {
                    bh_add_non_address_candidate(substring_without_address, candidate);
                }
            }
        }
    };

    scan_scope(global_scope, L"<global>");
    if (search_budget.exhausted())
    {
        bh_mark_symbol_search_limited(result, search_budget);
        return result;
    }

    CComPtr<IDiaEnumSymbols> compilands;
    hr = global_scope->findChildren(SymTagCompiland, nullptr, nsNone, &compilands);
    if (SUCCEEDED(hr) && compilands)
    {
        for (;;)
        {
            ULONG fetched = 0;
            CComPtr<IDiaSymbol> compiland;
            HRESULT next_hr = compilands->Next(1, &compiland, &fetched);
            if (FAILED(next_hr) || fetched == 0 || !compiland)
            {
                break;
            }
            if (!search_budget.try_examine_symbol())
            {
                break;
            }

            std::wstring compiland_name = L"<compiland>";
            CComBSTR dia_compiland_name;
            if (SUCCEEDED(compiland->get_name(&dia_compiland_name)) &&
                dia_compiland_name && dia_compiland_name[0])
            {
                compiland_name = dia_compiland_name;
            }

            scan_scope(compiland, compiland_name);
            if (search_budget.exhausted())
            {
                break;
            }
        }
    }
    if (search_budget.exhausted())
    {
        bh_mark_symbol_search_limited(result, search_budget);
        return result;
    }

    auto select_address_candidates = [&result](
        bh_symbol_match_kind match_kind,
        const std::vector<bh_symbol_candidate>& candidates,
        bool overflow) -> bool
    {
        if (candidates.empty() && !overflow)
        {
            return false;
        }

        result.match_kind = match_kind;
        result.candidates = candidates;
        result.candidate_overflow = overflow;
        if (candidates.size() == 1 && !overflow)
        {
            result.status = bh_symbol_resolution_status::resolved;
            result.address = candidates[0].address;
        }
        else
        {
            result.status = bh_symbol_resolution_status::ambiguous;
        }
        return true;
    };

    if (select_address_candidates(
            bh_symbol_match_kind::exact,
            exact_addresses,
            exact_overflow))
    {
        return result;
    }

    if (!exact_without_address.empty())
    {
        result.status = bh_symbol_resolution_status::no_static_address;
        result.match_kind = bh_symbol_match_kind::exact;
        result.candidates = exact_without_address;
        return result;
    }

    if (select_address_candidates(
            bh_symbol_match_kind::substring,
            substring_addresses,
            substring_overflow))
    {
        return result;
    }

    if (!substring_without_address.empty())
    {
        result.status = bh_symbol_resolution_status::no_static_address;
        result.match_kind = bh_symbol_match_kind::substring;
        result.candidates = substring_without_address;
        return result;
    }

    result.status = bh_symbol_resolution_status::symbol_not_found;
    return result;
}

std::wstring bh_get_native_module_name(DkmNativeModuleInstance* native_module)
{
    if (!native_module)
    {
        return L"<module>";
    }

    DkmString* module_name = native_module->Name();
    const wchar_t* module_text = DkmString::Value(module_name);
    return module_text && module_text[0] ? module_text : L"<module>";
}

std::wstring bh_safe_diagnostic_field(const std::wstring& text, size_t limit = 160)
{
    std::wstring safe;
    safe.reserve((std::min)(text.size(), limit));
    for (wchar_t ch : text)
    {
        if (safe.size() >= limit)
        {
            safe += L"...";
            break;
        }

        if (ch < L' ' || ch == L'\'' || ch == L'|')
        {
            safe += L' ';
        }
        else
        {
            safe += ch;
        }
    }
    return safe;
}

void bh_add_unique_module_name(
    std::vector<std::wstring>& modules,
    const std::wstring& module_name)
{
    for (const std::wstring& existing : modules)
    {
        if (_wcsicmp(existing.c_str(), module_name.c_str()) == 0)
        {
            return;
        }
    }

    modules.push_back(module_name);
}

bh_symbol_resolution_result bh_resolve_symbol_in_module(
    DkmNativeModuleInstance* native_module,
    const wchar_t* symbol_name,
    bh_agent_symbol_search::budget& search_budget)
{
    bh_symbol_resolution_result result =
        bh_resolve_symbol_with_dia(native_module, symbol_name, search_budget);
    const std::wstring module_name = bh_get_native_module_name(native_module);
    result.attempted_modules.push_back(module_name);
    if (!result.pdb_available)
    {
        result.unavailable_modules.push_back(module_name);
    }

    for (bh_symbol_candidate& candidate : result.candidates)
    {
        candidate.module_name = module_name;
    }

    if (result.status == bh_symbol_resolution_status::resolved ||
        result.status == bh_symbol_resolution_status::ambiguous ||
        result.status == bh_symbol_resolution_status::search_limited)
    {
        return result;
    }

    CComPtr<DkmString> export_name;
    HRESULT export_hr = DkmString::Create(symbol_name, &export_name);
    if (FAILED(export_hr) || !export_name)
    {
        return result;
    }

    CComPtr<DkmNativeInstructionAddress> export_address;
    export_hr = native_module->FindExportName(export_name, false, &export_address);
    if (FAILED(export_hr) || !export_address)
    {
        return result;
    }

    UINT64 absolute_address = native_module->BaseAddress() + export_address->RVA();
    auto cpu_address = export_address->CPUInstructionPart();
    if (cpu_address)
    {
        absolute_address = cpu_address->InstructionPointer;
    }
    if (absolute_address == 0)
    {
        return result;
    }

    bh_symbol_candidate candidate;
    candidate.address = absolute_address;
    candidate.module_name = module_name;
    candidate.compiland_name = L"<export>";
    candidate.decorated_name = symbol_name;
    candidate.address_status = bh_symbol_address_status::resolved;

    result.status = bh_symbol_resolution_status::resolved;
    result.match_kind = bh_symbol_match_kind::exact;
    result.address = absolute_address;
    result.candidate_overflow = false;
    result.candidates.clear();
    result.candidates.push_back(candidate);
    return result;
}

struct bh_symbol_resolution_accumulator
{
    std::vector<bh_symbol_candidate> exact_addresses;
    std::vector<bh_symbol_candidate> substring_addresses;
    std::vector<bh_symbol_candidate> exact_without_address;
    std::vector<bh_symbol_candidate> substring_without_address;
    std::vector<std::wstring> attempted_modules;
    std::vector<std::wstring> unavailable_modules;
    bool exact_overflow = false;
    bool substring_overflow = false;
    bool pdb_available = false;
    HRESULT dia_hr = E_FAIL;
};

void bh_accumulate_symbol_result(
    bh_symbol_resolution_accumulator& accumulator,
    const bh_symbol_resolution_result& module_result)
{
    if (module_result.pdb_available)
    {
        accumulator.pdb_available = true;
        accumulator.dia_hr = S_OK;
    }
    else if (!accumulator.pdb_available &&
             FAILED(module_result.dia_hr) &&
             accumulator.dia_hr == E_FAIL)
    {
        accumulator.dia_hr = module_result.dia_hr;
    }

    for (const std::wstring& module_name : module_result.attempted_modules)
    {
        bh_add_unique_module_name(accumulator.attempted_modules, module_name);
    }
    for (const std::wstring& module_name : module_result.unavailable_modules)
    {
        bh_add_unique_module_name(accumulator.unavailable_modules, module_name);
    }

    if (module_result.status == bh_symbol_resolution_status::resolved ||
        module_result.status == bh_symbol_resolution_status::ambiguous)
    {
        std::vector<bh_symbol_candidate>& candidates =
            module_result.match_kind == bh_symbol_match_kind::exact
                ? accumulator.exact_addresses
                : accumulator.substring_addresses;
        bool& overflow = module_result.match_kind == bh_symbol_match_kind::exact
            ? accumulator.exact_overflow
            : accumulator.substring_overflow;
        overflow = overflow || module_result.candidate_overflow;
        for (const bh_symbol_candidate& candidate : module_result.candidates)
        {
            bh_add_address_candidate(candidates, candidate, overflow);
        }
        return;
    }

    if (module_result.status == bh_symbol_resolution_status::no_static_address)
    {
        std::vector<bh_symbol_candidate>& candidates =
            module_result.match_kind == bh_symbol_match_kind::exact
                ? accumulator.exact_without_address
                : accumulator.substring_without_address;
        for (const bh_symbol_candidate& candidate : module_result.candidates)
        {
            bh_add_non_address_candidate(candidates, candidate);
        }
    }
}

bh_symbol_resolution_result bh_finish_symbol_accumulation(
    const bh_symbol_resolution_accumulator& accumulator)
{
    bh_symbol_resolution_result result;
    result.pdb_available = accumulator.pdb_available;
    result.dia_hr = accumulator.dia_hr;
    result.attempted_modules = accumulator.attempted_modules;
    result.unavailable_modules = accumulator.unavailable_modules;

    auto select_addresses = [&result](
        bh_symbol_match_kind match_kind,
        const std::vector<bh_symbol_candidate>& candidates,
        bool overflow) -> bool
    {
        if (candidates.empty() && !overflow)
        {
            return false;
        }

        result.match_kind = match_kind;
        result.candidates = candidates;
        result.candidate_overflow = overflow;
        if (candidates.size() == 1 && !overflow)
        {
            result.status = bh_symbol_resolution_status::resolved;
            result.address = candidates[0].address;
        }
        else
        {
            result.status = bh_symbol_resolution_status::ambiguous;
        }
        return true;
    };

    if (select_addresses(
            bh_symbol_match_kind::exact,
            accumulator.exact_addresses,
            accumulator.exact_overflow))
    {
        return result;
    }

    if (!accumulator.exact_without_address.empty())
    {
        result.status = bh_symbol_resolution_status::no_static_address;
        result.match_kind = bh_symbol_match_kind::exact;
        result.candidates = accumulator.exact_without_address;
        return result;
    }

    if (select_addresses(
            bh_symbol_match_kind::substring,
            accumulator.substring_addresses,
            accumulator.substring_overflow))
    {
        return result;
    }

    if (!accumulator.substring_without_address.empty())
    {
        result.status = bh_symbol_resolution_status::no_static_address;
        result.match_kind = bh_symbol_match_kind::substring;
        result.candidates = accumulator.substring_without_address;
        return result;
    }

    result.status = accumulator.pdb_available
        ? bh_symbol_resolution_status::symbol_not_found
        : bh_symbol_resolution_status::symbols_unavailable;
    return result;
}

const wchar_t* bh_match_kind_text(bh_symbol_match_kind match_kind)
{
    switch (match_kind)
    {
    case bh_symbol_match_kind::exact:
        return L"exact";
    case bh_symbol_match_kind::substring:
        return L"substring";
    default:
        return L"none";
    }
}

const wchar_t* bh_address_status_text(bh_symbol_address_status status)
{
    switch (status)
    {
    case bh_symbol_address_status::non_static_data:
        return L"non-static-data";
    case bh_symbol_address_status::non_code_public:
        return L"non-code-public";
    case bh_symbol_address_status::missing_address:
        return L"missing-rva-va";
    default:
        return L"resolved";
    }
}

std::wstring bh_format_symbol_resolution_error(
    const wchar_t* requested_symbol,
    const bh_symbol_resolution_result& result)
{
    const wchar_t* error_code = L"symbol_not_found";
    switch (result.status)
    {
    case bh_symbol_resolution_status::symbols_unavailable:
        error_code = L"symbol_pdb_unavailable";
        break;
    case bh_symbol_resolution_status::no_static_address:
        error_code = L"symbol_no_static_address";
        break;
    case bh_symbol_resolution_status::ambiguous:
        error_code = L"symbol_ambiguous";
        break;
    case bh_symbol_resolution_status::search_limited:
        error_code = L"symbol_search_limited";
        break;
    default:
        break;
    }

    std::wstring text = L"error: ";
    text += error_code;
    text += L" | symbol='";
    text += bh_safe_diagnostic_field(requested_symbol ? requested_symbol : L"");
    text += L"' | modules='";
    const size_t module_count = (std::min)(result.attempted_modules.size(), (size_t)8);
    for (size_t index = 0; index < module_count; ++index)
    {
        if (index != 0)
        {
            text += L",";
        }
        text += bh_safe_diagnostic_field(result.attempted_modules[index], 96);
    }
    if (result.attempted_modules.size() > module_count)
    {
        text += L",...";
    }
    text += L"' | pdb=";
    if (result.pdb_available && !result.unavailable_modules.empty())
    {
        text += L"partial";
    }
    else
    {
        text += result.pdb_available ? L"loaded" : L"unavailable";
    }
    text += L" | match=";
    text += bh_match_kind_text(result.match_kind);
    text += L" | search='exact-decorated,exact-undecorated,substring-decorated,substring-undecorated'";

    if (result.status == bh_symbol_resolution_status::search_limited)
    {
        text += L" | limit=";
        switch (result.search_limit)
        {
        case bh_agent_symbol_search::limit::elapsed_time:
            text += L"time";
            break;
        case bh_agent_symbol_search::limit::symbol_work:
            text += L"symbols";
            break;
        case bh_agent_symbol_search::limit::modules:
            text += L"modules";
            break;
        default:
            text += L"unknown";
            break;
        }
        wchar_t counters[96] = {};
        swprintf_s(
            counters,
            L" | examined_symbols=%llu | examined_modules=%llu",
            (unsigned long long)result.symbols_examined,
            (unsigned long long)result.modules_examined);
        text += counters;
    }

    if (FAILED(result.dia_hr))
    {
        wchar_t hr_text[32] = {};
        swprintf_s(hr_text, L" | dia_hr=0x%08X", (unsigned)result.dia_hr);
        text += hr_text;
    }

    if (!result.candidates.empty())
    {
        text += L" | candidates='";
        const size_t candidate_count = (std::min)(result.candidates.size(), (size_t)4);
        for (size_t index = 0; index < candidate_count; ++index)
        {
            if (index != 0)
            {
                text += L"; ";
            }

            const bh_symbol_candidate& candidate = result.candidates[index];
            text += bh_safe_diagnostic_field(candidate.module_name, 96);
            text += L"!";
            const std::wstring& candidate_name = !candidate.decorated_name.empty()
                ? candidate.decorated_name
                : candidate.undecorated_name;
            text += bh_safe_diagnostic_field(candidate_name, 160);
            if (candidate.address != 0)
            {
                wchar_t address_text[32] = {};
                swprintf_s(
                    address_text,
                    L"@0x%llX",
                    (unsigned long long)candidate.address);
                text += address_text;
            }
            text += L" compiland=";
            text += bh_safe_diagnostic_field(candidate.compiland_name, 120);
            text += L" status=";
            text += bh_address_status_text(candidate.address_status);
            if (candidate.address_status == bh_symbol_address_status::non_static_data)
            {
                wchar_t location_text[32] = {};
                swprintf_s(location_text, L"(%u)", (unsigned)candidate.location_type);
                text += location_text;
            }
        }
        if (result.candidates.size() > candidate_count || result.candidate_overflow)
        {
            text += L"; ...";
        }
        text += L"'";
    }

    return text;
}

HRESULT bh_create_worker_reply(
    DkmProcess* process,
    UINT32 message_code,
    const wchar_t* text,
    DkmCustomMessage** reply_out)
{
    if (!process || !text)
    {
        return E_INVALIDARG;
    }

    CComPtr<DkmVariant> value;
    HRESULT hr = DkmVariant::Create(text, &value);
    if (FAILED(hr) || !value)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    CComPtr<DkmCustomMessage> reply;
    hr = DkmCustomMessage::Create(
        process->Connection(),
        process,
        bh_guid_agent_message_to_local_worker,
        message_code,
        value,
        nullptr,
        &reply);
    if (FAILED(hr) || !reply)
    {
        return FAILED(hr) ? hr : E_FAIL;
    }

    if (reply_out)
    {
        *reply_out = reply.Detach();
    }
    return S_OK;
}

HRESULT bh_resolve_address_with_dia(DkmNativeModuleInstance* native_module, UINT32 rva, std::wstring& symbol_text)
{
	symbol_text.clear();

	if (!native_module)
	{
		return E_INVALIDARG;
	}

	CComPtr<IDiaSession> dia_session;
	HRESULT hr = bh_get_dia_session(native_module, &dia_session);
	if (FAILED(hr) || !dia_session)
	{
		return FAILED(hr) ? hr : E_FAIL;
	}

	CComPtr<IDiaSymbol> symbol;
	LONG displacement = 0;
	hr = dia_session->findSymbolByRVAEx(rva, SymTagNull, &symbol, &displacement);
	if (FAILED(hr) || !symbol)
	{
		return FAILED(hr) ? hr : E_FAIL;
	}

	CComBSTR function_name;
	if (FAILED(symbol->get_undecoratedName(&function_name)) || !function_name || !function_name[0])
	{
		if (FAILED(symbol->get_name(&function_name)) || !function_name || !function_name[0])
		{
			return E_FAIL;
		}
	}

	DkmString* module_name = native_module->Name();
	const wchar_t* module_text = DkmString::Value(module_name);
	if (!module_text || !module_text[0])
	{
		module_text = L"<module>";
	}

	wchar_t buffer[1024] = {};
	if (displacement > 0)
	{
		swprintf_s(buffer,
			L"%s!%s+0x%llX",
			module_text,
			(const wchar_t*)function_name,
			(unsigned long long)displacement);
	}
	else if (displacement < 0)
	{
		swprintf_s(buffer,
			L"%s!%s-0x%llX",
			module_text,
			(const wchar_t*)function_name,
			(unsigned long long)(-displacement));
	}
	else
	{
		swprintf_s(buffer,
			L"%s!%s",
			module_text,
			(const wchar_t*)function_name);
	}

	symbol_text = buffer;
	return S_OK;
}
}

HRESULT STDMETHODCALLTYPE bh_agent_local_worker_component::SendLower(
    DkmCustomMessage* pCustomMessage,
    DkmCustomMessage** ppReplyMessage)
{
    if (!pCustomMessage)
    {
        bh_agent_logf("engine.local_worker", "SendLower missing custom message");
        return E_POINTER;
    }

	UINT32 message_code = pCustomMessage->MessageCode();
	if (message_code != bh_message_to_local_worker_resolve_symbol &&
		message_code != bh_message_to_local_worker_resolve_address)
	{
		bh_agent_logf("engine.local_worker", "SendLower ignored messageCode=%u", (unsigned)message_code);
		return S_OK;
	}

	bh_agent_logf("engine.local_worker", "SendLower worker request code=%u", (unsigned)message_code);

    DkmProcess* process = pCustomMessage->Process();
    if (!process)
    {
        bh_agent_logf("engine.local_worker", "SendLower missing process");
        return E_FAIL;
    }

    DkmVariant* symbol_variant = pCustomMessage->Parameter1();
    DkmVariant* module_variant = pCustomMessage->Parameter2();
    if (!symbol_variant || !module_variant)
    {
        bh_agent_logf("engine.local_worker", "missing symbol/module variant");
        return E_INVALIDARG;
    }

	const VARIANT& value1 = symbol_variant->Value();
	const VARIANT& module_value = module_variant->Value();
	// An empty module name (a null BSTR is the empty string) means: search every loaded native module.
	if (value1.vt != VT_BSTR || !value1.bstrVal || module_value.vt != VT_BSTR)
	{
		bh_agent_logf("engine.local_worker", "invalid request/module variant types");
		return E_INVALIDARG;
	}

    CComPtr<DkmNativeRuntimeInstance> runtime;
    HRESULT hr = process->GetNativeRuntimeInstance(&runtime);
    if (FAILED(hr) || !runtime)
    {
        bh_agent_logf("engine.local_worker", "GetNativeRuntimeInstance failed hr=0x%08X", (unsigned)hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

	std::wstring requested_symbol = value1.bstrVal;
	std::wstring requested_module = module_value.bstrVal ? module_value.bstrVal : L"";
	bool module_qualified = false;
	if (message_code == bh_message_to_local_worker_resolve_symbol)
	{
		const size_t qualifier_separator = requested_symbol.find(L'!');
		if (qualifier_separator != std::wstring::npos)
		{
			if (qualifier_separator == 0 ||
				qualifier_separator + 1 >= requested_symbol.size())
			{
				bh_agent_logf("engine.local_worker", "invalid module-qualified symbol");
				return E_INVALIDARG;
			}

			requested_module = requested_symbol.substr(0, qualifier_separator);
			requested_symbol.erase(0, qualifier_separator + 1);
			module_qualified = true;
		}
	}

	CComPtr<DkmNativeModuleInstance> native_module;
	if (!requested_module.empty())
	{
		CComPtr<DkmString> module_name;
		hr = DkmString::Create(requested_module.c_str(), &module_name);
		if (SUCCEEDED(hr) && module_name)
		{
			CAutoDkmArray<DkmModuleInstance*> modules;
			hr = runtime->FindModulesByName(module_name, &modules);
			if (SUCCEEDED(hr))
			{
				for (UINT32 i = 0; i < modules.Length; ++i)
				{
					native_module = DkmNativeModuleInstance::TryCast(modules.Members[i]);
					if (native_module)
					{
						break;
					}
				}
			}
		}
	}

	if (module_qualified && !native_module)
	{
		bh_agent_logf("engine.local_worker",
			"module-qualified symbol module not found");
		std::wstring error_text = L"error: symbol_module_not_found | symbol='";
		error_text += bh_safe_diagnostic_field(requested_symbol);
		error_text += L"' | module='";
		error_text += bh_safe_diagnostic_field(requested_module);
		error_text += L"' | pdb=not-checked | search='exact-decorated,exact-undecorated,substring-decorated,substring-undecorated'";
		return bh_create_worker_reply(
			process,
			message_code,
			error_text.c_str(),
			ppReplyMessage);
	}

	if (message_code == bh_message_to_local_worker_resolve_symbol)
	{
		bh_agent_symbol_search::budget search_budget;
		bh_symbol_resolution_result resolution;
		bool has_preferred_result = false;
		if (native_module)
		{
			resolution = bh_resolve_symbol_in_module(
				native_module,
				requested_symbol.c_str(),
				search_budget);
			has_preferred_result = true;
			if ((resolution.status == bh_symbol_resolution_status::resolved ||
				 resolution.status == bh_symbol_resolution_status::ambiguous) &&
				resolution.match_kind == bh_symbol_match_kind::exact)
			{
				goto symbol_resolution_complete;
			}
		}

		if (module_qualified)
		{
			goto symbol_resolution_complete;
		}

		{
			bh_symbol_resolution_accumulator accumulator;
			if (has_preferred_result)
			{
				bh_accumulate_symbol_result(accumulator, resolution);
			}

			CAutoDkmArray<DkmNativeModuleInstance*> native_modules;
			hr = runtime->GetNativeModuleInstances(&native_modules);
			if (FAILED(hr) || native_modules.Length == 0)
			{
				bh_agent_logf(
					"engine.local_worker",
					"GetNativeModuleInstances failed hr=0x%08X",
					(unsigned)hr);
				if (!has_preferred_result)
				{
					resolution = bh_symbol_resolution_result();
					resolution.status = bh_symbol_resolution_status::symbols_unavailable;
					resolution.dia_hr = FAILED(hr) ? hr : E_FAIL;
				}
				goto symbol_resolution_complete;
			}

			for (UINT32 index = 0; index < native_modules.Length; ++index)
			{
				DkmNativeModuleInstance* module_instance = native_modules.Members[index];
				if (!module_instance || module_instance == native_module.p)
				{
					continue;
				}

				bh_symbol_resolution_result module_result =
					bh_resolve_symbol_in_module(
						module_instance,
						requested_symbol.c_str(),
						search_budget);
				bh_accumulate_symbol_result(accumulator, module_result);
				if (module_result.status == bh_symbol_resolution_status::search_limited)
				{
					break;
				}
			}

			resolution = bh_finish_symbol_accumulation(accumulator);
			if (search_budget.exhausted())
			{
				bh_mark_symbol_search_limited(resolution, search_budget);
			}

		}

	symbol_resolution_complete:
		if (resolution.status != bh_symbol_resolution_status::resolved ||
			resolution.address == 0)
		{
			std::wstring error_text =
				bh_format_symbol_resolution_error(requested_symbol.c_str(), resolution);
			bh_agent_logf(
				"engine.local_worker",
				"resolve symbol diagnostic status=%u",
				(unsigned)resolution.status);
			return bh_create_worker_reply(
				process,
				message_code,
				error_text.c_str(),
				ppReplyMessage);
		}

		wchar_t address_text[64] = {};
		swprintf_s(
			address_text,
			L"0x%llX",
			(unsigned long long)resolution.address);
		bh_agent_logf(
			"engine.local_worker",
			"resolved symbol to 0x%llX",
			(unsigned long long)resolution.address);
		return bh_create_worker_reply(
			process,
			message_code,
			address_text,
			ppReplyMessage);
	}

	UINT64 absolute_address = 0;
	if (wcsncmp(value1.bstrVal, L"0x", 2) == 0 || wcsncmp(value1.bstrVal, L"0X", 2) == 0)
	{
		absolute_address = _wcstoui64(value1.bstrVal + 2, nullptr, 16);
	}
	else
	{
		absolute_address = _wcstoui64(value1.bstrVal, nullptr, 0);
	}

	if (!native_module)
	{
		bh_agent_logf("engine.local_worker", "resolve address module not found");
		return E_FAIL;
	}

	UINT64 module_base = native_module->BaseAddress();
	if (absolute_address < module_base)
	{
		bh_agent_logf("engine.local_worker", "resolve address below module base");
		return E_INVALIDARG;
	}

	UINT64 rva64 = absolute_address - module_base;
	if (rva64 > 0xFFFFFFFFull)
	{
		bh_agent_logf("engine.local_worker", "resolve address rva overflow");
		return E_INVALIDARG;
	}

	std::wstring resolved_symbol;
	hr = bh_resolve_address_with_dia(native_module, (UINT32)rva64, resolved_symbol);
	if (FAILED(hr) || resolved_symbol.empty())
	{
		bh_agent_logf("engine.local_worker", "resolve address with DIA failed hr=0x%08X", (unsigned)hr);
		return FAILED(hr) ? hr : E_FAIL;
	}

	CComPtr<DkmVariant> symbol_variant_out;
	hr = DkmVariant::Create(resolved_symbol.c_str(), &symbol_variant_out);
	if (FAILED(hr) || !symbol_variant_out)
	{
		bh_agent_logf("engine.local_worker", "DkmVariant::Create symbol failed hr=0x%08X", (unsigned)hr);
		return FAILED(hr) ? hr : E_FAIL;
	}

	CComPtr<DkmCustomMessage> reply;
	hr = DkmCustomMessage::Create(
		process->Connection(),
		process,
		bh_guid_agent_message_to_local_worker,
		message_code,
		symbol_variant_out,
		nullptr,
		&reply);
	if (FAILED(hr) || !reply)
	{
		bh_agent_logf("engine.local_worker", "Create resolve address reply failed hr=0x%08X", (unsigned)hr);
		return FAILED(hr) ? hr : E_FAIL;
	}

	bh_agent_logf("engine.local_worker", "resolved address 0x%llX to symbol", (unsigned long long)absolute_address);

	if (ppReplyMessage)
	{
		*ppReplyMessage = reply.Detach();
	}

	return S_OK;
}
