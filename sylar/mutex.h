#ifndef __SYLAR_MUTEX_H__
#define __SYLAR_MUTEX_H__

#include <thread>
#include <functional>
#include <memory>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <atomic>
#include <list>

#include "noncopyable.h"

namespace sylar{

/**
 * @brief 信号量
 */
class Semaphore : Noncopyable {
public:
    /**
     * @brief 构造函数
     * @param[in] count 信号量值的大小
     */
    Semaphore(uint32_t count = 0);

    /**
     * @brief 析构函数
     */
    ~Semaphore();

    /**
     * @brief 获取信号量
     */
    void wait();

    /**
     * @brief 释放信号量
     */
    void notify();
private:
    sem_t m_semaphore;
};

//局部锁
template<class T>
struct ScopedLockImpl {
public:
    ScopedLockImpl(T& mutex):m_mutex(mutex){
        m_mutex.lock();
        m_locked = true;
    }
    ~ScopedLockImpl(){
        unlock();
    }
    void lock(){
        if(!m_locked) {
            m_mutex.lock();
            m_locked = true;
        }
    }
    void unlock(){
        if(m_locked){
            m_mutex.unlock();
            m_locked = false;
        }
    }
private:
    T& m_mutex;
    bool m_locked;
};

//局部读锁模板
template<class T>
struct ReadScopedLockImpl{
public:
    ReadScopedLockImpl(T& mutex):m_mutex(mutex){
        m_mutex.rdlock();
        m_locked = true;
    }
    ~ReadScopedLockImpl(){
        unlock();
    }
    void lock(){
        if(!m_locked) {
            m_mutex.rdlock();
            m_locked = true;
        }
    }
    void unlock(){
        if(m_locked){
            m_mutex.unlock();
            m_locked = false;
        }
    }
private:
    T& m_mutex;
    bool m_locked;
};

//局部写锁模板
template<class T>
struct WriteScopedLockImpl{
public:
    WriteScopedLockImpl(T& mutex):m_mutex(mutex){
        m_mutex.wrlock();
        m_locked = true;
    }
    ~WriteScopedLockImpl(){
        unlock();
    }
    void lock(){
        if(!m_locked) {
            m_mutex.wrlock();
            m_locked = true;
        }
    }
    void unlock(){
        if(m_locked){
            m_mutex.unlock();
            m_locked = false;
        }
    }
private:
    T& m_mutex;
    bool m_locked;
};

//互斥量
class Mutex : Noncopyable{
public:
    typedef ScopedLockImpl<Mutex> Lock;
    Mutex(){
        pthread_mutex_init(&m_mutex, nullptr);
    }
    ~Mutex(){
        pthread_mutex_destroy(&m_mutex);
    }

    void lock(){
        pthread_mutex_lock(&m_mutex);
    }
    void unlock(){
        pthread_mutex_unlock(&m_mutex);
    }
private:
    pthread_mutex_t m_mutex;

};

// 空锁(用于调试)
class NullMutex : Noncopyable{
public:
    /// 局部锁
    typedef ScopedLockImpl<NullMutex> Lock;
    NullMutex() {}
    ~NullMutex() {}
    void lock() {}
    void unlock() {}
};

class RWMutex : Noncopyable{
public:
    typedef ReadScopedLockImpl<RWMutex> ReadLock;
    typedef WriteScopedLockImpl<RWMutex> WriteLock;

    RWMutex(){
        pthread_rwlock_init(&m_lock, nullptr);
    }
    ~RWMutex(){
        pthread_rwlock_destroy(&m_lock);
    }
    void rdlock(){
        pthread_rwlock_rdlock(&m_lock);
    }
    void wrlock(){
        pthread_rwlock_wrlock(&m_lock);
    }
    void unlock(){
        pthread_rwlock_unlock(&m_lock);
    }
private:
    pthread_rwlock_t m_lock;
};

//空读写锁(用于调试)
class NullRWMutex : Noncopyable {
public:
    /// 局部读锁
    typedef ReadScopedLockImpl<NullMutex> ReadLock;
    /// 局部写锁
    typedef WriteScopedLockImpl<NullMutex> WriteLock;

    NullRWMutex() {}
    ~NullRWMutex() {}
    void rdlock() {}
    void wrlock() {}
    void unlock() {}
};

//自旋锁
class Spinlock : Noncopyable{
public:
    typedef ScopedLockImpl<Spinlock> Lock;
    Spinlock(){
        pthread_spin_init(&m_mutex, 0);
    }
    ~Spinlock(){
        pthread_spin_destroy(&m_mutex);
    }
    void lock(){
        pthread_spin_lock(&m_mutex);
    }
    void unlock(){
        pthread_spin_unlock(&m_mutex);
    }
private:
    pthread_spinlock_t m_mutex;
};

//原子锁
class CASLock : Noncopyable{
public:
    typedef ScopedLockImpl<CASLock> Lock;
    CASLock(){
        m_mutex.clear();//设置为false
    }
    ~CASLock(){
    }
    void lock(){
        //来自不同线程的两个原子操作顺序不一定？那怎么能限制一下它们的顺序？这就需要两
        //个线程进行一下同步（synchronize-with）。同步什么呢？同步对一个变量的读写操作。
        //线程 A 原子性地把值写入 x (release), 然后线程 B 原子性地读取 x 的值（acquire）
        //. 这样线程 B 保证读取到 x 的最新值。注意 release -- acquire 有个牛逼的副作用：
        //线程 A 中所有发生在 release x 之前的写操作，对在线程 B acquire x 之后的任何
        //读操作都可见！本来 A, B 间读写操作顺序不定。这么一同步，在 x 这个点前后， A, B
        // 线程之间有了个顺序关系，称作 inter-thread happens-before.
        while(std::atomic_flag_test_and_set_explicit(&m_mutex,std::memory_order_acquire));
    }
    void unlock(){
        std::atomic_flag_clear_explicit(&m_mutex,std::memory_order_release);
    }

private:
    volatile std::atomic_flag m_mutex;
};

}

#endif