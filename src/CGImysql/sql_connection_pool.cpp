#include "sql_connection_pool.h"

/*连接池类方法实现*/
// 构造函数
connection_pool::connection_pool() : m_CurConn(0), m_FreeConn(0) {}

// 获取单例实例对象
connection_pool *connection_pool::GetInstance()
{
  static connection_pool connPool;
  return &connPool;
}

// 构造初始化
void connection_pool::init(std::string url, std::string User, std::string PassWord, std::string DBName, int Port, int MaxConn, int close_log)
{
  m_url = url;
  m_Port = Port;
  m_User = User;
  m_PassWord = PassWord;
  m_DatabaseName = DBName;
  m_close_log = close_log;

  for (int i = 0; i < MaxConn; i++)
  {
    // 创建一个MYSQL对象
    MYSQL *con = NULL;
    con = mysql_init(con);

    if (con == NULL)
    {
      LOG_ERROR("mysql_init error")
      exit(1);
    }
    // 创建到MYSQL服务器的实际连接
    con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

    if (con == NULL)
    {
      LOG_ERROR("mysql_real_connect error")
      exit(1);
    }
    connList.push_back(con); // 放入连接池管理链表
    ++m_FreeConn;            // 空闲（可用）连接数加1
  }
  m_MaxConn = m_FreeConn; // 最大连接数 = 空闲连接数+已使用连接数
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
  MYSQL *con = NULL;

  if (connList.size() == 0)
    return NULL;

  std::unique_lock<std::mutex> lock(m_mtx);
  m_cv.wait(lock, [this]()
            { return m_FreeConn > 0; });
  con = connList.front(); // 获取链表最前端链接
  connList.pop_front();   // 去除链表最前端连接
  --m_FreeConn;
  ++m_CurConn;

  return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
  if (NULL == con)
    return false;

  {
    std::lock_guard<std::mutex> lock(m_mtx);
    connList.push_back(con); // 归还连接至链表尾部
    ++m_FreeConn;
    --m_CurConn;
  }
  m_cv.notify_one();

  return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{
  std::lock_guard<std::mutex> lock(m_mtx);
  if (connList.size() > 0)
  {
    std::list<MYSQL *>::iterator it;
    for (it = connList.begin(); it != connList.end(); ++it)
    {
      MYSQL *con = *it;
      mysql_close(con);
    }
    m_CurConn = 0;
    m_FreeConn = 0;
    connList.clear();
  }
}

// 当前空闲的连接数
int connection_pool::GetFreeConn()
{
  return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
  DestroyPool(); // 销毁数据库连接池
}

/*RAII封装类方法实现*/
// 构造函数
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
  // 从连接池中取出一个空闲连接
  *SQL = connPool->GetConnection();
  this->conRAII = *SQL;
  this->poolRAII = connPool;
}

// 析构函数
connectionRAII::~connectionRAII()
{
  // 释放连接资源，即归还给连接池
  this->poolRAII->ReleaseConnection(this->conRAII);
}