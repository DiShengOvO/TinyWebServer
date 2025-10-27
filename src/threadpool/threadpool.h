#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <vector>
#include <cstdio>
#include <exception>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "../CGImysql/sql_connection_pool.h"
#include "../log/log.h"

template <typename T>
class threadpool
{
public:
  /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
  threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
  ~threadpool();
  bool append(T *request, int state); // Reactor模式
  bool append_p(T *request);          // Proactor模式

private:
  void worker();

private:
  int m_max_requests;                 // 请求队列中允许的最大请求数
  std::vector<std::thread> m_threads; // 描述线程池的数组
  std::list<T *> m_workqueue;         // 请求队列
  std::mutex m_mtx;                   // 保护请求队列的互斥锁
  std::condition_variable m_cv;       // 条件变量
  connection_pool *m_connPool;        // 数据库
  int m_actor_model;                  // 0为Proactor模式，1为Reactor模式
  bool m_stop;                        // 线程池是否停止工作
};

template <typename T>
// 线程池构造函数
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model), m_max_requests(max_requests), m_connPool(connPool), m_stop(false)
{
  if (thread_number <= 0 || max_requests <= 0)
    throw std::exception();
  m_threads.reserve(thread_number);
  for (int i = 0; i < thread_number; ++i)
  {
    m_threads.emplace_back([this]()
                           { this->worker(); });
  }
}
template <typename T>
threadpool<T>::~threadpool()
{
  {
    std::unique_lock<std::mutex> lock(m_mtx);
    // 更改停止标志
    m_stop = true;
  }
  // 通知所有阻塞中的线程
  m_cv.notify_all();
  // 确保线程执行完
  for (auto &t : m_threads)
  {
    if (t.joinable())
      t.join();
  }
}

template <typename T>
// Reactor模式下的请求入队,I/O事件要分读写逻辑
bool threadpool<T>::append(T *request, int state)
{
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_stop || (m_workqueue.size() >= m_max_requests))
      return false;

    // I/0事件种类，0为读，1为写
    request->m_state = state;
    m_workqueue.push_back(request);
  }
  m_cv.notify_one();
  return true;
}

template <typename T>
// Proactor模式下的请求入队
bool threadpool<T>::append_p(T *request)
{
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_stop || (m_workqueue.size() >= m_max_requests))
      return false;
    m_workqueue.push_back(request);
  }
  m_cv.notify_one();
  return true;
}

// 线程池中的所有线程都睡眠，等待请求队列中新增任务
template <typename T>
void threadpool<T>::worker()
{
  while (true)
  {
    try
    {
      T *request = nullptr;
      std::unique_lock<std::mutex> lock(m_mtx);
      m_cv.wait(lock, [this]()
                { return this->m_stop || !this->m_workqueue.empty(); });
      if (m_stop && m_workqueue.empty())
        break;
      if (!m_workqueue.empty())
      {
        request = m_workqueue.front();
        m_workqueue.pop_front();
      }

      // Reactor模式
      if (m_actor_model == 1)
      {
        // 读事件
        if (request->m_state == 0)
        {
          if (request->read_once())
          {
            request->improv = 1;
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
          }
          else // 失败
          {
            request->improv = 1;     // 1表示数据处理完毕
            request->timer_flag = 1; // 1表示关闭连接
          }
        }
        else // 写事件
        {
          if (request->write())
          {
            request->improv = 1;
          }
          else // 失败
          {
            request->improv = 1;
            request->timer_flag = 1;
          }
        }
      }
      else // Proactor模式
      {
        // 线程池不需要进行数据读取，而是直接开始业务处理
        // 之前的操作已经将数据读取到http的read和write的buffer中了
        connectionRAII mysqlcon(&request->mysql, m_connPool);
        request->process();
      }
    }
    catch (const std::exception &e)
    {
    }
  }
}
#endif
