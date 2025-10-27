#include "./config/config.h"

// 服务器主程序，调用WebServer类实现Web服务器
int main(int argc, char *argv[])
{
  // 需要修改的数据库信息,登录名,密码,库名
  std::string user = "root";
  std::string passwd = "123";
  std::string databasename = "webserDb";

  // 命令行解析
  Config config;
  config.parse_arg(argc, argv);

  WebServer server;

  // 初始化
  server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
              config.TRIGMode, config.sql_num, config.thread_num,
              config.close_log, config.actor_model);

  // 日志：初始化后第一个启动的
  server.log_write();
  // 数据库
  server.sql_pool();
  // 线程池
  server.thread_pool();
  // 监听
  server.eventListen();
  // 运行
  server.eventLoop();

  return 0;
}