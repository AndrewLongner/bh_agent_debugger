#pragma once

// Side-effect-free expression evaluation through Concord.
//
// The EnvDTE evaluator may assign variables or call functions in the debuggee. The default
// eval path instead takes the Concord frame behind Visual Studio's current stack frame and
// evaluates with DkmEvaluationFlags::NoSideEffects | NoFuncEval. Each expression evaluator
// decides what counts as a side effect, so only Microsoft evaluators known to honor those
// flags are accepted; every other case fails closed.
//
// IVsWrappedDkmStackFrame may only be used on the Visual Studio main thread, so the frame is
// captured there through a message-only window. Evaluation itself runs on the calling thread.
//
// Requires the package precompiled header (ATL, EnvDTE) before inclusion.

#include <vsdebugeng.h>
#include <vsdebugeng.ad7interop.h>
#include <atomic>
#include <new>
#include <string>
#include <vector>

namespace bh_eval
{
    namespace dkm = Microsoft::VisualStudio::Debugger;

    // Main-thread dispatch stays below the 5 s pipe-thread shutdown wait in PreClosing.
    constexpr DWORD dispatch_timeout_ms = 3000;
    constexpr UINT32 inspection_timeout_ms = 5000;
    constexpr DWORD completion_timeout_ms = 6000;

    inline std::string response_text(const wchar_t* text, size_t max_bytes)
    {
        std::string utf8;
        if (text && *text)
        {
            const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (length > 1)
            {
                utf8.resize(static_cast<size_t>(length) - 1);
                if (WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], length, nullptr, nullptr) <= 0)
                {
                    utf8.clear();
                }
            }
        }

        for (char& ch : utf8)
        {
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                ch = ' ';
            }
        }

        if (utf8.size() > max_bytes)
        {
            size_t cut = max_bytes;
            while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80)
            {
                --cut;
            }
            utf8.resize(cut);
            utf8 += "...";
        }

        return utf8.empty() ? std::string("<unknown>") : utf8;
    }

    inline std::string hr_error(const char* what, HRESULT hr)
    {
        char text[160] = {};
        _snprintf_s(text, sizeof(text), _TRUNCATE, "error: eval %s hr=0x%08X", what, static_cast<unsigned>(hr));
        return text;
    }

    inline std::wstring text_or_empty(dkm::DkmString* text)
    {
        const wchar_t* value = dkm::DkmString::Value(text);
        return value ? std::wstring(value) : std::wstring();
    }

    // Frame lookup handed to the main thread. Reference counted because a timed-out
    // SendMessageTimeout can still be dispatched later.
    struct frame_request
    {
        std::atomic<long> refs{ 1 };
        std::atomic<bool> done{ false };
        CComPtr<IServiceProvider> provider;
        std::string error;
        CComPtr<dkm::CallStack::DkmStackFrame> frame;

        void add_ref()
        {
            ++refs;
        }

        void release()
        {
            if (--refs == 0)
            {
                delete this;
            }
        }
    };

    // Main thread only.
    inline void capture_frame(frame_request& request)
    {
        if (!request.provider)
        {
            request.error = "error: no service provider";
            return;
        }

        CComPtr<_DTE> dte;
        HRESULT hr = request.provider->QueryService(SID_SDTE, __uuidof(_DTE), reinterpret_cast<void**>(&dte));
        if (FAILED(hr) || !dte)
        {
            request.error = "error: could not get DTE";
            return;
        }

        CComPtr<VxDTE::Window> main_window;
        long main_hwnd = 0;
        if (FAILED(dte->get_MainWindow(&main_window)) || !main_window ||
            FAILED(main_window->get_HWnd(&main_hwnd)) || main_hwnd == 0 ||
            GetWindowThreadProcessId(reinterpret_cast<HWND>(static_cast<LONG_PTR>(main_hwnd)), nullptr) != GetCurrentThreadId())
        {
            request.error = "error: eval dispatcher is not on the Visual Studio main thread";
            return;
        }

        CComPtr<::Debugger> debugger;
        hr = dte->get_Debugger(&debugger);
        if (FAILED(hr) || !debugger)
        {
            request.error = "error: could not get debugger";
            return;
        }

        dbgDebugMode mode = dbgDesignMode;
        hr = debugger->get_CurrentMode(&mode);
        if (FAILED(hr))
        {
            request.error = "error: could not get debugger mode";
            return;
        }

        if (mode != dbgBreakMode)
        {
            request.error = "error: eval requires break mode";
            return;
        }

        CComPtr<::StackFrame> frame;
        hr = debugger->get_CurrentStackFrame(&frame);
        if (FAILED(hr) || !frame)
        {
            request.error = "error: no active stack frame";
            return;
        }

        CComPtr<dkm::CallStack::IVsWrappedDkmStackFrame> wrapped;
        CComPtr<dkm::CallStack::DkmStackFrame> dkm_frame;
        hr = frame.QueryInterface(&wrapped);
        if (SUCCEEDED(hr) && wrapped)
        {
            hr = wrapped->GetDkmFrameObject(&dkm_frame);
        }

        if (hr != S_OK || !dkm_frame)
        {
            request.error = "error: eval current frame has no Concord frame; side-effect blocking unavailable (eval --side-effects <expr> allows side effects)";
            return;
        }

        request.frame = dkm_frame;
    }

    // Message-only window owned by the Visual Studio main thread. The message carries no
    // pointer: it only drains this object's queue, so a spoofed message is harmless.
    class ui_dispatcher
    {
    public:
        ui_dispatcher() = default;
        ui_dispatcher(const ui_dispatcher&) = delete;
        ui_dispatcher& operator=(const ui_dispatcher&) = delete;

        ~ui_dispatcher()
        {
            stop();
        }

        // Main thread.
        bool start()
        {
            {
                CComCritSecLock<CComAutoCriticalSection> guard(lock_);
                if (hwnd_)
                {
                    return true;
                }
            }

            if (dispatch_message() == 0)
            {
                return false;
            }

            HINSTANCE instance = ATL::_AtlBaseModule.GetModuleInstance();
            WNDCLASSEXW existing = {};
            existing.cbSize = sizeof(existing);
            if (GetClassInfoExW(instance, class_name(), &existing))
            {
                if (existing.lpfnWndProc != &ui_dispatcher::window_proc)
                {
                    return false;
                }
            }
            else
            {
                WNDCLASSEXW window_class = {};
                window_class.cbSize = sizeof(window_class);
                window_class.lpfnWndProc = &ui_dispatcher::window_proc;
                window_class.hInstance = instance;
                window_class.lpszClassName = class_name();
                if (!RegisterClassExW(&window_class))
                {
                    return false;
                }
            }

            HWND hwnd = CreateWindowExW(0, class_name(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
            if (!hwnd)
            {
                return false;
            }

            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            CComCritSecLock<CComAutoCriticalSection> guard(lock_);
            hwnd_ = hwnd;
            return true;
        }

        // Main thread. Safe to call more than once.
        void stop()
        {
            HWND hwnd = nullptr;
            std::vector<frame_request*> pending;
            {
                CComCritSecLock<CComAutoCriticalSection> guard(lock_);
                hwnd = hwnd_;
                hwnd_ = nullptr;
                pending.swap(pending_);
            }

            for (frame_request* request : pending)
            {
                request->release();
            }

            if (hwnd)
            {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                if (!DestroyWindow(hwnd))
                {
                    (void)PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
            }
        }

        // Any thread. On success the caller owns one reference to request.
        bool capture(IServiceProvider* provider, frame_request*& request, std::string& error)
        {
            request = nullptr;
            frame_request* pending = new (std::nothrow) frame_request();
            if (!pending)
            {
                error = "error: eval out of memory";
                return false;
            }

            pending->provider = provider;
            HWND hwnd = nullptr;
            {
                CComCritSecLock<CComAutoCriticalSection> guard(lock_);
                hwnd = hwnd_;
                if (hwnd)
                {
                    pending->add_ref();
                    pending_.push_back(pending);
                }
            }

            if (!hwnd)
            {
                pending->release();
                error = "error: eval main-thread dispatcher unavailable";
                return false;
            }

            DWORD_PTR ignored = 0;
            (void)SendMessageTimeoutW(hwnd, dispatch_message(), 0, 0,
                SMTO_NORMAL | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, dispatch_timeout_ms, &ignored);
            if (!pending->done.load())
            {
                // The queue keeps its reference until the main thread drains it or stop runs.
                pending->release();
                error = "error: eval main-thread dispatch timeout";
                return false;
            }

            request = pending;
            return true;
        }

    private:
        static UINT dispatch_message()
        {
            static const UINT message = RegisterWindowMessageW(L"bh_agent_debugger.eval.frame_request");
            return message;
        }

        static const wchar_t* class_name()
        {
            return L"bh_agent_debugger_eval_dispatch";
        }

        static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            const UINT dispatch = dispatch_message();
            if (dispatch != 0 && message == dispatch)
            {
                ui_dispatcher* self = reinterpret_cast<ui_dispatcher*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (self)
                {
                    self->drain();
                }
                return 1;
            }

            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        void drain()
        {
            std::vector<frame_request*> pending;
            {
                CComCritSecLock<CComAutoCriticalSection> guard(lock_);
                pending.swap(pending_);
            }

            for (frame_request* request : pending)
            {
                capture_frame(*request);
                request->done.store(true);
                request->release();
            }
        }

        CComAutoCriticalSection lock_;
        HWND hwnd_ = nullptr;
        std::vector<frame_request*> pending_;
    };

    // Receives the evaluation result. If the waiter abandons it after a timeout, the late
    // completion closes the result, expression, and inspection session itself.
    class evaluate_completion : public dkm::IDkmCompletionRoutine<dkm::Evaluation::DkmEvaluateExpressionAsyncResult>
    {
    public:
        static evaluate_completion* create(
            dkm::Evaluation::DkmInspectionSession* session,
            dkm::Evaluation::DkmLanguageExpression* expression)
        {
            evaluate_completion* completion = new (std::nothrow) evaluate_completion();
            if (!completion)
            {
                return nullptr;
            }

            completion->event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!completion->event_)
            {
                completion->Release();
                return nullptr;
            }

            completion->session_ = session;
            completion->expression_ = expression;
            return completion;
        }

        STDMETHODIMP QueryInterface(REFIID riid, void** object) override
        {
            if (!object)
            {
                return E_POINTER;
            }

            *object = nullptr;
            if (!InlineIsEqualGUID(riid, IID_IUnknown) && !InlineIsEqualGUID(riid, dkm::IID_IDkmCompletionRoutine))
            {
                return E_NOINTERFACE;
            }

            *object = static_cast<dkm::IDkmCompletionRoutine<dkm::Evaluation::DkmEvaluateExpressionAsyncResult>*>(this);
            AddRef();
            return S_OK;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return ++refs_;
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            const ULONG refs = --refs_;
            if (refs == 0)
            {
                delete this;
            }
            return refs;
        }

        void STDMETHODCALLTYPE OnComplete(const dkm::Evaluation::DkmEvaluateExpressionAsyncResult& result) override
        {
            CComCritSecLock<CComAutoCriticalSection> guard(lock_);
            if (abandoned_)
            {
                if (result.pResultObject)
                {
                    result.pResultObject->Close();
                }

                if (expression_)
                {
                    expression_->Close();
                    expression_.Release();
                }

                if (session_)
                {
                    session_->Close();
                    session_.Release();
                }
            }
            else
            {
                error_code_ = result.ErrorCode;
                result_ = result.pResultObject;
                completed_ = true;
            }

            SetEvent(event_);
        }

        HANDLE event() const
        {
            return event_;
        }

        // Returns false when the completion already arrived, so its result must be used.
        bool abandon()
        {
            CComCritSecLock<CComAutoCriticalSection> guard(lock_);
            if (completed_)
            {
                return false;
            }

            abandoned_ = true;
            return true;
        }

        // Valid after the event is signaled or abandon returned false.
        HRESULT error_code() const
        {
            return error_code_;
        }

        dkm::Evaluation::DkmEvaluationResult* result() const
        {
            return result_;
        }

        void close_result()
        {
            CComCritSecLock<CComAutoCriticalSection> guard(lock_);
            if (result_)
            {
                result_->Close();
                result_.Release();
            }
        }

    protected:
        // Protected so CComPtr's no-AddRef/Release wrapper can derive from this class.
        ~evaluate_completion()
        {
            if (result_)
            {
                result_->Close();
            }

            if (event_)
            {
                CloseHandle(event_);
            }
        }

    private:
        evaluate_completion() = default;

        std::atomic<ULONG> refs_{ 1 };
        CComAutoCriticalSection lock_;
        HANDLE event_ = nullptr;
        bool completed_ = false;
        bool abandoned_ = false;
        HRESULT error_code_ = E_PENDING;
        CComPtr<dkm::Evaluation::DkmEvaluationResult> result_;
        CComPtr<dkm::Evaluation::DkmInspectionSession> session_;
        CComPtr<dkm::Evaluation::DkmLanguageExpression> expression_;
    };

    struct outcome
    {
        // True when the evaluator produced a success or failed result.
        bool evaluated = false;
        bool valid = false;
        // Complete error response when evaluated is false.
        std::string error;
        std::wstring type;
        // Value for a success result, evaluator message for a failed result.
        std::wstring value;
    };

    inline bool honors_no_side_effects(const dkm::Evaluation::DkmCompilerId& id)
    {
        using dkm::Evaluation::DkmLanguageId;
        if (!InlineIsEqualGUID(id.VendorId, dkm::Evaluation::DkmVendorId::Microsoft))
        {
            return false;
        }

        return InlineIsEqualGUID(id.LanguageId, DkmLanguageId::Cpp) ||
            InlineIsEqualGUID(id.LanguageId, DkmLanguageId::C) ||
            InlineIsEqualGUID(id.LanguageId, DkmLanguageId::CSharp) ||
            InlineIsEqualGUID(id.LanguageId, DkmLanguageId::VB);
    }

    struct close_on_exit
    {
        CComPtr<dkm::Evaluation::DkmInspectionSession> session;
        CComPtr<dkm::Evaluation::DkmLanguageExpression> expression;

        ~close_on_exit()
        {
            if (expression)
            {
                expression->Close();
            }

            if (session)
            {
                session->Close();
            }
        }

        // Ownership moved to an abandoned completion routine.
        void dismiss()
        {
            expression.Release();
            session.Release();
        }
    };

    inline void read_result(HRESULT error_code, dkm::Evaluation::DkmEvaluationResult* result, outcome& out)
    {
        if (FAILED(error_code))
        {
            out.error = hr_error("failed", error_code);
            return;
        }

        if (!result)
        {
            out.error = "error: eval returned no result";
            return;
        }

        if (dkm::Evaluation::DkmSuccessEvaluationResult* success = dkm::Evaluation::DkmSuccessEvaluationResult::TryCast(result))
        {
            out.evaluated = true;
            out.valid = true;
            out.type = text_or_empty(success->Type());
            out.value = text_or_empty(success->Value());
            return;
        }

        if (dkm::Evaluation::DkmFailedEvaluationResult* failed = dkm::Evaluation::DkmFailedEvaluationResult::TryCast(result))
        {
            out.evaluated = true;
            out.value = text_or_empty(failed->ErrorMessage());
            return;
        }

        out.error = dkm::Evaluation::DkmIntermediateEvaluationResult::TryCast(result) ?
            "error: eval intermediate result unsupported" :
            "error: eval unsupported result kind";
    }

    // Calling thread must be initialized for Concord (bh_agent_dkm_thread_scope).
    inline void evaluate(dkm::CallStack::DkmStackFrame* frame, const std::wstring& text, outcome& out)
    {
        out = outcome();
        dkm::DkmProcess* process = frame ? frame->Process() : nullptr;
        dkm::DkmThread* thread = frame ? frame->Thread() : nullptr;
        dkm::DkmRuntimeInstance* runtime = frame ? frame->RuntimeInstance() : nullptr;
        if (!process || !thread || !runtime)
        {
            out.error = "error: eval current frame has no runtime instance";
            return;
        }

        close_on_exit owned;
        HRESULT hr = dkm::Evaluation::DkmInspectionSession::Create(process, dkm::DkmDataItem::Null(), &owned.session);
        if (FAILED(hr) || !owned.session)
        {
            out.error = hr_error("inspection session failed", hr);
            return;
        }

        // Symbol compiler id when available; zero GUIDs select the runtime's default language.
        dkm::Evaluation::DkmCompilerId compiler_id = {};
        if (dkm::DkmInstructionAddress* address = frame->InstructionAddress())
        {
            CComPtr<dkm::Symbols::DkmInstructionSymbol> symbol;
            dkm::Evaluation::DkmCompilerId symbol_compiler_id = {};
            if (SUCCEEDED(address->GetSymbol(&symbol)) && symbol &&
                SUCCEEDED(symbol->GetCompilerId(owned.session, &symbol_compiler_id)))
            {
                compiler_id = symbol_compiler_id;
            }
        }

        dkm::DkmEngineSettings* settings = process->EngineSettings();
        CComPtr<dkm::Evaluation::DkmLanguage> language;
        hr = settings ? settings->GetLanguage(compiler_id, &language) : E_POINTER;
        if (FAILED(hr) || !language)
        {
            out.error = hr_error("language unavailable", hr);
            return;
        }

        if (!honors_no_side_effects(language->Id()))
        {
            out.error = "error: eval side-effect blocking unsupported for language=" +
                response_text(dkm::DkmString::Value(language->Name()), 64) +
                "; eval --side-effects <expr> allows side effects";
            return;
        }

        const dkm::Evaluation::DkmEvaluationFlags_t flags = static_cast<dkm::Evaluation::DkmEvaluationFlags_t>(
            dkm::Evaluation::DkmEvaluationFlags::TreatAsExpression |
            dkm::Evaluation::DkmEvaluationFlags::NoSideEffects |
            dkm::Evaluation::DkmEvaluationFlags::NoFuncEval);

        CComPtr<dkm::Evaluation::DkmInspectionContext> context;
        hr = dkm::Evaluation::DkmInspectionContext::Create(owned.session, runtime, thread, inspection_timeout_ms, flags,
            dkm::Evaluation::DkmFuncEvalFlags::None, 10, language, nullptr, &context);
        if (FAILED(hr) || !context)
        {
            out.error = hr_error("inspection context failed", hr);
            return;
        }

        CComPtr<dkm::DkmString> expression_text;
        hr = dkm::DkmString::Create(text.c_str(), &expression_text);
        if (FAILED(hr) || !expression_text)
        {
            out.error = hr_error("expression text failed", hr);
            return;
        }

        hr = dkm::Evaluation::DkmLanguageExpression::Create(language, flags, expression_text, dkm::DkmDataItem::Null(), &owned.expression);
        if (FAILED(hr) || !owned.expression)
        {
            out.error = hr_error("expression create failed", hr);
            return;
        }

        CComPtr<dkm::DkmWorkList> work_list;
        hr = dkm::DkmWorkList::Create(nullptr, &work_list);
        if (FAILED(hr) || !work_list)
        {
            out.error = hr_error("work list failed", hr);
            return;
        }

        CComPtr<evaluate_completion> completion;
        completion.Attach(evaluate_completion::create(owned.session, owned.expression));
        if (!completion)
        {
            (void)work_list->Cancel();
            out.error = "error: eval out of memory";
            return;
        }

        hr = context->EvaluateExpression(work_list, owned.expression, frame, completion);
        if (SUCCEEDED(hr))
        {
            hr = work_list->BeginExecution();
        }

        if (FAILED(hr))
        {
            (void)work_list->Cancel();
            out.error = hr_error("start failed", hr);
            return;
        }

        if (WaitForSingleObject(completion->event(), completion_timeout_ms) != WAIT_OBJECT_0 && completion->abandon())
        {
            (void)work_list->Cancel(false);
            owned.dismiss();
            out.error = "error: eval timeout";
            return;
        }

        read_result(completion->error_code(), completion->result(), out);
        completion->close_result();
    }
}
