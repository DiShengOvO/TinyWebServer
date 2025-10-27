#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "../threadpool/threadpool.h"
#include "../http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

// http_conn对象与timer封装
struct user_data
{
  int fd;                  // 文件描述符
  http_conn users_conn;    // HTTP连接对象
  client_data users_timer; // 计时器封装对象
};

class WebServer
{
public:
  WebServer();
  ~WebServer();

  void init(int port, std::string user, std::string passWord, std::string databaseName,
            int log_write, int trigmode, int sql_num,
            int thread_num, int close_log, int actor_model);

  void thread_pool();                                        // 创建线程池
  void sql_pool();                                           // 创建连接池
  void log_write();                                          // 启动日志系统
  void eventListen();                                        // 启动监听事件
  void eventLoop();                                          // 启动事务处理循环
  void timer(int connfd, struct sockaddr_in client_address); // 设置定时器
  void adjust_timer(util_timer *timer);                      // 调整定时器
  void deal_timer(util_timer *timer, int sockfd);            // 关闭定时器
  bool dealclinetdata();                                     // 处理用户数据
  bool dealwithsignal(bool &timeout, bool &stop_server);     // 处理定时器产生的SIGALRM和SIGTERM信号
  void dealwithread(int sockfd);                             // 读客户端发送数据
  void dealwithwrite(int sockfd);                            // 写发给客户端数据

public:
  // 服务器基础信息
  int m_port;       // 端口
  char *m_root;     // 根目录
  int m_log_write;  // 日志类型：同步 or 异步？
  int m_close_log;  // 是否禁用日志系统
  int m_actormodel; // 处理模式：Reactor/Proactor

  // 网络信息
  int m_pipefd[2];  // 相互连接的套接字，用于传递定时器信号
  int m_epollfd;    // epoll对象
  http_conn *users; // 单个http连接

  // 数据库相关
  connection_pool *m_connPool; // 数据库连接池单例模式对象
  std::string m_user;          // 登陆数据库用户名
  std::string m_passWord;      // 登陆数据库密码
  std::string m_databaseName;  // 使用数据库名
  int m_sql_num;               // 数据库连接池数量

  // 线程池相关
  threadpool<http_conn> *m_pool;
  int m_thread_num; // 线程池内线程数

  // epoll_event相关
  epoll_event events[MAX_EVENT_NUMBER];

  int m_listenfd;       // 监听套接字
  int m_OPT_LINGER;     // 是否优雅下线
  int m_LISTENTrigmode; // listenfd:ET/LT
  int m_CONNTrigmode;   // connfd:ET/LT

  // 定时器相关
  client_data *users_timer;
  // 工具类
  Utils utils;
};
#endif
