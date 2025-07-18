#ifndef LST_TIMER_
#define LST_TIMER_

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../log/log.h"

class util_timer;

struct client_data {
    sockaddr_in address;  // 客户端地址信息
    int sockfd;           // 客户端 socket 描述符
    util_timer* timer;    // 与该客户端关联的定时器
};

// 链表节点类 - 单个定时器对象
class util_timer {
   public:
    util_timer() : prev(NULL), next(NULL) {}

   public:
    time_t expire;  // 定时器超时时间点

    void (*cb_func)(client_data*);  // 超时后的回调函数
    client_data* user_data;         // 客户端数据（socket、地址等）
    util_timer* prev;               // 指向前一个定时器
    util_timer* next;               // 指向后一个定时器
};

// 链表管理类 - 管理多个定时器（util_timer）的升序链表
class sort_timer_lst {
   public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer* timer);
    void adjust_timer(util_timer* timer);
    void del_timer(util_timer* timer);
    void tick();

   private:
    void add_timer(util_timer* timer, util_timer* lst_head);

    util_timer* head;
    util_timer* tail;
};

class Utils {
   public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sig_handler(int sig);

    // 设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发 SIGNALRM 信号
    void timer_handler();

    void show_error(int connfd, const char* info);

   public:
    sort_timer_lst m_timer_lst;  // 定时器链表管理器
    int m_TIMESLOT;              // 定时器定时槽（秒）
    static int* u_pipefd;        // 信号通过管道通知主线程
    static int u_epollfd;        // epoll 文件描述符，用于信号集成
};

void cb_func(client_data* user_data);

#endif  // !LST_TIMER_