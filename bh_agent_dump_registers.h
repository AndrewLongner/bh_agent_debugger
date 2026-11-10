#pragma once
#include "bh_agent_dump_support.h"

// The reader is DkmThread in production; tests supply captured-context fixtures.
// This deliberately has no Windows process/thread handle fallback.
class bh_dump_registers
{
public:
    template<class Reader>
    static HRESULT read(Reader* thread, unsigned width, UINT64& ip, UINT64& sp, std::string& text)
    {
        ip = sp = 0;
        text.clear();
        if (!thread) return E_POINTER;
        if (width == 8)
        {
            CONTEXT context = {};
            const HRESULT hr = thread->GetContext(CONTEXT_CONTROL | CONTEXT_INTEGER, &context, sizeof(context));
            if (FAILED(hr)) return hr;
            ip = context.Rip;
            sp = context.Rsp;
            text = "rip=" + bh_dump::hex(ip) + " rsp=" + bh_dump::hex(sp) + " rbp=" + bh_dump::hex(context.Rbp) +
                " rax=" + bh_dump::hex(context.Rax) + " rbx=" + bh_dump::hex(context.Rbx) + " rcx=" + bh_dump::hex(context.Rcx) +
                " rdx=" + bh_dump::hex(context.Rdx) + " rsi=" + bh_dump::hex(context.Rsi) + " rdi=" + bh_dump::hex(context.Rdi) +
                " r8=" + bh_dump::hex(context.R8) + " r9=" + bh_dump::hex(context.R9) + " r10=" + bh_dump::hex(context.R10) +
                " r11=" + bh_dump::hex(context.R11) + " r12=" + bh_dump::hex(context.R12) + " r13=" + bh_dump::hex(context.R13) +
                " r14=" + bh_dump::hex(context.R14) + " r15=" + bh_dump::hex(context.R15) + " flags=" + bh_dump::hex(context.EFlags);
            return S_OK;
        }
        if (width == 4)
        {
            WOW64_CONTEXT context = {};
            const HRESULT hr = thread->GetContext(WOW64_CONTEXT_CONTROL | WOW64_CONTEXT_INTEGER, &context, sizeof(context));
            if (FAILED(hr)) return hr;
            ip = context.Eip;
            sp = context.Esp;
            text = "eip=" + bh_dump::hex(ip) + " esp=" + bh_dump::hex(sp) + " ebp=" + bh_dump::hex(context.Ebp) +
                " eax=" + bh_dump::hex(context.Eax) + " ebx=" + bh_dump::hex(context.Ebx) + " ecx=" + bh_dump::hex(context.Ecx) +
                " edx=" + bh_dump::hex(context.Edx) + " esi=" + bh_dump::hex(context.Esi) + " edi=" + bh_dump::hex(context.Edi) +
                " flags=" + bh_dump::hex(context.EFlags);
            return S_OK;
        }
        return E_NOTIMPL;
    }
};
