#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
// 带指针的类自定义析构函数
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 如果新的定时器超时时间小于当前头部结点
    // 直接将当前定时器结点作为头部结点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 否则调用private函数，调整内部结点
    add_timer(timer, head);
}

// 调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;

    // 被调整的定时器在链表尾部 或 定时器超时值仍然小于下一个定时器超时值，不用调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    // 被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    // 被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

// 删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    // 链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }

    // 被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }

    // 被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }

    // 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 定时任务处理函数
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }
    // 获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        // 因为是升序链表。如果当前时间小于定时器的超时时间，后面的定时器肯定也没有到期
        if (cur < tmp->expire)
        {
            break;
        }
        // 当前定时器到期，则执行定时事件
        tmp->cb_func(tmp->user_data);

        // 将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

// 私有成员，被公有成员add_timer和adjust_time调用：调整链表内部节点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        // 遍历当前结点之后的链表，找到应该插入的位置，插入链表。
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 目标定时器需要放到尾结点处的情况
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数只通过管道发信号值，不处理对应逻辑。缩短异步执行时间

//信号处理函数
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    // 中断后再次进入该函数，环境变量和之前的相同，避免数据丢失
    int save_errno = errno;
    int msg = sig;
    // 把信号值从管道写端写入，传输类型为char型
    send(u_pipefd[1], (char *)&msg, 1, 0);
    // 恢复原先的errno
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    // 创建sigaction结构体
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    // 信号处理函数只发函数值，不做逻辑处理
    sa.sa_handler = handler;

    if (restart)
        // 指定信号处理的行为，使被信号打断的系统调用自动重新发起
        sa.sa_flags |= SA_RESTART;
        
    // 把所有信号添加到信号集中
    sigfillset(&sa.sa_mask);

    // 执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;

// 定时事件：删除非活动socket上的注册事件，并关闭
void cb_func(client_data *user_data)
{
    // 从内核事件表删除事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    // 关闭文件描述符，释放连接资源
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
