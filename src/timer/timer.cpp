#include "timer.h"
#include "../http/http_conn.h"

// 初始化静态(static)成员变量
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

// 定时器容器类的构造函数
sort_timer_lst::sort_timer_lst()
{
  head = NULL;
  tail = NULL;
}

// 析构函数
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

// 添加定时器
void sort_timer_lst::add_timer(util_timer *timer)
{
  if (!timer) // 空指针
  {
    return;
  }
  if (!head) // 链表为空
  {
    head = tail = timer;
    return;
  }
  // 定时器中是按照expire从小到大排序
  // 如果新的定时器超时时间小于当前头部结点
  // 直接将当前定时器结点作为头部结点
  if (timer->expire < head->expire)
  {
    timer->next = head;
    head->prev = timer;
    head = timer;
    return;
  }
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
  // 被调整的定时器在链表尾部
  // or 定时器超时值仍然小于下一个定时器超时值，不调整
  if (!tmp || (timer->expire < tmp->expire))
  {
    return;
  }

  // 定时器超时值大于下一个定时器超时值，将定时器取出，重新插入
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

// 删除定时器:既是双向链表节点的删除
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

  // 遍历定时器链表
  while (tmp)
  {
    // if当前时间小于定时器的超时时间，后面的定时器也没有到期
    if (cur < tmp->expire)
    {
      break;
    }

    // 当前定时器到期，则调用回调函数，执行定时事件
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

// 加入新的定时器,第二个参数为待加入链表部分
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
  util_timer *prev = lst_head;
  util_timer *tmp = prev->next;
  // 从双向链表中找到该定时器应该放置的位置
  // 即遍历一遍双向链表找到对应的位置
  // 由于该函数会被单参数同名函数与自身调用
  // 在调用者单参同名函数逻辑中已经处理了应该插入在head之前的情况，因此调用这个函数默认定时器应该插入head之后
  while (tmp)
  {
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

  // 遍历完发现，目标定时器需要放到尾结点处
  if (!tmp)
  {
    // 此时prev指向尾结点
    prev->next = timer;
    timer->prev = prev;
    timer->next = NULL;
    tail = timer;
  }
}

// 初始化：设置时隙slot
void Utils::init(int timeslot)
{
  // 设置alarm时隙，每次调用定时检查事件后都要重新设置alarm()
  m_TIMESLOT = timeslot;
}

// 将内核事件表注册读事件，ET模式，是否选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
  epoll_event event;
  event.data.fd = fd;

  // EPOLLIN ：可读事件  EPOLLRDHUP：对端连接关闭事件
  if (1 == TRIGMode)
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
  else
    event.events = EPOLLIN | EPOLLRDHUP;

  // 如果对描述符socket注册了EPOLLONESHOT事件，
  // 那么操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次。
  if (one_shot)
    event.events |= EPOLLONESHOT;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

// 信号处理函数
void Utils::sig_handler(int sig)
{
  /*
  为什么要去保存errno并还原？原因如下：
  1.sigaction是在内核中注册了对特定信号的处理策略，触发的是中断，中断和普通异步最大的区别是中断要打断并占据当前线程
  2.中断的现场保护中并不存在对errno这种C标准库定义的全局变量的维护，必须手动实现现场保护
  */
  int save_errno = errno;
  char msg = sig;
  //  将信号值从管道写端写入，传输字符类型
  write(u_pipefd[1], &msg, 1); // 一个字节写入，默认原子性操作，不需要进行额外保护
  errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
  // 创建sigaction结构体
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
  sa.sa_handler = handler;
  if (restart)
    sa.sa_flags |= SA_RESTART; // 信号中断时，重启该系统调用
  // 将所有信号添加到信号集中
  sigfillset(&sa.sa_mask); // sa_mask的初始化

  // 执行sigaction函数
  assert(sigaction(sig, &sa, NULL) != -1);
  // 断言，如果为false则打印错误信息并调用abort()终止程序
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
  m_timer_lst.tick(); // 检查并处理定时器超时事件

  // 重新设置SIGALRM信号，以不断触发timer_handler函数，最小的时间单位为5s
  alarm(m_TIMESLOT);
}

// 向连接套接字发送错误信息并关闭连接
void Utils::show_error(int connfd, const char *info)
{
  send(connfd, info, strlen(info), 0);
  close(connfd);
}

// 定时器回调函数:从内核事件表删除事件，关闭文件描述符，释放连接资源
void cb_func(client_data *user_data)
{
  // 删除非活动连接在socket上的注册事件
  epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
  assert(user_data);
  // 删除非活动连接在socket上的注册事件
  close(user_data->sockfd);
  // 减少连接数
  http_conn::m_user_count--;
}
