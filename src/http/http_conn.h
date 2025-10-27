#ifndef __HTTP_CONN_H__
#define __HTTP_CONN_H__

#include <assert.h>
#include <cstdio>
#include <stdlib.h>
#include <string.h>
#include <cstdarg>
#include <map>
#include <errno.h>
#include <signal.h>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include "../CGImysql/sql_connection_pool.h"
#include "../timer/timer.h"
#include "../log/log.h"

class http_conn
{
public:
  // 设置读取文件的名称m_real_file大小
  static const int FILENAME_LEN = 200;
  // 设置读缓冲区m_read_buf大小
  static const int READ_BUFFER_SIZE = 2048;
  // 设置写缓冲区m_write_buf大小
  static const int WRITE_BUFFER_SIZE = 1024;
  // 报文的请求方法，本项目只用到GET和POST
  enum METHOD
  {
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATH
  };
  // 主状态机状态，检查请求报文中元素
  enum CHECK_STATE
  {
    CHECK_STATE_REQUESTLINE = 0, // 解析请求行
    CHECK_STATE_HEADER,          // 解析请求头
    CHECK_STATE_CONTENT          // 解析消息体，仅用于解析POST请求
  };
  // HTTP状态码
  enum HTTP_CODE
  {
    // parse过程可能返回值
    NO_REQUEST,        // 未接收到请求
    GET_REQUEST,       // GET请求
    BAD_REQUEST,       // 错误请求（该返回值在do_request过程也可能出现，比如请求文件名实际是一个目录名）
    INTERNAL_ERROR,    // 服务器内部错误
                       // do_request过程可能返回值
    FILE_REQUEST,      // 文件请求
    NO_RESOURCE,       // 未找到资源
    FORBIDDEN_REQUEST, // 禁止访问
                       // 未实际使用
    CLOSED_CONNECTION  // 连接关闭
  };
  // 从状态机的状态，文本解析是否成功
  enum LINE_STATUS
  {
    LINE_OK = 0, // 完整读取一行
    LINE_BAD,    // 报文语法有误
    LINE_OPEN    // 读取的行不完整
  };

public:
  http_conn() {}
  ~http_conn() {}

public:
  // 初始化套接字
  void init(int sockfd, const sockaddr_in &addr, char *, int, int, std::string user, std::string passwd, std::string sqlname);
  // 关闭HTTP连接
  void close_conn(bool real_close = true);
  // http处理函数
  void process();
  // 读取浏览器发送的数据
  bool read_once();
  // 给相应报文中写入数据
  bool write();
  sockaddr_in *get_address()
  {
    return &m_address;
  }
  // 初始化数据库读取表
  void initmysql_result(connection_pool *connPool);

  int timer_flag; // 是否关闭连接
  int improv;     // 是否完成数据处理
private:
  void init();
  // 从m_read_buf读取，并处理请求报文
  HTTP_CODE process_read();
  // 向m_write_buf写入响应报文数据
  bool process_write(HTTP_CODE ret);
  // 主状态机解析报文中的请求行数据
  HTTP_CODE parse_request_line(char *text);
  // 主状态机解析报文中的请求头数据
  HTTP_CODE parse_headers(char *text);
  // 主状态机解析报文中的请求内容
  HTTP_CODE parse_content(char *text);
  // 生成响应报文
  HTTP_CODE do_request();

  // m_start_line是已经解析的字符
  // get_line用于将指针向后偏移，指向未处理的字符
  char *get_line() { return m_read_buf + m_start_line; };

  // 从状态机读取一行，分析是请求报文的哪一部分
  LINE_STATUS parse_line();

  void unmap();

  // 根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
  bool add_response(const char *format, ...);
  bool add_content(const char *content);
  bool add_status_line(int status, const char *title);
  bool add_headers(int content_length);
  bool add_content_type();
  bool add_content_length(int content_length);
  bool add_linger();
  bool add_blank_line();

public:
  static int m_epollfd;
  static int m_user_count;
  MYSQL *mysql;
  int m_state; // IO 事件类别:读为0, 写为1

private:
  int m_sockfd;
  sockaddr_in m_address;

  char m_read_buf[READ_BUFFER_SIZE]; // 存储读取的请求报文数据
  int m_read_idx;                    // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置
  int m_checked_idx;                 // m_read_buf读取的位置m_checked_idx
  int m_start_line;                  // m_read_buf中已经解析的字符个数

  char m_write_buf[WRITE_BUFFER_SIZE]; // 存储发出的响应报文数据
  int m_write_idx;                     // 指示buffer中的长度,即写指针

  // 主状态机的状态
  CHECK_STATE m_check_state;
  // 请求方法
  METHOD m_method;

  // 以下为解析请求报文中对应的6个变量
  // 存储读取文件的名称
  char m_real_file[FILENAME_LEN]; // 文件绝对路径
  // 关键信息
  char *m_url;     // 请求行中URL部分，即相对路径
  char *m_version; // HTTP版本，在这个项目中期望为“HTTP/1.1”
  // 关键信息
  char *m_host;         // Host 请求头的值（即客户端请求的域名或 IP）,HTTP/1.1 要求：每个请求必须包含 Host 头（用于虚拟主机)
  int m_content_length; // Content-Length 请求头的值，表示请求体（body）的字节数
  // 关键信息
  bool m_linger; // 标记客户端是否希望保持长连接（keep-alive）

  // 以下为文件读取相关变量
  char *m_file_address; // 读取服务器上的文件地址
  struct stat m_file_stat;
  struct iovec m_iv[2]; // io向量机制iovec
  int m_iv_count;

  // 关键信息
  int cgi; // Common Gateway Interface（通用网关接口),用来标识是否为POST请求，因为POST请求要用CGI程序来处理
  // 关键信息
  char *m_string;      // 存储请求头数据，即请求体content的内容
  int bytes_to_send;   // 剩余发送字节数
  int bytes_have_send; // 已发送字节数
  char *doc_root;

  std::map<std::string, std::string> m_users; // 用户名密码匹配表
  int m_TRIGMode;                             // 触发模式
  int m_close_log;                            // 是否开启日志

  char sql_user[100];
  char sql_passwd[100];
  char sql_name[100];
};

#endif