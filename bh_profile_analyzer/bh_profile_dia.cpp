#include <windows.h>
#include <atlbase.h>
#include <dia2.h>
#include <cvconst.h>
#include <memory>
#include <new>

// Fixed versioned ABI; no C++/COM object crosses into managed code except an
// opaque, thread-affine session handle. Only local PDBs are opened, with GUID/age
// validation. No registry COM activation, symbol servers or source retrieval.
struct source_frame
{
    DWORD id, parent, line, column, flags;
    wchar_t name[512], file[1024];
};
struct source_result
{
    DWORD size, count, flags;
    source_frame frames[17]; // physical function plus at most 16 inline contexts
};
struct dia_context
{
    HMODULE module = nullptr;
    bool uninitialize = false;
    DWORD thread = GetCurrentThreadId();
    CComPtr<IDiaDataSource> source;
    CComPtr<IDiaSession> session;
    ~dia_context()
    {
        session.Release(); source.Release();
        if (module) FreeLibrary(module);
        if (uninitialize) CoUninitialize();
    }
};
extern "C" __declspec(dllexport) void __cdecl bh_dia_close(dia_context* context)
{
    if (context && context->thread == GetCurrentThreadId()) delete context;
}
extern "C" __declspec(dllexport) HRESULT __cdecl bh_dia_open(const wchar_t* dll, const wchar_t* pdb, GUID guid, DWORD age, dia_context** result)
{
    if (!dll || !pdb || !result) return E_POINTER;
    *result = nullptr;
    try
    {
        auto context = std::make_unique<dia_context>();
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return hr;
        context->uninitialize = SUCCEEDED(hr);
        context->module = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!context->module) return HRESULT_FROM_WIN32(GetLastError());
        using get_factory = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
        auto get = reinterpret_cast<get_factory>(GetProcAddress(context->module, "DllGetClassObject"));
        if (!get) return E_NOINTERFACE;
        CComPtr<IClassFactory> factory;
        hr = get(__uuidof(DiaSource), IID_IClassFactory, reinterpret_cast<void**>(&factory));
        if (FAILED(hr)) return hr;
        hr = factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), reinterpret_cast<void**>(&context->source));
        if (FAILED(hr)) return hr;
        hr = context->source->loadAndValidateDataFromPdb(pdb, &guid, 0, age);
        if (FAILED(hr)) return hr;
        hr = context->source->openSession(&context->session);
        if (FAILED(hr)) return hr;
        *result = context.release();
        return S_OK;
    }
    catch (...) { return E_OUTOFMEMORY; }
}
template <size_t N> static void copy_text(wchar_t (&target)[N], BSTR text, DWORD& flags)
{
    if (!text) return;
    if (SysStringLen(text) >= N) flags |= 2;
    wcsncpy_s(target, text, _TRUNCATE);
}
static void read_line(IDiaEnumLineNumbers* lines, source_frame& frame)
{
    if (!lines) return;
    // Multiple distinct locations at one RVA are ambiguous, not a reason to
    // arbitrarily choose a line. Duplicated identical records are harmless.
    for (unsigned n = 0; n < 65; ++n)
    {
        CComPtr<IDiaLineNumber> item;
        ULONG fetched = 0;
        const HRESULT hr = lines->Next(1, &item, &fetched);
        if (FAILED(hr)) { frame.flags |= 1; break; }
        if (!fetched) break;
        if (n == 64) { frame.flags |= 1; break; }
        DWORD line = 0, column = 0;
        CComPtr<IDiaSourceFile> source;
        CComBSTR file;
        item->get_lineNumber(&line); item->get_columnNumber(&column);
        if (FAILED(item->get_sourceFile(&source)) || !source || FAILED(source->get_fileName(&file)) || !file || !SysStringLen(file) || !line) continue;
        if (frame.line && (frame.line != line || wcscmp(frame.file, file))) { frame.flags |= 1; break; }
        frame.line = line; frame.column = column;
        copy_text(frame.file, file, frame.flags);
    }
    if (frame.flags) { frame.line = 0; frame.column = 0; frame.file[0] = 0; }
}
extern "C" __declspec(dllexport) HRESULT __cdecl bh_dia_resolve(dia_context* context, DWORD rva, source_result* result)
{
    if (!context || !result || result->size != sizeof(source_result)) return E_INVALIDARG;
    if (context->thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    try
    {
        ZeroMemory(result, sizeof(*result)); result->size = sizeof(*result);
        CComPtr<IDiaSymbol> physical;
        HRESULT hr = context->session->findSymbolByRVA(rva, SymTagFunction, &physical);
        if (hr != S_OK || !physical) return S_FALSE;
        auto& first = result->frames[0];
        physical->get_symIndexId(&first.id);
        CComBSTR name; physical->get_name(&name); copy_text(first.name, name, first.flags);
        CComPtr<IDiaEnumLineNumbers> lines;
        if (SUCCEEDED(context->session->findLinesByRVA(rva, 1, &lines))) read_line(lines, first);
        result->count = 1;
        CComPtr<IDiaEnumSymbols> inlines;
        hr = context->session->findInlineFramesByRVA(physical, rva, &inlines);
        if (FAILED(hr)) { result->flags |= 1; return S_OK; }
        if (!inlines) return S_OK;
        for (unsigned n = 0; n < 17; ++n)
        {
            CComPtr<IDiaSymbol> symbol;
            ULONG fetched = 0;
            hr = inlines->Next(1, &symbol, &fetched);
            if (FAILED(hr)) { result->flags |= 1; break; }
            if (!fetched) break;
            if (n == 16) { result->flags |= 2; break; }
            auto& frame = result->frames[result->count++];
            symbol->get_symIndexId(&frame.id);
            CComBSTR inline_name; symbol->get_name(&inline_name); copy_text(frame.name, inline_name, frame.flags);
            CComPtr<IDiaSymbol> parent;
            symbol->get_lexicalParent(&parent);
            for (unsigned depth = 0; parent && depth < 64; ++depth)
            {
                DWORD tag = 0, id = 0;
                parent->get_symTag(&tag); parent->get_symIndexId(&id);
                if (tag == SymTagInlineSite || tag == SymTagFunction) { frame.parent = id; break; }
                CComPtr<IDiaSymbol> next; parent->get_lexicalParent(&next); parent = next;
            }
            CComPtr<IDiaEnumLineNumbers> inline_lines;
            if (SUCCEEDED(context->session->findInlineeLinesByRVA(symbol, rva, 1, &inline_lines))) read_line(inline_lines, frame);
        }
        return S_OK;
    }
    catch (...) { return E_FAIL; }
}
