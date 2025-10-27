#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <iostream>
#include <list>
#include <string>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <error.h>
#include <mysql/mysql.h>
#include "../log/log.h"

class connection_pool
{
public:
  MYSQL *GetConnection();              // 获取数据库连接
  bool ReleaseConnection(MYSQL *conn); // 释放连接
  int GetFreeConn();                   // 获取一个空闲连接
  void DestroyPool();                  // 销毁所有连接

  // 利用局部静态变量懒汉模式实现单例模式
  static connection_pool *GetInstance();

  // 初始化
  void init(std::string url, std::string User, std::string PassWord, std::string DataBaseName, int Port, int MaxConn, int close_log);

private:
  connection_pool();
  ~connection_pool();

  int m_MaxConn;                // 最大连接数
  int m_CurConn;                // 当前已使用的连接数
  int m_FreeConn;               // 当前空闲的连接数
  std::mutex m_mtx;             // 互斥锁
  std::condition_variable m_cv; // 条件变量
  std::list<MYSQL *> connList;  // 连接池，通过链表管理

public:
  std::string m_url;          // 主机地址
  std::string m_Port;         // 数据库端口号
  std::string m_User;         // 登陆数据库用户名
  std::string m_PassWord;     // 登陆数据库密码
  std::string m_DatabaseName; // 使用数据库名
  int m_close_log;            // 日志开关
};

// RAII封装
class connectionRAII
{

public:
  connectionRAII(MYSQL **con, connection_pool *connPool);
  ~connectionRAII();

private:
  MYSQL *conRAII;
  connection_pool *poolRAII;
};

#endif