#ifndef CONFIG_H
#define CONFIG_H

#include "../webserver/webserver.h"

// 配置信息类
class Config
{
public:
  Config();
  ~Config() {};

  // 命令行参数解析函数
  void parse_arg(int argc, char *argv[]);

  // 端口号
  int PORT;

  // 日志写入方式
  int LOGWrite; // 同步（生成和写入同步进行） or 异步（生成日志放入阻塞队列，写操作从阻塞队列取并写入）

  // 触发组合模式
  int TRIGMode; // 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET

  // listenfd触发模式
  int LISTENTrigmode; // LT or ET

  // connfd触发模式
  int CONNTrigmode; // LT or ET

  // 数据库连接池数量
  int sql_num;

  // 线程池内的线程数量
  int thread_num;

  // 是否关闭日志
  int close_log;

  // 并发模型选择
  int actor_model; // Reactor or Proactor
};

#endif