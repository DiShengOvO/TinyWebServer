#include "webserver.h"

// 主要完成服务器初始化：http连接、设置根目录、开启定时器对象
WebServer::WebServer()
{
  // 分配MAX_FD个连接空间以供使用
  users = new http_conn[MAX_FD];

  // root文件夹路径
  char server_path[200];
  getcwd(server_path, 200);
  char root[6] = "/root";
  m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
  strcpy(m_root, server_path);
  strcat(m_root, root);

  // 为每个连接设置定时器
  users_timer = new client_data[MAX_FD];
}

// 服务器资源释放
WebServer::~WebServer()
{
  close(m_epollfd);     // 关闭epoll
  close(m_listenfd);    // 关闭监听套接字
  close(m_pipefd[1]);   // 关闭管道写段
  close(m_pipefd[0]);   // 关闭管道读端
  delete[] users;       // 释放http连接分配空间
  delete[] users_timer; // 释放定时器分配空间
  delete m_pool;        // 释放线程池分配空间
}

// 初始化用户名、数据库等信息
void WebServer::init(int port, std::string user, std::string passWord, std::string databaseName, int log_write,
                     int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
  m_port = port;
  m_user = user;
  m_passWord = passWord;
  m_databaseName = databaseName;
  m_sql_num = sql_num;
  m_thread_num = thread_num;
  m_log_write = log_write;
  m_close_log = close_log;
  m_actormodel = actor_model; // epoll的I/O处理模式：Reactor or Proactor?
  // 设置触发模式，trigmode高位为监听触发模式，低位为连接触发模式
  m_LISTENTrigmode = trigmode >> 1;
  m_CONNTrigmode = trigmode & 1;
}

// 初始化日志系统
void WebServer::log_write()
{
  if (m_close_log == 0)
  {
    // 确定日志类型：同步/异步
    if (m_log_write == 1)
    {
      // 异步：设置阻塞队列最大长度为800
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
    }
    else
    {
      // 同步：阻塞队列最大长度为0
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
  }
}

// 初始化数据库连接池
void WebServer::sql_pool()
{
  // 单例模式获取唯一实例
  m_connPool = connection_pool::GetInstance();
  // 完成连接池的初始化，在这个过程已经连接到数据库
  m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

  // 初始化数据库读取表
  users->initmysql_result(m_connPool);
}

// 创建线程池
void WebServer::thread_pool()
{
  // 线程池
  m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// 开启服务器监听事件
void WebServer::eventListen()
{
  // SOCK_STREAM 表示使用面向字节流的TCP协议
  m_listenfd = socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  assert(m_listenfd >= 0);

  int ret = 0;                                 // 函数调用返回值，用于判断是否错误
  struct sockaddr_in address;                  // 地址结构
  bzero(&address, sizeof(address));            // 清空
  address.sin_family = AF_INET;                // IPv4
  address.sin_addr.s_addr = htonl(INADDR_ANY); // 绑定到本机任意IP
  address.sin_port = htons(m_port);            // 设置监听套接字所用端口，连接套接字会复用这个端口，但会通过四元组区分

  int flag = 1;
  // 启用SO_REUSEADDR选项：①允许端口复用 ②服务器重启时跳过TIME_WAIT对端口的保护，立即重新绑定对应端口
  setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  // 绑定地址结构到连接套接字
  ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
  // 检查绑定是否成功
  assert(ret >= 0);
  // 表示全连接队列最大长度为1024
  ret = listen(m_listenfd, 1024);
  assert(ret >= 0);

  // 初始化utils，设置服务器的最小时间间隙
  utils.init(TIMESLOT);

  // epoll创建内核事件表
  epoll_event events[MAX_EVENT_NUMBER];
  m_epollfd = epoll_create1(EPOLL_CLOEXEC); // EPOLL_CLOEXEC - 执行exec时关闭epollfd
  assert(m_epollfd != -1);

  // 添加监听规则到epoll红黑树，监听套接字一般不设置为oneshot模式：触发一次后不再触发
  utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
  // 设置http_conn类中的全局变量m_epollfd（未设置前为-1表示不存在）
  http_conn::m_epollfd = m_epollfd;

  // socketpair()函数用于创建一对无名的、相互连接的套接子，目的是实现计时器通信
  ret = socketpair(PF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, m_pipefd);
  assert(ret != -1);
  // 将计时器读端可读事件注册到epoll红黑树上
  utils.addfd(m_epollfd, m_pipefd[0], false, 0);

  // 忽略SIGPIPE信号
  utils.addsig(SIGPIPE, SIG_IGN); // 向已经关闭的 TCP 连接写数据，内核会发送 SIGPIPE 信号 SIG_IGN表示忽略信号

  // 对SIGALRM和SIGTERM信号设置处理函数：将信号写入管道写端
  utils.addsig(SIGALRM, utils.sig_handler, false);
  utils.addsig(SIGTERM, utils.sig_handler, false);

  // 设置alarm
  alarm(TIMESLOT);

  // 工具类,信号和描述符基础操作
  Utils::u_pipefd = m_pipefd;
  Utils::u_epollfd = m_epollfd;
}

// 创建一个定时器节点，将连接信息挂载（绑定）到http_conn对象
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
  // http_conn对象已构造，主要是绑定套接字描述符，注册epoll事件以及传入数据库信息
  users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

  // 初始化client_data数据
  // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
  users_timer[connfd].address = client_address;
  users_timer[connfd].sockfd = connfd;
  util_timer *timer = new util_timer;
  timer->user_data = &users_timer[connfd];
  // 设置回调函数
  timer->cb_func = cb_func; // cb_func是timer.h中的一个全局函数
  time_t cur = time(NULL);
  // TIMESLOT:最小时间间隔单位为5s 计时器时长设置为3倍TIMESLOT
  timer->expire = cur + 3 * TIMESLOT;
  users_timer[connfd].timer = timer;
  // m_timer_lst为定时器容器类对象，就是一个双端链表
  utils.m_timer_lst.add_timer(timer);
}

// 若数据活跃，定时器寿命加3个时间单位 调整定时器在双向链表上的位置
void WebServer::adjust_timer(util_timer *timer)
{
  time_t cur = time(NULL);
  timer->expire = cur + 3 * TIMESLOT;
  // 调整对应计时器在链表中的位置
  utils.m_timer_lst.adjust_timer(timer);

  LOG_INFO("%s", "adjust timer once")
}

// 超时，删除定时器节点，关闭对应连接
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
  // 调用回调函数：从内核事件表删除epoll事件，关闭对应连接套接字的文件描述符，释放http_conn连接资源
  timer->cb_func(&users_timer[sockfd]);
  // 移除定时器（从链表中删除对应节点并释放空间）
  utils.m_timer_lst.del_timer(timer);

  LOG_INFO("close fd %d", users_timer[sockfd].sockfd)
}

// 处理用户连接事件：即listenfd的可读(EPOLLIN)事件
bool WebServer::dealclinetdata()
{
  struct sockaddr_in client_address;
  socklen_t client_addrlength = sizeof(client_address);
  // LT模式
  if (m_LISTENTrigmode == 0)
  {
    int connfd = accept4(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength, SOCK_NONBLOCK);
    if (connfd < 0)
    {
      LOG_ERROR("%s:errno is:%d", "accept error", errno)
      return false;
    }
    if (http_conn::m_user_count >= MAX_FD) // 超过服务器最大可同时处理连接数
    {
      /*
        值得一提的是，这里面有一个很大的设计缺陷，那就是：
        MAX_FD > max(connfd) > http_conn::m_user_count ,而在通常情况下MAX_FD >> http_conn::m_user_count
        这意味着这个判断条件的结果其实永远都是false
      */

      // 此时连接被接受，但无法分配http_conn对象进行处理（解析+响应），因此直接向客户端发送错误信息“服务器繁忙”并关闭连接
      utils.show_error(connfd, "Internal server busy");
      LOG_ERROR("%s", "Internal server busy")
      return false;
    }
    // 为connfd创建定时器并将connfd挂载到对应的connfd对象上
    timer(connfd, client_address);
  }
  // ET模式
  else
  {
    // 需要循环读取直至全连接队列为空
    while (true)
    {
      int connfd = accept4(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength, SOCK_NONBLOCK);
      if (connfd < 0)
      {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          // 全连接队列已空，正常退出
          break;
        }
        else if (errno == EINTR)
        {
          // 被信号处理中断，继续下一个循环尝试
          continue;
        }
        else
        {
          LOG_ERROR("%s:errno is:%d", "accept error", errno)
          break;
        }
      }
      if (http_conn::m_user_count >= MAX_FD)
      {
        utils.show_error(connfd, "Internal server busy");
        LOG_ERROR("%s", "Internal server busy")
        continue;
      }
      timer(connfd, client_address);
    }
  }
  return true;
}

// 处理定时器信号,根据管道读端的信号类型（SIGALRM or SIGTERM）设置相应标志位（timeout or stop_server）
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
  int ret = 0;
  int sig;
  char signals[1024];
  // 从管道读端读出信号值，成功返回字节数，失败返回-1
  // 正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
  ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
  if (ret == -1)
  {
    if (errno == EINTR)
      return true;
    else
    {
      LOG_ERROR("%s:errno is:%d", "pipefd recv error", errno)
      return false;
    }
  }
  else if (ret == 0)
  {
    LOG_ERROR("Pipe write end closed unexpectedly.")
    stop_server = true;
    return false;
  }
  else
  {
    // 处理信号值对应的逻辑
    for (int i = 0; i < ret; ++i)
    {

      // 这里面明明是字符
      switch (signals[i])
      {
      // 这里是整型
      case SIGALRM:
      {
        timeout = true;
        break;
      }
      // 关闭服务器
      case SIGTERM:
      {
        stop_server = true;
        break;
      }
      }
    }
  }
  return true;
}

// 处理连接套接字可读（EPOLLIN）事件
void WebServer::dealwithread(int sockfd)
{
  // 创建定时器临时变量，将该连接对应的定时器取出来
  util_timer *timer = users_timer[sockfd].timer;

  // Reactor模式
  if (m_actormodel == 1)
  {
    if (timer)
    {
      // 连接活跃，更新定时器:重新设置生命周期为3倍TIMESLOT
      adjust_timer(timer);
    }

    // 若监测到读事件，将该事件放入请求队列
    m_pool->append(users + sockfd, 0); // 第二个参数为0，表示是读事件
    while (true)
    {
      // 轮询判断是否完成处理，improv=1即表示完成了处理
      if (users[sockfd].improv == 1)
      {
        // 是否关闭连接判断，一般数据处理失败时timer_flag会被设置为1
        if (users[sockfd].timer_flag == 1)
        {
          deal_timer(timer, sockfd);
          users[sockfd].timer_flag = 0;
        }
        users[sockfd].improv = 0;
        break;
      }
    }
  }
  // Proactor模式
  else
  {
    // 先读取数据，再放进请求队列
    if (users[sockfd].read_once())
    {
      LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr))
      // 将该事件放入请求队列
      m_pool->append_p(users + sockfd);
      if (timer)
      {
        // 连接活跃，更新定时器:重新设置生命周期为3倍TIMESLOT
        adjust_timer(timer);
      }
    }
    else
    {
      // 虽然不算超时事件，但可以调用超时处理移除计时器并关闭连接
      deal_timer(timer, sockfd);
    }
  }
}

// 处理连接套接字可写（EPOLLOUT）事件
void WebServer::dealwithwrite(int sockfd)
{
  util_timer *timer = users_timer[sockfd].timer;
  // Reactor模式
  if (m_actormodel == 1)
  {
    if (timer)
    {
      adjust_timer(timer);
    }

    m_pool->append(users + sockfd, 1); // 第二个参数为1，表示是写事件

    while (true)
    {
      if (users[sockfd].improv == 1)
      {
        if (users[sockfd].timer_flag == 1)
        {
          deal_timer(timer, sockfd);
          users[sockfd].timer_flag = 0;
        }
        users[sockfd].improv = 0;
        break;
      }
    }
  }
  else
  {
    // Proactor模式
    if (users[sockfd].write())
    {
      // 写成功
      LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

      // 更新计时器
      if (timer)
      {
        adjust_timer(timer);
      }
    }
    else
    {
      deal_timer(timer, sockfd);
    }
  }
}

// 事件循环（即服务器主线程）
void WebServer::eventLoop()
{
  // 定义超时和服务器关闭标志以便进行对SIGALRM与SIGTERM信号的处理
  bool timeout = false;
  bool stop_server = false;
  while (!stop_server)
  {

    // epoll等待事件（阻塞）
    int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
    /*
    EINTR错误的产生： 系统调用被信号（signal）中断，即当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
    例如：在socket服务器端，设置了信号捕获机制，有子进程，
    当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
    在epoll_wait时，因为设置了alarm定时触发警告，导致每次返回-1，errno为EINTR，对于这种错误返回
    忽略这种错误，让epoll报错误号为4时，再次做一次epoll_wait
    */
    if (number < 0 && errno != EINTR)
    {
      LOG_ERROR("%s", "epoll failure")
      break;
    }
    // 对所有就绪事件进行处理，epoll监听三类事件，fd分别为:1.监听套接字fd 2.连接套接字fd 3.计时器所用通信管道的读端fd
    for (int i = 0; i < number; i++)
    {
      // 获取事件fd，便于后续传参给相应的事件处理函数
      int sockfd = events[i].data.fd;
      // 获取连接套接字事件的指针，用以访问对应的http_conn对象以及client_data计时器封装对象
      auto connection = events[i].data.ptr;
      // ===情况1：处理新到的客户连接===
      if (sockfd == m_listenfd)
      {
        bool flag = dealclinetdata();
        if (flag == false)
        {
          LOG_ERROR("%s", "dealclientdata failure")
          continue;
        }
      }
      // ===情况2：处理定时器所产生信号，即计时器通信管道读端（m_pipefd[0]）可读（EPOLLIN）事件===
      else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
      {
        // 接收到SIGALRM信号，timeout设置为True
        bool flag = dealwithsignal(timeout, stop_server);
        if (flag == false)
          LOG_ERROR("%s", "dealwithsignal failure")
      }
      // ===情况3：处理客户连接上接收到的数据，即可读（EPOLLIN）事件===
      else if (events[i].events & EPOLLIN)
      {
        dealwithread(sockfd);
      }
      // ===情况4：处理客户连接上send的数据，即可写（EPOLLOUT）事件===
      else if (events[i].events & EPOLLOUT)
      {
        dealwithwrite(sockfd);
      }
      // ===情况5：处理异常事件===
      else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
      {
        // EPOLLRDHUP：TCP 层对端关闭
        // EPOLLHUP：socket连接 挂起 /不可用/已失效，往往与EPOLLERR一起返回
        // EPOLLERR：socket上发生了错误
        // 服务器端关闭连接，移除对应的定时器
        util_timer *timer = users_timer[sockfd].timer; // 指针指向对应定时器
        deal_timer(timer, sockfd);                     // 移除定时器并关闭sockfd对应连接
      }
    }
    // 处理定时器为非必须事件，收到信号并不是立马处理 而是完成读写事件后，再进行处理
    if (timeout)
    {
      // alarm触发，timer_handler()函数会调用tick函数检查计时器链表，删除超时节点并通过回调函数进行处理对应连接，然后重新设置alarm
      utils.timer_handler();
      LOG_INFO("%s", "timer tick");
      timeout = false;
    }
  }
}