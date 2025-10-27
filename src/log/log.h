#ifndef __LOG_H__
#define __LOG_H__

#include <iostream>
#include <cstring>
#include <string>
#include <cstdarg> //提供一组宏访问可变参数
#include <thread>
#include <chrono>
#include <ctime>
#include "block_queue.h"

class Log
{
public:
  // 获取单例实例对象
  static Log *get_instance()
  {
    static Log instance;
    return &instance;
  }

  // 异步写日志的public接口，调用private方法async_write_log
  static void *flush_log_thread(void *args)
  {
    // 通过获取单例对象调用异步写日志操作
    Log::get_instance()->async_write_log();
    return nullptr;
  }

  // 可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
  bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

  // 将输出内容按照标准格式整理，level表示日志分级
  void write_log(int level, const char *format, ...);

  // 强制刷新缓冲区
  void flush(void);

private:
  // 构造函数
  Log();
  // 析构函数
  virtual ~Log();
  // 禁用拷贝
  Log(const Log &) = delete;
  Log &operator=(const Log &) = delete;
  // 禁用移动
  Log(const Log &&) = delete;
  Log &operator=(const Log &&) = delete;

  // 异步写日志
  void *async_write_log()
  {
    std::string single_log;
    // 从阻塞队列中取出一个日志single_log，写入文件
    while (m_log_queue->pop(single_log)) // 队列受锁与信号量保护
    {
      std::lock_guard<std::mutex> lock(m_mtx); // 提前枷锁由于循环问题会导致阻塞队列无法被写入
      fputs(single_log.c_str(), m_fp);
    }
    return nullptr;
  }

private:
  char dir_name[128];                    // 路径名
  char log_name[128];                    // log文件名
  int m_split_lines;                     // 日志最大行数
  int m_log_buf_size;                    // 日志缓冲区大小
  long long m_count;                     // 日志行数记录
  int m_today;                           // 因为按天分类,记录当前时间是那一天
  FILE *m_fp;                            // 打开log的文件指针
  char *m_buf;                           // 要输出的内容
  block_queue<std::string> *m_log_queue; // 阻塞队列
  bool m_is_async;                       // 是否同步标志位
  std::mutex m_mtx;                      // 互斥锁
  int m_close_log;                       // 关闭日志
};

// 由于日志类为单例模式，其中方法不会被其他程序直接调用，下面四个可变参数宏提供了其他程序的调用方法。

// Debug，调试代码时的输出，在系统实际运行时，一般不使用。
#define LOG_DEBUG(format, ...)                                \
  if (0 == m_close_log)                                       \
  {                                                           \
    Log::get_instance()->write_log(0, format, ##__VA_ARGS__); \
    Log::get_instance()->flush();                             \
  }

// Info，报告系统当前的状态，当前执行的流程或接收的信息等。
#define LOG_INFO(format, ...)                                 \
  if (0 == m_close_log)                                       \
  {                                                           \
    Log::get_instance()->write_log(1, format, ##__VA_ARGS__); \
    Log::get_instance()->flush();                             \
  }

// Warn，这种警告与调试时终端的warning类似，同样是调试代码时使用。
#define LOG_WARN(format, ...)                                 \
  if (0 == m_close_log)                                       \
  {                                                           \
    Log::get_instance()->write_log(2, format, ##__VA_ARGS__); \
    Log::get_instance()->flush();                             \
  }

// Error，输出系统的错误信息。
#define LOG_ERROR(format, ...)                                \
  if (0 == m_close_log)                                       \
  {                                                           \
    Log::get_instance()->write_log(3, format, ##__VA_ARGS__); \
    Log::get_instance()->flush();                             \
  }
//__VA_ARGS__是一个可变参数的宏
// 前面加上##的作用在于，当可变参数的个数为0时，这里printf参数列表中的的##会把前面多余的","去掉，否则会编译出错
#endif