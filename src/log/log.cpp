#include "log.h"

// 构造函数
Log::Log() : m_count(0), m_is_async(false) {}

// 析构函数，负责关闭日志文件指针
Log::~Log()
{
  if (m_fp != NULL)
    fclose(m_fp);
}

// 日志初始化函数
// 异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
  // 如果设置了max_queue_size,则设置为异步
  // 因为容量为0说明无缓冲区存放日志内容，日志生成后必须同步执行写入日志操作
  if (max_queue_size >= 1)
  {
    m_is_async = true;
    m_log_queue = new block_queue<std::string>(max_queue_size); // 创建一个string的阻塞队列
    std::thread tid(flush_log_thread, nullptr);                 // flush_log_thread为回调函数指针,这里表示创建线程异步写日志
    tid.detach();
  }

  m_close_log = close_log;
  m_log_buf_size = log_buf_size;
  m_buf = new char[m_log_buf_size];
  memset(m_buf, '\0', m_log_buf_size);
  m_split_lines = split_lines;

  // 获取当前时间点并转换为time_t:UTC秒数
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);

  // 将UTC秒数转换为本地时间结构tm（包含年月时日分秒等信息）
  struct tm *sys_tm = std::localtime(&t);
  struct tm my_tm = *sys_tm;

  // 从后往前找到第一个/的位置，分离路径和文件名
  const char *p = strrchr(file_name, '/');
  char log_full_name[256] = {0};

  // 若输入的文件名没有'/'，则直接将时间+文件名作为日志名，相当于自定义日志名
  if (p == NULL)
  {
    snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
  }
  else
  {
    // 记录文件名
    strcpy(log_name, p + 1);
    // 记录路径名
    strncpy(dir_name, file_name, static_cast<size_t>(p - file_name + 1));
    // 日志名为：目录名+年_月_日+文件名
    snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
  }

  // 记录当前创建日志天数
  m_today = my_tm.tm_mday;

  // 设置日志文件指针
  m_fp = fopen(log_full_name, "a"); // a:以追加模式打开只写文件
  if (m_fp == NULL)
  {
    // 文件打开失败
    return false;
  }

  return true;
}

// 生成日志并根据m_is_async执行不同的写入操作
void Log::write_log(int level, const char *format, ...)
{
  // 获取调用该日志生成函数时的时间
  auto now_chrono = std::chrono::system_clock::now();
  auto now_time_t = std::chrono::system_clock::to_time_t(now_chrono);
  // 获取精确到微秒时间小数点右侧直至微秒级的部分
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now_chrono.time_since_epoch() % std::chrono::seconds(1))
                    .count();

  struct tm *sys_tm = std::localtime(&now_time_t);
  struct tm my_tm = *sys_tm;

  // 根据日志分级填写日志首部
  char s[16] = {0};
  switch (level)
  {
  case 0:
    strcpy(s, "[debug]:");
    break;
  case 1:
    strcpy(s, "[info]:");
    break;
  case 2:
    strcpy(s, "[warn]:");
    break;
  case 3:
    strcpy(s, "[erro]:");
    break;
  default:
    strcpy(s, "[info]:");
    break;
  }
  // 写入一行log，对m_count++, m_split_lines最大行数
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_count++;                                                    // 日志行数
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) // 天数不同或该日志文件行数写满
    {

      char new_log[256] = {0};
      // 强制刷新写缓冲区
      fflush(m_fp);
      fclose(m_fp);

      // 记录当前时间（年_月_日）
      char tail[16] = {0};
      snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

      if (m_today != my_tm.tm_mday) // 天数不同
      {
        // 设置新日志文件名并更新m_today
        snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
        m_today = my_tm.tm_mday;
        m_count = 0;
      }
      else // 该日志文件已满
      {
        // 设置同一天的新日志文件名：<目录名>+<年_月_日>+<文件名>.<序号> , 同一天的第一个文件序号为0，默认不标识
        snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
      }

      // 切换文件指针指向新日志文件
      m_fp = fopen(new_log, "a");
    }
  }

  va_list valst;
  va_start(valst, format); // 将传入的format参数赋值给valst，便于格式化输出

  std::string log_str;

  {
    std::lock_guard<std::mutex> lock(m_mtx);

    // 写入的具体时间内容格式
    long usec = static_cast<long>(now_us);

    // 写入时间（精确至小数点后微秒部分）+类型的日志头，eg: "2025-08-20 04:40:19.472125 [info]: "
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, usec, s);

    // 根据通过宏接受的格式与参数写入日志具体内容
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';     // 日志换行
    m_buf[n + m + 1] = '\0'; // 字符串结束标志
    log_str = m_buf;         // 到此为止，一行日志记录生成完毕
  }

  // 异步写日志，只把生成的一行日志放入阻塞队列
  // 日志单例对象构造并初始化时，若为异步就已经启动了从阻塞队列取日志记录并写入日志文件的线程
  if (m_is_async && !m_log_queue->full())
  {
    m_log_queue->push(log_str); // 队列受锁与信号量保护
  }
  else
  {
    // 同步任务通过互斥锁进行生成与写入同步操作
    std::lock_guard<std::mutex> lock(m_mtx);
    fputs(log_str.c_str(), m_fp);
  }

  va_end(valst);
}

void Log::flush(void)
{
  std::lock_guard<std::mutex> lock(m_mtx);
  // 强制刷新写入流缓冲区
  fflush(m_fp);
}
