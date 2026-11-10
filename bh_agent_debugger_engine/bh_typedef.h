#pragma once

typedef unsigned char U8;
typedef unsigned short U16;
typedef short I16;
typedef unsigned int U32;
typedef int I32;
typedef unsigned long long U64;
typedef long long I64;
typedef float F32;
typedef double F64;

#if _DEBUG
// TODO: Complete assertion macro
#define bh_assert(Expression) \
	if (!(Expression))        \
	{                         \
		int *i(0);            \
		*i = 0;               \
	}
#else
#define bh_assert(Expression)
#endif
