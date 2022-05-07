#include "util.h"

namespace sylar{

pid_t GetThreadId(){
  // syscall - 间接系统调用syscall() 执行一个系统调用，根据指定的参数number和所有系统调用的汇编语言接口来确定调用哪个系统调用。
  //系统调用所使用的符号常量可以在头文件<sys/syscll.h>里面找到。
  return syscall(SYS_gettid);
}
uint32_t GetFiberId() {
  return 0;
}

}