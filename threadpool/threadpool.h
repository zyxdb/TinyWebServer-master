#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

// 线程池类定义
template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量,max_requests是请求队列中最多允许的、等待处理的请求的数量,connection_pool是数据库连接池指针*/
    // actor_model是模式切换状态指示参数,用来标识线程是否要被结束。
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    // 向请求队列中插入任务请求（可是为啥要2个函数？一个带状态一个不带状态？）
    bool append(T *request, int state);
    bool append_p(T *request);

private:
    // 线程处理函数和运行函数需要设置为私有函数
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;           //线程池中的线程数
    int m_max_requests;            //请求队列中允许的最大请求数
    pthread_t *m_threads;          //描述线程池的数组,其大小为m_thread_number
    std::list<T *> m_workqueue;    //请求队列
    locker m_queuelocker;          //保护请求队列的互斥锁
    sem m_queuestat;               //是否有任务需要处理
    connection_pool *m_connPool;   //数据库连接池
    int m_actor_model;             //模式切换
};

//线程池构造函数
template <typename T>
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        // pthread_create的4个参数分别为:
        // 1.返回新生成的线程id
        // 2.指向线程属性的指针,通常为NULL（可以在这里直接设置线程分离）
        // 3.处理线程函数的地址——如果该函数是类成员函数,需要设置成静态成员函数(因为参数会自带this指针,不能通过编译)
        // 4.参数3传入的函数中的参数,最多传1个
        // pthread_create的返回值表示成功，返回0。表示出错，返回表示-1。
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }

        // 将线程进行分离后，不用单独对工作线程进行回收
        // 线程分离状态：指定该状态，线程主动与主控线程断开关系。
        // 线程结束后（不会产生僵尸线程），其退出状态不由其他线程获取，而直接自己自动释放（自己清理掉PCB的残留资源）。网络、多线程服务器常用。
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

// 析构函数
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

// 向请求队列中添加任务
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    // 通过互斥锁保证线程安全
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    // 向链表队列中添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 添加完成后通过信号量提醒有任务要处理
    m_queuestat.post();
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 线程处理函数
// 将类的对象作为参数传递给静态函数worker，在该函数里调用这个对象的动态方法run()
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    // 应该的是返回void*类型才对啊？
    return pool;
}

// run执行任务
// 主要的实现函数，工作线程从请求队列中取出某个任务进行处理，需要注意线程同步的问题。
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        // 信号量等待
        // 如果对一个值为0的信号量调用sem_wait()，这个函数就会地等待直到有其它线程增加了这个值使它不再是0为止。
        m_queuestat.wait();
        // 被唤醒后先加上互斥锁（重复加锁怎么办？）
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }

        // 从请求队列中取出第一个任务
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        // 防止第一个任务是空指针
        if (!request)
            continue;
        if (1 == m_actor_model)
        {
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    // 从连接池中取出一个数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    // process（模板类中的方法，这里是http类）进行处理
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
