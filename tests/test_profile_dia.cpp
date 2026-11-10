#include "../bh_profile_analyzer/bh_profile_dia.cpp"
#include <cstdio>
#include <cstdlib>
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
int wmain(int argc, wchar_t** argv)
{
    check(argc == 3, "DLL and fixture PDB arguments required");
    // Obtain metadata from our own compiler-produced PDB for the identity test.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(dll != nullptr, "load package DIA");
    using factory_fn = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto get = reinterpret_cast<factory_fn>(GetProcAddress(dll, "DllGetClassObject"));
    check(get != nullptr, "DIA class factory");
    CComPtr<IClassFactory> factory;
    check(SUCCEEDED(get(__uuidof(DiaSource), IID_IClassFactory, reinterpret_cast<void**>(&factory))), "class factory");
    CComPtr<IDiaDataSource> source;
    check(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), reinterpret_cast<void**>(&source))), "create data source");
    check(SUCCEEDED(source->loadDataFromPdb(argv[2])), "load fixture PDB");
    CComPtr<IDiaSession> session;
    check(SUCCEEDED(source->openSession(&session)), "fixture session");
    CComPtr<IDiaSymbol> global;
    session->get_globalScope(&global);
    GUID guid = {}; DWORD age = 0;
    check(SUCCEEDED(global->get_guid(&guid)) && SUCCEEDED(global->get_age(&age)), "PDB identity");
    dia_context* context = nullptr;
    check(SUCCEEDED(bh_dia_open(argv[1], argv[2], guid, age, &context)) && context, "validated native open");
    GUID wrong = guid; wrong.Data1 ^= 1;
    dia_context* bad = nullptr;
    check(FAILED(bh_dia_open(argv[1], argv[2], wrong, age, &bad)) && !bad, "mismatched GUID rejected");
    check(FAILED(bh_dia_open(argv[1], argv[2], guid, age + 1, &bad)) && !bad, "mismatched age rejected");
    CComPtr<IDiaEnumSymbols> functions;
    check(SUCCEEDED(session->findChildren(global, SymTagFunction, L"source_drilldown_root", nsCaseSensitive, &functions)), "find root");
    CComPtr<IDiaSymbol> function;
    ULONG count = 0; functions->Next(1, &function, &count);
    check(count == 1 && function, "root symbol");
    DWORD rva = 0; ULONGLONG length = 0;
    function->get_relativeVirtualAddress(&rva); function->get_length(&length);
    check(length > 0 && length <= 8192, "bounded fixture code range");
    unsigned line_locations = 0, inline_locations = 0, nested_locations = 0;
    for (DWORD offset = 0; offset < length; ++offset)
    {
        source_result result = {}; result.size = sizeof(result);
        check(SUCCEEDED(bh_dia_resolve(context, rva + offset, &result)), "resolve RVA");
        check(result.count <= 17 && result.flags == 0, "bounded complete inline enumeration");
        if (result.count && result.frames[0].line) ++line_locations;
        if (result.count > 1) ++inline_locations;
        if (result.count > 2)
        {
            ++nested_locations;
            check(result.frames[0].line == 16, "physical call site matches fixture loop");
        }
        for (DWORD i = 1; i < result.count; ++i)
        {
            bool parent_found = false;
            for (DWORD j = 0; j < result.count; ++j) if (result.frames[j].id == result.frames[i].parent) parent_found = true;
            check(parent_found, "inline lexical parent present");
            check(result.frames[i].name[0] && result.frames[i].line, "inline name and line available");
            if (!wcscmp(result.frames[i].name, L"source_inline_inner"))
                check(result.frames[i].line >= 3 && result.frames[i].line <= 5, "inner leaf source range");
            if (!wcscmp(result.frames[i].name, L"source_inline_outer") && result.count > 2)
                check(result.frames[i].line == 9 || result.frames[i].line == 11, "outer inline call site");
        }
    }
    std::printf("fixture codeBytes=%llu lineLocations=%u inlineLocations=%u nestedLocations=%u\n", length, line_locations, inline_locations, nested_locations);
    check(line_locations > 0 && inline_locations > 0 && nested_locations > 0, "forced nested inline chains resolved");
    bh_dia_close(context);
    function.Release(); functions.Release(); global.Release(); session.Release(); source.Release(); factory.Release();
    FreeLibrary(dll); CoUninitialize();
    std::puts("PASS: native DIA line/inline identity validation");
}
