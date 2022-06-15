#ifndef __SYLAR_MACRO_H__
#define __SYLAR_MACRO_H__

#include <string.h>
#include <assert.h>
#include "log.h"
#include "util.h"
//这个指令是gcc引入的，作用是允许程序员将最有可能执行的分支告诉编译器。这个指令的写法为：
//__builtin_expect(EXP, N)。意思是：EXP==N的概率很大
//if(likely(value))  //等价于 if(value)
//if(unlikely(value))  //也等价于 if(value)
//defined用来检测常量有没有被定义 gcc编译
#if defined __GNUC__ || defined __llvm__
/// LIKCLY 宏的封装, 告诉编译器优化,条件大概率成立
#define SYLAR_LIKELY(x)    __builtin_expect(!!(x), 1)
#define SYLAR_UNLIKELY(x)    __builtin_expect(!!(x), 0)
#else
# define SYLAR_LIKELY(x)      (x)
# define SYLAR_UNLIKELY(x)      (x)
#endif

///断言宏封装如果(SYLAR_UNLIKELY(!(x)))为真，打印错误日志
#define SYLAR_ASSERT(x) \
    if(SYLAR_UNLIKELY(!(x))){\
        SYLAR_LOG_ERROR(SYLAR_LOG_ROOT()) << "ASSERTION: " #x \
            << "\nbacktrace:\n" \
            << sylar::BacktraceToString(100, 2, "    "); \
        assert(x); \
    }
#define SYLAR_ASSERT2(x, w) \
    if(SYLAR_UNLIKELY(!(x))){\
        SYLAR_LOG_ERROR(SYLAR_LOG_ROOT()) << "ASSERTION: " #x \
            << "\n"                                      \
            << w  \
            << "\nbacktrace:\n" \
            << sylar::BacktraceToString(100, 2, "    "); \
        assert(x); \
    }
    
   
#endif