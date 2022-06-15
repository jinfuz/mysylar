#include "timer.h"
#include "util.h"
#include "macro.h"

namespace sylar {
    
bool Timer::Comparator::operator()(const Timer::ptr& lhs
                        , const Timer::ptr& rhs) const {
    if(!lhs && !rhs) return false;
    if(!lhs) return true;
    if(!rhs) return false;
    if(lhs->m_next < rhs->m_next){
        return true;
    }
    if(lhs->m_next > rhs->m_next){
        return false;
    }
    return lhs.get() < rhs.get();
}

Timer::Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager) 
    :m_recurring(recurring)
    ,m_ms(ms)
    ,m_cb(cb)
    ,m_manager(manager) {
    m_next = sylar::GetElapsedMS() + m_ms;
}

Timer::Timer(uint64_t next)
    :m_next(next) {
}

bool Timer::cancel() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    //重置定时器回调函数，从定时器管理器的定时器集合删除定时器
    if(m_cb) {
        m_cb = nullptr;
        auto it = m_manager->m_timers.find(shared_from_this());
        m_manager->m_timers.erase(it);
        return true;
    }
    return false;
}

//把当前定时器时间延到下一个周期，由于是最小堆要保证有序不能直接改m_next，需要先删除再加入定时器，
bool Timer::refresh() {
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb){
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    m_manager->m_timers.erase(it);
    m_next = sylar::GetElapsedMS() + m_ms;
    m_manager->m_timers.insert(shared_from_this());
    return true;
}

bool Timer::reset(uint64_t ms, bool from_now) {
    if(ms == m_ms && !from_now) {
        return true;
    }
    TimerManager::RWMutexType::WriteLock lock(m_manager->m_mutex);
    if(!m_cb){
        return false;
    }
    auto it = m_manager->m_timers.find(shared_from_this());
    if(it == m_manager->m_timers.end()) {
        return false;
    }
    //先删除定时器，再重新加入
    m_manager->m_timers.erase(it);
    uint64_t start = 0;
    if(from_now) {
        start = sylar::GetElapsedMS();
    } else {
        start = m_next - m_ms;
    }
    m_ms = ms;
    m_next = start + m_ms;
    m_manager->addTimer(shared_from_this(), lock);
    return true;
}

TimerManager::TimerManager() {
    m_previouseTime = sylar::GetElapsedMS();
}

TimerManager::~TimerManager() {
}

bool TimerManager::hasTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    return !m_timers.empty();
}

Timer::ptr TimerManager::addTimer(uint64_t ms, std::function<void()> cb
                                  ,bool recurring) {
    Timer::ptr timer(new Timer(ms, cb, recurring, this));
    RWMutexType::WriteLock lock(m_mutex);
    addTimer(timer, lock);
    return timer;
}  

void TimerManager::addTimer(Timer::ptr val, RWMutexType::WriteLock& lock) {
    auto it = m_timers.insert(val).first;
    //如果插入在定时器集合的开头
    bool at_front = (it == m_timers.begin()) && !m_tickled;
    if(at_front) {
        m_tickled = true;
    }
    lock.unlock();

    if(at_front) {
        onTimerInsertedAtFront();//由子类实现
    }
}

//全局函数，入参用weak_ptr,用shared_ptr无法知道是否是悬空的，执行回调函数
static void OnTimer(std::weak_ptr<void> weak_cond, std::function<void()> cb) {
    std::shared_ptr<void> tmp = weak_cond.lock();
    if(tmp) {
        cb();
    }
}

Timer::ptr TimerManager::addConditionTimer(uint64_t ms, std::function<void()> cb
                                        , std::weak_ptr<void> weak_cond
                                        , bool recurring) {
    return addTimer(ms, std::bind(&OnTimer, weak_cond, cb), recurring);
}

//最近一个定时器执行的时间间隔(毫秒)
uint64_t TimerManager::getNextTimer() {
    RWMutexType::ReadLock lock(m_mutex);
    m_tickled = false;
    if(m_timers.empty()) {
        return ~0ull;//unsigned long long 类型的0取反, uint64_t最大值
    }

    const Timer::ptr& next = *m_timers.begin();
    uint64_t now_ms = sylar::GetElapsedMS();
    if(now_ms >= next->m_next){
        return 0;
    } else {
        return next->m_next - now_ms;
    }
}

bool TimerManager::detectClockRollover(uint64_t now_ms) {
    bool rollover = false;
    //系统时间回滚了1个小时以上
    if(now_ms < m_previouseTime &&
            now_ms < (m_previouseTime - 60 * 60 * 1000)) {
        rollover = true;
    }
    m_previouseTime = now_ms;
    return rollover;
}

void TimerManager::listExpiredCb(std::vector<std::function<void()> >& cbs) {
    uint64_t now_ms = sylar::GetElapsedMS();
    std::vector<Timer::ptr> expired;
    {
        RWMutexType::ReadLock lock(m_mutex);
        if(m_timers.empty()){
            return;
        }
    }

    RWMutexType::WriteLock lock(m_mutex);
    if(m_timers.empty()) {
        return;
    }
    bool rollover = false;
    if(SYLAR_UNLIKELY(detectClockRollover(now_ms))) {
        // 使用clock_gettime(CLOCK_MONOTONIC_RAW)，应该不可能出现时间回退的问题
        //CLOCK_MONOTONIC_RAW:从系统启动这一刻起开始计时,不受系统时间被用户改变的影响
        rollover = true;
    }
    //没有超时定时器
    if(!rollover && ((*m_timers.begin())->m_next > now_ms)) {
        return;
    }

    Timer::ptr now_timer(new Timer(now_ms));
    //如果系统时间往回调了1个小时以上，那就触发全部定时器,否则找出第一个大于等于now_timer的定时器
    auto it = rollover ? m_timers.end() : m_timers.lower_bound(now_timer);
    //跳过等于now_timer的定时器
    while(it != m_timers.end() && (*it)->m_next == now_ms) {
        ++it;
    }

    expired.insert(expired.begin(), m_timers.begin(), it);
    m_timers.erase(m_timers.begin(), it);
    //预先设定容量到指定值，背后执行的可能是内存分配
    cbs.reserve(expired.size());

    for(auto& timer : expired) {
        cbs.push_back(timer->m_cb);
        if(timer->m_recurring) {
            //循环定时器修改时间加入定时器集合
            timer->m_next = now_ms + timer->m_ms;
            m_timers.insert(timer);
        } else {
            timer->m_cb = nullptr;
        }
    }
}

}



