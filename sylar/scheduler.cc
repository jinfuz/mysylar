#include "scheduler.h"
#include "log.h"
#include "macro.h"
#include "hook.h"

namespace sylar{
static sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");

//当前线程的调度器，同一个调度器下的所有线程共享同一个实例
static thread_local Scheduler* t_scheduler = nullptr;
//当前线程的调度协程，每个线程都独有一份
static thread_local Fiber* t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string& name)
    :m_name(name),m_useCaller(use_caller) {
    SYLAR_ASSERT(threads > 0);
    if(use_caller) {
        sylar::Fiber::GetThis();
        --threads;//使用当前线程作为调度线程,可用线程减一

        SYLAR_ASSERT(GetThis() == nullptr);//当前线程的调度器初始化时应为空
        t_scheduler = this;

         
        //caller线程的主协程不会被线程的调度协程run进行调度，而且，线程的调度协程停止
        //时，应该返回caller线程的主协程,在user caller情况下，把caller线程的主协程
        //暂时保存起来，等调度协程结束时，再resume caller协程
         
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        sylar::Thread::SetName(m_name);

        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = sylar::GetThreadId();
        m_threadIds.push_back(m_rootThread);

    }else{
        m_rootThread = -1;
    }
    m_threadCount = threads;
}

Scheduler* Scheduler::GetThis() {
    return t_scheduler;
}

void Scheduler::setThis() {
    t_scheduler = this;
}

Fiber* Scheduler::GetMainFiber() {
    return t_scheduler_fiber;
}

Scheduler::~Scheduler() {
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::~Scheduler()";
    SYLAR_ASSERT(m_stopping);
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

void Scheduler::start(){
    SYLAR_LOG_DEBUG(g_logger) << "start";
    MutexType::Lock lock(m_mutex);
    if(m_stopping){
        SYLAR_LOG_ERROR(g_logger) << "Scheduler is stopped";
        return;
    }
    SYLAR_ASSERT(m_threads.empty());
    m_threads.resize(m_threadCount);
    for (size_t i = 0; i < m_threadCount; i++) {
        //每个线程都要执行协程调度器的run,处理一个任务,new的时候就开始执行run了
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this),
                                      m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
}
//读取的时候也加锁，防止有其他线程修改m_stopping，m_tasks，m_activeThreadCount
bool Scheduler::stopping() {
    MutexType::Lock lock(m_mutex);
    return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}

void Scheduler::tickle() { 
    SYLAR_LOG_DEBUG(g_logger) << "ticlke"; 
}

void Scheduler::idle() {
    SYLAR_LOG_DEBUG(g_logger) << "idle";
    while (!stopping()) {
        sylar::Fiber::GetThis()->yield();//从正在运行的协程切换到当前线程的主协程,如果协程参与调度器调度，那么切换到调度器的主协程
    }
}

void Scheduler::stop(){
    SYLAR_LOG_DEBUG(g_logger) << "stop";
    if (stopping()) {
        return;
    }
    m_stopping = true;

    // 如果use caller，那只能由caller线程发起stop，其他线程不能自己调用stop
    //GetThis()返回的是当前线程的scheduler
    if (m_useCaller) {
        SYLAR_ASSERT(GetThis() == this);
    } else {
        SYLAR_ASSERT(GetThis() != this);
    }

    //通知所有线程
    for (size_t i = 0; i < m_threadCount; i++) {
        tickle();
    }
    //单独通知caller线程的主协程
    if (m_rootFiber) {
        tickle();
    }

    //在use caller情况下，调度器协程结束时，应该返回caller协程
    if (m_rootFiber) {
        m_rootFiber->resume();
        SYLAR_LOG_DEBUG(g_logger) << "m_rootFiber end";
    }

    //把Thread::ptr交换出来，确定shared_ptr的析构时间，否则要等Scheduler析构调用智能指针析构函数
    std::vector<Thread::ptr> thrs;
    {
        MutexType::Lock lock(m_mutex);
        thrs.swap(m_threads);
    }
    for (auto &i : thrs) {
        i->join();
    }

}

void Scheduler::run() {
    SYLAR_LOG_DEBUG(g_logger) << "run";
    set_hook_enable(true);
    setThis();
    //当前不是调度线程，则设置当前线程的正在运行的协程为当前线程的调度协程
    if(sylar::GetThreadId() != m_rootThread){
        t_scheduler_fiber = sylar::Fiber::GetThis().get();
    }
    //空闲协程会yeild切换到当前线程的主协程,一直resume一个空闲协程，然后yeild回来
    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this)));
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while(true){
        task.reset();
        bool tickle_me = false;// 是否tickle其他线程进行任务调度
        {
            MutexType::Lock lock(m_mutex);
            auto it = m_tasks.begin();
            //遍历所有调度任务
            while(it != m_tasks.end()){
                if(it->thread != -1 && it->thread != sylar::GetThreadId()){
                    // 指定了调度线程，但不是在当前线程上调度，标记一下需要通知其他线程进行调度，然后跳过这个任务，继续下一个
                    ++it;
                    tickle_me = true;
                    continue;
                }
                // 找到一个未指定线程，或是指定了当前线程的任务
                SYLAR_ASSERT(it->fiber || it->cb);

                // [BUG FIX]: hook IO相关的系统调用时，在检测到IO未就绪的情况下，会先添加对应的读写事件，再yield当前协程，等IO就绪后再resume当前协程
                // 多线程高并发情境下，有可能发生刚添加事件就被触发的情况，如果此时当前协程还未来得及yield，则这里就有可能出现协程状态仍为RUNNING的情况
                // 这里简单地跳过这种情况，以损失一点性能为代价，否则整个协程框架都要大改
                //比如对于主动yield的协程，要先把当前协程放入任务队列，再yield,这两个动作之间任务又被执行了
                if(it->fiber && it->fiber->getState() == Fiber::RUNNING) {
                    ++it;
                    continue;
                }
                // 当前调度线程找到一个任务，准备开始调度，将其从任务队列中剔除，活动线程数加1
                task = *it;
                m_tasks.erase(it++);
                ++m_activeThreadCount;
                break;
            }
            // 当前线程拿完一个任务后，发现任务队列还有剩余，那么tickle一下其他线程
            tickle_me |= (it != m_tasks.end());
        }

        if (tickle_me) {
            tickle();
        }
        if (task.fiber) {
            // resume协程，resume返回时，协程要么执行完了，要么半路yield了，总之这个任务就算完成了，活跃线程数减一
            task.fiber->resume();
            --m_activeThreadCount;
            task.reset();
        } else if (task.cb) {
            if (cb_fiber) {
                cb_fiber->reset(task.cb);
            } else {
                cb_fiber.reset(new Fiber(task.cb));
            }
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset();
        } else {
            // 进到这个分支情况一定是任务队列空了，调度idle协程即可
            if (idle_fiber->getState() == Fiber::TERM) {
                // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                SYLAR_LOG_DEBUG(g_logger) << "idle fiber term";
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
        
    }
    SYLAR_LOG_DEBUG(g_logger) << "Scheduler::run() exit";
}

}