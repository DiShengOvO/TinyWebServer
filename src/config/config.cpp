#include "config.h"

Config::Config()
{
  // 端口号,默认9006
  PORT = 9006;

  // 日志写入方式，默认同步
  LOGWrite = 0;

  // 触发组合模式,默认listenfd LT + connfd LT
  TRIGMode = 0;

  // listenfd触发模式，默认LT
  LISTENTrigmode = 0;

  // connfd触发模式，默认LT
  CONNTrigmode = 0;

  // 数据库连接池数量,默认8
  sql_num = 8;

  // 线程池内的线程数量,默认8
  thread_num = 8;

  // 关闭日志,默认不关闭
  close_log = 0;

  // 并发模型,默认是proactor
  actor_model = 0;
}

void Config::parse_arg(int argc, char *argv[])
{
  int opt;
  const char *str = "p:l:m:o:s:t:c:a:";
  while ((opt = getopt(argc, argv, str)) != -1)
  {
    switch (opt)
    {
    case 'p': // 端口号
    {
      PORT = atoi(optarg);
      break;
    }
    case 'l': // 日志写入方式：0=同步写，1=异步写
    {
      LOGWrite = atoi(optarg);
      break;
    }
    case 'm': // 出发组合模式：0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET
    {
      TRIGMode = atoi(optarg);
      break;
    }
    case 's': // 数据库连接池大小
    {
      sql_num = atoi(optarg);
      break;
    }
    case 't': // 线程池线程数
    {
      thread_num = atoi(optarg);
      break;
    }
    case 'c': // 是否关闭日志：0=开启日志，1=关闭日志
    {
      close_log = atoi(optarg);
      break;
    }
    case 'a': // 是否启用 Reactor 模式（Proactor）：0=半同步半反应堆，1=Proactor
    {
      actor_model = atoi(optarg);
      break;
    }
    default:
      break;
    }
  }
}