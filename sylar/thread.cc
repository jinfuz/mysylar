#include "thread.h"
#include "log.h"
#include "util.h"

namespace sylar{

//thread_local每个线程都会有这个变量，线程名称和this
static thread_local Thread* t_thread = nullptr;
static thread_local std::string t_thread_name = "UNKNOW";

static sylar::Logger::ptr  g_logger = SYLAR_LOG_NAME("system");

Thread* Thread::GetThis(){
    return t_thread;
}

const std::string& Thread::GetName(){
    return t_thread_name;
}

void Thread::SetName(const std::string& name){
    if(name.empty()){
        return;
    }
    if(t_thread){
        t_thread->m_name = name;
    }
    t_thread_name = name;

}

Thread::Thread(std::function<void()> cb, const std::string& name)
    :m_cb(cb),m_name(name){
    if(name.empty()) {
        m_name = "UNKNOW";
    }
    int rt = pthread_create(&m_thread, nullptr, &Thread::run, this);//若线程创建成功，则返回0。若线程创建失败，则返回出错编号
    if(rt) {
        SYLAR_LOG_ERROR(g_logger) << "pthread_create thread fail, rt =" << rt 
        << " name=" <<name;
        throw std::logic_error("pthread_create error");
    }
    //确保线程创建成功之后就跑起来了
    m_semaphore.wait();
}
Thread::~Thread(){
    if(m_thread){
        //指定该状态，线程主动与主控线程断开关系。线程结束后（不会产生僵尸线程），
        //其退出状态不由其他线程获取，而直接自己自动释放（自己清理掉PCB的残留资源）
        //一般情况下，线程终止后，其终止状态一直保留到其它线程调用pthread_join获取它的
        //状态为止（或者进程终止被回收了）。但是线程也可以被置为detach状态，这样的线程一
        //旦终止就立刻回收它占用的所有资源，而不保留终止状态。不能对一个已经处于detach
        //状态的线程调用pthread_join，这样的调用将返回EINVAL错误（22号错误）。
        //也就是说，如果已经对一个线程调用了pthread_detach就不能再调用pthread_join了。
        pthread_detach(m_thread);
    }
}
void Thread::join(){
    if(m_thread){
        int rt = pthread_join(m_thread, nullptr);
        if(rt) {
            SYLAR_LOG_ERROR(g_logger) << "pthread_join thread fail, rt=" << rt
                << " name=" << m_name;
            throw std::logic_error("pthread_join error");
        }
        m_thread = 0;
    }
}

void* Thread::run(void* arg) {
    Thread* thread = (Thread*) arg;
    t_thread = thread;
    t_thread_name = thread->m_name;
    thread->m_id = sylar::GetThreadId();
     pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

    std::function<void()> cb;
    cb.swap(thread->m_cb);

    thread->m_semaphore.notify();

    cb();
    return 0;



}
}