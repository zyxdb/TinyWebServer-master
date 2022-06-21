#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;

// 用户名和密码
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    // fcntl(int fd,int cmd)是计算机中的一种函数，通过fcntl可以改变已打开的文件性质。参数fd是被参数cmd操作的描述符。
    // F_GETFL 取得文件描述符状态标志，此标志为open（）的参数flags。
    int old_option = fcntl(fd, F_GETFL);
    /* 设置为非阻塞*/
    int new_option = old_option | O_NONBLOCK;
    // F_SETFL 设置文件描述符状态旗标，参数arg为新旗标，但只允许O_APPEND、O_NONBLOCK和O_ASYNC位的改变，其他位的改变将不受影响。
    fcntl(fd, F_SETFL, new_option);
    // fcntl的返回值与命令有关。如果出错，所有命令都返回-1，如果成功则返回相应标志
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    // ET模式
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    // LT模式
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    // 主状态机初始状态设为【解析请求行】
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            // 下一个字符是buffer结尾，接收不完整，需要继续接收
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 为啥需要2个\0？ ———— 把原本这里的/r/n，在读入的时候要转换成\0\0，方便text可以直接读取完整的行进行解析
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 如果上次读取到/r就到了buffer的末尾，还没接收完整，再次接收的时候就会出现这种情况。
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    // 缓冲区满的话，返回失败。
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        // 从套接字接收数据，存储在m_read_buf缓冲区.函数说明如下：
            /*
            不论是客户还是服务器应用程序，都用recv函数从TCP连接的另一端接收数据。
            该函数的第一个参数指定接收端套接字描述符;
            第二个参数指明一个缓冲区，该缓冲区用来存放recv函数接收到的数据;
            第三个参数指明buf的长度;
            第四个参数一般置0。
            */
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        // 更新已读取字节数
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            // 从套接字接收数据，存储在m_read_buf缓冲区
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 非阻塞ET模式下，需要一次性把数据全部读完
                // EAGAIN、EWOULDBLOCK为无数据可读
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            // 如果recv函数在等待协议接收数据时网络中断了，那么它返回0。（如对端的socket已正常关闭）
            else if (bytes_read == 0)
            {
                return false;
            }
            // 更新已读取字节数
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。

    // 找到请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");
    // 如果找不到代表格式有错误，直接返回BAD
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // 把该位置改为\0，便于把前面数据取出；然后跳过该位置，指向下一个位置
    *m_url++ = '\0';

    // 取出数据，通过和GET、POST比较，确定请求方式。
    // 因为m_url已经把之前的一个位置取\0了，所以method会形如：{'G','E','T','\0'}
    char *method = text;
    // strcasecmp:忽略大小写比较字符串
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;

    // 此时m_url虽然已经跳过第一个空格或者\0，但是不知道后面还有没有。要让m_url指向请求资源的第一个字符
    // 使用库函数strspn，检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_url += strspn(m_url, " \t");
    // 使用和判断 POST或GET 的相同逻辑，判断版本号。
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // 对请求资源的前7个和前8个字符进行判断
    // 因为有些报文的请求资源中会带有http://或https://,需要对这种特殊情况单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        // strchr：在一个串中查找给定字符的第一个匹配之处
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    // 一般情况的请求url形如/562f25980001b1b106000338.jpg。非这3种格式的返回BAD
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示欢迎界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 请求行处理完毕，将主状态机 状态切换为 请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 判断是空行还是请求头
    // 空行的情况
    if (text[0] == '\0')
    {
        // 判断content（消息体）长度是否为0，不为0则进入消息体处理状态，为0代表已经获得完整HTTP请求。
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // 解析请求头部 connection 字段
    // 判断是keep-alive还是close，决定是长连接还是短连接
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t"); // 跳过空格和\t字符
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    // 解析请求头部 内容长度 字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 解析请求头部 HOST 字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 解析消息体函数。
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';

        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    //判断http请求是否被完整读入
    return NO_REQUEST;
}

// process_read通过while循环，将主状态机进行封装，对报文的每一行进行循环处理
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    /*  
    详解判断条件：
        · 主状态机转移到CHECK_STATE_CONTENT（解析消息体的状态）
        · 从状态机转移到LINE_OK
        · 两者为【或】的关系，条件为真则继续循环，否则退出
    循环体内部做的事情：
        · 从状态机读取数据
        · 调用get_line()，通过m_start_line把从状态机读取的数据间接赋给text
        · 主状态机解析text
    */
    // (m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) 的必要性：因为POST请求中，消息体末尾没有/r/n字符，不能用从状态机的状态作为循环入口。
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        // 解析请求行
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        // 解析请求头
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 完整解析GET请求后，跳转到报文响应函数
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        // 解析消息体
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            // 完整解析POST请求后，跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();
            // 这是报文解析的最后一步，避免再次循环，从状态机的状态切换为【读取中】
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 处理完请求消息之后，需要在此完成请求资源映射。
http_conn::HTTP_CODE http_conn::do_request()
{
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);

    // 找到m_url中/的位置
    const char *p = strrchr(m_url, '/');
    // 因为m_url一共只有8种情况，分别对应8种不同的操作。以下一一列举：

    // 处理cgi,实现登录和注册校验。m_url有以下两种情况。可以根据*(p + 1)是2还是3来判断具体是哪个操作
    /*
        · /2CGISQL.cgi
            · POST请求，进行登录校验
            · 验证成功跳转到welcome.html，即资源请求成功页面
            · 验证失败跳转到logError.html，即登录失败页面
        · /3CGISQL.cgi
            · POST请求，进行注册校验
            · 注册成功跳转到log.html，即登录页面
            · 注册失败跳转到registerError.html，即注册失败页面
    */
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;

        // 以&为分隔符，前面的为用户名，后面是密码
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 注册校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 判断有无重名
            if (users.find(name) == users.end())
            {
                // 向数据库中插入数据时，上锁
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                // 校验成功，跳转登录页面
                if (!res)
                    strcpy(m_url, "/log.html");
                // 校验失败，跳转注册失败页面
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    /*
        /0
        · POST请求，跳转到register.html，即注册页面
    */
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        // 将网站目录和/register.html进行拼接，更新到m_real_file中
        // strncpy函数用于将指定长度的字符串复制到字符数组中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*
        /1
        · POST请求，跳转到log.html，即登录页面
    */
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*
        /5
        · POST请求，跳转到picture.html，即图片请求页面
    */
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*
        /6
        · POST请求，跳转到video.html，即视频请求页面
    */
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*
        /7
        · POST请求，跳转到fans.html，即关注页面
    */
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /*
        / 
        · GET请求，跳转到欢迎访问页面
    */

    //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
    else
        //发送url实际请求的文件
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    // 如果要发送数据长度为0，表示响应报文为空
    if (bytes_to_send == 0)
    {
        // 重新注册写事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        // 把响应报文的状态行、消息头、空行和正文发给浏览器端。返回已发送的字节数
        temp = writev(m_sockfd, m_iv, m_iv_count);
        // 发送失败
        if (temp < 0)
        {
            // 判断缓存区是不是满了
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 发送失败且不是缓冲区问题，取消映射
            unmap();
            return false;
        }
        // 更新已发送字节
        bytes_have_send += temp;
        bytes_to_send -= temp;
        // 判断第一个iovec头部信息是否发送完，发送完了就发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 如果数据已经全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            // 重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
            
            // 如果是长连接，重新初始化HTTP对象
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 可变参数列表
    va_list arg_list;
    // 将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    // 将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        // 清空可变参列表
        va_end(arg_list);
        return false;
    }
    // 更新m_write_idx位置
    m_write_idx += len;
    // 清空可变参列表
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
// 添加状态行：http/1.1 状态码 状态消息
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加消息报头，由 响应报文的长度、连接状态和空行 组成
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
// 添加content-length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// 添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
// 添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    // 服务器内部错误。
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    // 报文语法有误
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    // 没有访问权限
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    // 访问正常
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        // 如果请求的资源存在
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // 第一个iovec指针指向【待发出的响应报文数组】，长度指向【已写入的长度】
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // 发送的全部数据位响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // 请求资源大小为0，返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
