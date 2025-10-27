#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <mutex>
#include <condition_variable>

// 循环队列
template <class T>
class block_queue
{
public:
  // 构造函数，队列默认最大长度为1000
  block_queue(int max_size = 1000) : m_max_size(max_size), m_size(0), m_front(-1), m_back(-1)
  {
    if (max_size <= 0)
    {
      exit(-1);
    }
    m_array = new T[max_size];
  }

  // 清空队列
  void clear()
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_size = 0;
    m_front = -1;
    m_back = -1;
  }

  // 析构函数
  ~block_queue()
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_array != NULL)
      delete[] m_array;
  }

  // 判断队列是否满了
  bool full()
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_size >= m_max_size)
    {
      return true;
    }
    return false;
  }

  // 判断队列是否为空
  bool empty()
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_size == 0)
    {
      return true;
    }
    return false;
  }

  // 返回队首元素
  bool front(T &value)
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_size == 0)
    {
      return false;
    }
    value = m_array[m_front];
    return true;
  }

  // 返回队尾元素
  bool back(T &value)
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (0 == m_size)
    {
      return false;
    }
    value = m_array[m_back];
    return true;
  }

  // 获取队列当前长度
  int size()
  {
    int tmp = 0;

    std::lock_guard<std::mutex> lock(m_mtx);
    tmp = m_size;

    return tmp;
  }

  // 获取队列最大长度
  int max_size()
  {
    int tmp = 0;

    std::lock_guard<std::mutex> lock(m_mtx);
    tmp = m_max_size;

    return tmp;
  }

  // 往队列添加元素，需要将所有使用队列的线程先唤醒
  // 当有元素push进队列,相当于生产者生产了一个元素
  // 若当前没有线程等待条件变量,则唤醒无意义
  bool push(const T &item)
  {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (m_size >= m_max_size)
    {
      m_cv.notify_all();
      return false;
    }

    m_back = (m_back + 1) % m_max_size;
    m_array[m_back] = item;

    m_size++;

    m_cv.notify_all();
    return true;
  }
  // pop时,如果当前队列没有元素,将会等待条件变量
  bool pop(T &item)
  {
    std::unique_lock<std::mutex> lock(m_mtx);
    m_cv.wait(lock, [this]
              { return m_size > 0; });
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    m_size--;
    return true;
  }

private:
  std::mutex m_mtx;
  std::condition_variable m_cv;

  T *m_array;     // 以数组形式实现队列
  int m_size;     // 队列长度
  int m_max_size; // 队列最大长度
  int m_front;    // 头部指针
  int m_back;     // 尾部指针
};

#endif
