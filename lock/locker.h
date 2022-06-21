#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 信号量类
class sem
{
public:
    sem()
    {
        // sem_init() 成功时返回 0,错误时返回 -1,并把 errno 设置为合适的值。
        // sem     ：指向信号量对象
        // pshared : 指明信号量的类型。不为0时此信号量在进程间共享,否则只能为当前进程的所有线程共享。
        // value   : 指定信号量的初始值
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    // 以原子操作方式把信号量-1,信号量为0时,sem_wait阻塞
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    // 以原子操作方式把信号量+1,信号量大于0时,sem_post阻塞
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

// 互斥锁类
class locker
{
public:
    locker()
    {
        // 初始化互斥锁
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        // 销毁互斥锁
        pthread_mutex_destroy(&m_mutex);
    }
    // 以原子操作方式给互斥锁加锁
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    // 以原子操作方式给互斥锁解锁
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    // 返回互斥锁对象
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 条件变量类:当某个共享数据达到某个值时,唤醒等待这个共享数据的线程。
class cond
{
public:
    cond()
    {
        // 初始化条件变量
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        // 销毁条件变量
        pthread_cond_destroy(&m_cond);
    }
    
    bool wait(pthread_mutex_t *m_mutex)
    {
        //条件变量的使用机制需要配合锁来使用
        //内部会有一次加锁和解锁
        //封装起来会使得更加简洁
        
        int ret = 0;

        //这里不知道为啥要注释掉。后面需要再看看
        //pthread_mutex_lock(&m_mutex);

        // 调用时需要传入 mutex参数(加锁的互斥锁) ,函数执行时,先把调用线程放入条件变量的请求队列,然后将互斥锁mutex解锁,当函数成功返回为0时,互斥锁会再次被锁上. 也就是说函数内部会有一次解锁和加锁操作.
        ret = pthread_cond_wait(&m_cond, m_mutex);

        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;

        //这里不知道为啥要注释掉。后面需要再看看
        //pthread_mutex_lock(&m_mutex);

        // 计时等待方式如果在给定时刻前条件没有满足,则返回ETIMEDOUT,结束等待
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);

        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    // 激活一个等待该条件的线程，存在多个等待线程时按入队顺序激活其中一个
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    // 以广播的方式唤醒所有等待目标条件变量的线程。
    bool broadcast()
    {
        // 和上一个函数的区别一个是“一个”，一个是“所有”。
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
