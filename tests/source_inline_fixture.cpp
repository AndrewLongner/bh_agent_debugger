__forceinline int source_inline_inner(volatile int* value)
{
    int a = *value;
    *value = a * 7 + 3;
    return *value ^ 123;
}
__forceinline int source_inline_outer(volatile int* value)
{
    int a = source_inline_inner(value);
    *value = a + 11;
    return source_inline_inner(value) + a;
}
extern "C" __declspec(dllexport) __declspec(noinline) int source_drilldown_root(volatile int* value)
{
    int total = 0;
    for (int i = 0; i < 23; ++i) total += source_inline_outer(value);
    return total;
}
