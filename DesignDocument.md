# TinyWebServer 设计文档（面试版）

> 本文档从架构设计、模块职责、设计模式、工作逻辑及面试要点等维度，对项目进行全面梳理，便于面试中系统性地阐述。

---

## 一、项目概述

**TinyWebServer** 是一个基于 **C++11/20** 开发的轻量级高并发 Web 服务器，采用 **epoll** 实现 I/O 多路复用，支持 **Reactor** 与 **Proactor** 两种并发模型，具备 **线程池、数据库连接池、同步/异步日志、定时器** 等完整组件。服务器可处理静态资源请求及基于 MySQL 的用户注册/登录等 CGI 业务。

### 核心技术栈
- **网络 I/O**：epoll（LT/ET）+ 非阻塞 socket + `EPOLLONESHOT`
- **并发模型**：Reactor / Proactor + 线程池（半同步半反应堆）
- **数据库**：MySQL + 连接池（单例 + RAII）
- **日志系统**：单例模式 + 阻塞队列，支持同步/异步双模式
- **定时器**：升序双向链表，管理非活动连接超时关闭
- **文件传输**：`mmap` 内存映射 + `writev` 分散写（iovec）

---

## 二、系统架构图

```
┌─────────────────────────────────────────────────────────────┐
│                        客户端 (Browser)                       │
└──────────────────────┬──────────────────────────────────────┘
                       │ TCP
                       ▼
┌─────────────────────────────────────────────────────────────┐
│  主线程 (Main Thread)                                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  Config 解析 │  │  epoll_wait │  │  信号处理(SIGALRM)   │  │
│  └─────────────┘  └──────┬──────┘  └─────────────────────┘  │
│                          │                                   │
│  ┌───────────────────────┼───────────────────────────────┐  │
│  │  监听 fd (listenfd)   │   连接 fd (connfd)            │  │
│  │  · accept 新连接      │   · 可读/可写/异常/断开事件    │  │
│  │  · 创建 http_conn     │   · 交付线程池 / 直接处理      │  │
│  │  · 挂载定时器         │   · 调整/删除定时器           │  │
│  └───────────────────────┴───────────────────────────────┘  │
└──────────────────────────┬──────────────────────────────────┘
                           │ 任务队列 (threadpool)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  工作线程池 (Worker Threads)                                 │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐        ┌─────────┐    │
│  │ Thread 1│ │ Thread 2│ │ Thread 3│ ...... │ Thread N│    │
│  │ · read  │ │ · read  │ │ · read  │        │ · read  │    │
│  │ · process│ │ · process│ │ · process│        │ · process│    │
│  │ · write │ │ · write │ │ · write │        │ · write │    │
│  └────┬────┘ └────┬────┘ └────┬────┘        └────┬────┘    │
│       └─────────────┴─────────────┘                  │        │
│              竞争条件由 mutex + condition_variable 保护        │
└─────────────────────────────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  支撑子系统                                                   │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────┐ │
│  │ 日志系统    │  │ 数据库连接池 │  │ 定时器链表  │  │ 内存映射│ │
│  │ Log(单例)  │  │ connection_pool│  │ sort_timer_lst│  │ mmap   │ │
│  └────────────┘  └────────────┘  └────────────┘  └────────┘ │
└─────────────────────────────────────────────────────────────┘
```

---

## 三、模块详细设计

### 3.1 配置模块 (`config`)

| 项 | 说明 |
|---|---|
| **文件** | `config.h` / `config.cpp` |
| **职责** | 解析命令行参数，初始化服务器运行配置（端口、触发模式、线程数、连接池大小、日志模式、并发模型等）。 |
| **设计模式** | 无特殊设计模式，封装为独立配置类，职责单一（**SRP 单一职责原则**）。 |
| **工作逻辑** | 构造函数设定默认值，如 `PORT=9006`、`TRIGMode=0`（LT+LT）、`actor_model=0`（Proactor）；`parse_arg()` 通过 `getopt` 解析命令行并覆盖默认值。 |

---

### 3.2 WebServer 核心 (`webserver`)

| 项 | 说明 |
|---|---|
| **文件** | `webserver.h` / `webserver.cpp` |
| **职责** | 服务器的“大脑”，负责 socket 监听、epoll 事件循环、连接生命周期管理、定时器调度、任务分发。 |
| **设计模式** | **外观模式（Facade）**：将日志、数据库、线程池、网络 I/O、定时器等子系统的初始化与运转统一封装在 `WebServer` 类中，对外提供简洁的 `init() → log_write() → sql_pool() → thread_pool() → eventListen() → eventLoop()` 启动流程。 |
| **关键数据结构** | `user_data` 结构体：将 `fd`、`http_conn` 对象、`client_data` 定时器封装绑定，便于通过 fd 索引。 |

#### 工作逻辑（事件循环 `eventLoop`）
1. **阻塞等待**：`epoll_wait` 等待就绪事件。
2. **事件分发**：
   - `listenfd` 可读 → `dealclinetdata()`：LT 模式 `accept` 一次，ET 模式循环 `accept` 至 `EAGAIN`；为每个新连接创建 `http_conn` 并挂载定时器。
   - `pipefd[0]` 可读 → `dealwithsignal()`：通过 `socketpair` 接收 `SIGALRM`/`SIGTERM` 信号，设置 `timeout` 或 `stop_server` 标志。
   - `connfd` 可读 → `dealwithread()`：Reactor 模式直接入线程池队列；Proactor 模式先 `read_once()` 再入队。
   - `connfd` 可写 → `dealwithwrite()`：同理，Reactor 入队写，Proactor 直接 `write()`。
   - 异常事件（`RDHUP/HUP/ERR`）→ 调用 `deal_timer()` 关闭连接。
3. **定时器扫描**：每次循环末尾，若 `timeout=true`，调用 `utils.timer_handler()` → `tick()` 清理超时连接，并重新 `alarm(TIMESLOT)`。

---

### 3.3 HTTP 连接模块 (`http`)

| 项 | 说明 |
|---|---|
| **文件** | `http_conn.h` / `htttp_conn.cpp` |
| **职责** | 封装单个 TCP 连接上的 HTTP 协议处理：请求解析、业务逻辑（CGI/静态资源）、响应组装、数据收发。 |
| **设计模式** | **状态机模式（State Machine）**：使用“主状态机 + 从状态机”两级结构完成 HTTP 报文解析。 |

#### 状态机设计
- **主状态机（`CHECK_STATE`）**：
  - `CHECK_STATE_REQUESTLINE` → 解析请求行（方法、URL、版本）
  - `CHECK_STATE_HEADER` → 解析请求头（Host、Content-Length、Connection）
  - `CHECK_STATE_CONTENT` → 解析消息体（仅 POST）
- **从状态机（`LINE_STATUS`）**：
  - `parse_line()` 按 `\r\n` 切分行，返回 `LINE_OK` / `LINE_BAD` / `LINE_OPEN`。
- **驱动逻辑**：`process_read()` 在 `while` 循环中，先从状态机取一行，再根据主状态机状态调用对应解析函数，直到报文完整或出错。

#### CGI 与静态资源处理 (`do_request`)
- **POST 请求（CGI）**：解析 `user=xxx&passwd=xxx`，通过 `m_url` 末尾标志位判断业务：
  - `/2`：登录校验（查内存 `map`）
  - `/3`：注册插入（先查重，再 `mysql_query` 插入）
- **静态资源**：将 `doc_root` 与 `m_url` 拼接成绝对路径，使用 `stat()` 检查文件存在性与权限，再通过 `mmap()` 映射到内存，配合 `writev()` 发送 HTTP 头部 + 文件内容，实现**零拷贝（近似）**发送。

#### 关键技术点
- **iovec 分散写**：`m_iv[0]` 指向 HTTP 响应头缓冲区，`m_iv[1]` 指向 `mmap` 后的文件地址，`writev` 一次系统调用发送两部分数据，避免用户态拷贝。
- **EPOLLONESHOT**：每个 `connfd` 注册时开启 `EPOLLONESHOT`，确保同一 socket 上同一事件在任何时刻只被一个线程处理，防止多线程竞态。事件处理完毕后通过 `modfd()` 重新注册。

---

### 3.4 线程池模块 (`threadpool)

| 项 | 说明 |
|---|---|
| **文件** | `threadpool.h`（模板类，头文件即实现） |
| **职责** | 维护固定数量的工作线程及任务队列，将主线程接收到的 I/O 事件分发给工作线程处理。 |
| **设计模式** | **生产者-消费者模式**：主线程是生产者，将任务 `append` 到队列；工作线程是消费者，从队列 `pop` 并执行。 |

#### 工作逻辑
- **初始化**：构造函数创建 `thread_number` 个 `std::thread`，每个线程执行 `worker()` 死循环。
- **任务入队**：
  - `append(request, state)`：Reactor 模式使用，需区分读/写事件（`state=0/1`）。
  - `append_p(request)`：Proactor 模式使用，I/O 已由主线程完成，工作线程只负责 `process()`。
- **线程同步**：`std::mutex` 保护任务队列，`std::condition_variable` 实现线程阻塞与唤醒。
- **工作线程执行流（Reactor）**：
  1. 读事件 → `request->read_once()` → `connectionRAII` 取数据库连接 → `request->process()`
  2. 写事件 → `request->write()`
  3. 设置 `improv=1`（处理完成），若失败则设置 `timer_flag=1`（需关闭连接）。

---

### 3.5 定时器模块 (`timer)

| 项 | 说明 |
|---|---|
| **文件** | `timer.h` / `timer.cpp` |
| **职责** | 管理非活动连接，超时后自动关闭 socket，释放资源，防止僵尸连接占用 fd。 |
| **设计模式** | **职责链模式（简化版）** / **回调模式**：通过升序双向链表组织定时器，每个节点持有回调函数 `cb_func`，超时后链式触发。 |

#### 数据结构
- **`util_timer`**：双向链表节点，包含 `expire`（超时时间）、`cb_func`（回调函数指针）、`user_data`（指向 `client_data`）。
- **`sort_timer_lst`**：升序双向链表，核心操作 `add_timer`、`adjust_timer`、`del_timer`、`tick`。
- **`Utils`**：工具类，封装 epoll 操作、信号设置、定时器扫描入口 `timer_handler()`。

#### 工作逻辑
1. **定时器创建**：新连接建立时，`WebServer::timer()` 为其分配 `util_timer`，超时时间设为 `cur + 3 * TIMESLOT`（默认 15s），插入链表。
2. **定时器调整**：连接上有读写事件发生时，`adjust_timer()` 更新 `expire` 并调整节点在链表中的位置（保持升序）。
3. **超时处理**：
   - `SIGALRM` 信号每隔 `TIMESLOT`（5s）触发一次。
   - 信号处理函数 `sig_handler` 将信号值写入 `socketpair` 管道写端，主线程在 epoll 中感知读端可读。
   - 主线程调用 `timer_handler()` → `tick()`，从链表头部遍历，删除所有 `expire <= cur` 的节点，并执行回调 `cb_func`：从 epoll 删除事件、关闭 fd、减少连接计数。

> **面试亮点**：采用 `socketpair + epoll` 将异步信号转为同步事件处理，避免了信号处理函数中直接操作共享资源带来的线程安全问题（**统一事件源**思想）。

---

### 3.6 日志模块 (`log)

| 项 | 说明 |
|---|---|
| **文件** | `log.h` / `log.cpp` / `block_queue.h` |
| **职责** | 记录服务器运行日志，支持分级别（DEBUG/INFO/WARN/ERROR）、按天/按行数自动切分文件。 |
| **设计模式** | **单例模式（Meyer's 懒汉式）**：`static Log instance;` C++11 保证线程安全；**生产者-消费者模式**：异步日志通过阻塞队列解耦日志生成与磁盘写入。 |

#### 同步 vs 异步
- **同步**：`write_log()` 直接 `fputs` 写入文件，简单但会阻塞业务线程。
- **异步**：`init()` 时若 `max_queue_size > 0`，则创建 `block_queue` 并启动独立写日志线程 `flush_log_thread`。业务线程只负责 `push` 日志字符串到队列；写线程循环 `pop` 并写入文件。

#### `block_queue` 设计
- 基于**循环数组**实现的有界阻塞队列。
- `push` 时若队列满则丢弃（或唤醒）/`pop` 时若队列空则阻塞等待 `condition_variable`。
- 使用 `std::mutex` 保证线程安全。

#### 日志切分策略
- **按天切分**：`m_today != 当前日期` 时，关闭旧文件，以新日期命名创建新文件。
- **按行数切分**：同一日内，当日志行数 `m_count % m_split_lines == 0` 时，创建带序号后缀的新文件（如 `ServerLog.1`）。

---

### 3.7 数据库连接池 (`CGImysql)

| 项 | 说明 |
|---|---|
| **文件** | `sql_connection_pool.h` / `sql_connection_pool.cpp` |
| **职责** | 预先创建并维护一组 MySQL 连接，避免频繁创建/销毁连接的开销，提高数据库访问效率。 |
| **设计模式** | **单例模式（Meyer's 懒汉式）** + **对象池模式（Object Pool）** + **RAII 模式**。 |

#### 工作逻辑
- **单例获取**：`connection_pool::GetInstance()` 返回唯一实例。
- **初始化**：`init()` 预先创建 `MaxConn` 个 MySQL 连接（`mysql_real_connect`），存入 `std::list<MYSQL*>`。
- **获取连接**：`GetConnection()` 从链表头部取出一个连接，若链表为空则通过 `condition_variable` 阻塞等待。
- **释放连接**：`ReleaseConnection()` 将连接归还链表尾部，并通知等待线程。
- **RAII 封装**：`connectionRAII` 类在构造时取连接，析构时自动归还，防止遗漏释放，简化调用方代码。

> **面试亮点**：在 `http_conn::initmysql_result()` 中，服务器启动时预先将 `user` 表数据加载到内存 `std::map`，后续登录/注册直接查内存，避免每次请求都查询数据库，大幅降低数据库压力。

---

## 四、并发模型详解（Reactor vs Proactor）

| 维度 | Reactor（反应堆） | Proactor（前摄器） |
|---|---|---|
| **定义** | 主线程只负责监听和分发 I/O 事件，数据读写由工作线程完成。 | 主线程负责完成数据的读写，工作线程只处理业务逻辑。 |
| **代码体现** | `dealwithread()` 中直接 `m_pool->append(users+sockfd, 0)`，工作线程调用 `read_once()`。 | `dealwithread()` 中先 `users[sockfd].read_once()`，成功后再 `m_pool->append_p(users+sockfd)`。 |
| **优点** | 更符合传统 Linux 网络编程思维；读写逻辑与业务解耦清晰。 | 工作线程纯计算，不阻塞在 I/O 上，任务处理更统一。 |
| **缺点** | 工作线程可能阻塞在 `read/write`。 | 主线程承担了 I/O 负担，在高并发极端场景下可能成为瓶颈。 |
| **设计模式** | **Reactor 模式**（事件驱动 + 事件分发器 + 事件处理器） | 近似 **Proactor 模式**（异步 I/O 完成通知，这里用主线程同步读写模拟） |

> 项目通过 `m_actormodel` 配置位（0=Proactor, 1=Reactor）在编译/运行期灵活切换。

---

## 五、关键技术总结（面试话术）

### 5.1 为什么用 epoll？
- **无 fd 数量限制**：仅受系统内存限制（对比 select 的 1024）。
- **事件驱动**：只返回就绪 fd，时间复杂度 `O(1)`（对比 poll 的 `O(n)` 遍历）。
- **支持 ET/LT**：ET 模式减少 epoll 唤醒次数，配合非阻塞 IO 可实现高吞吐。

### 5.2 为什么用线程池而非一个连接一个线程？
- 线程创建/销毁开销大，且线程数过多会导致上下文切换和内存耗尽。
- 线程池通过**固定线程数 + 任务队列**实现资源复用与流量削峰，属于 **“半同步/半反应堆”** 模型。

### 5.3 为什么要 `EPOLLONESHOT`？
- 多线程环境下，若一个 fd 的读事件就绪，多个线程可能同时被唤醒处理同一事件，造成竞态。
- `EPOLLONESHOT` 保证每次只通知一次，处理完毕后通过 `modfd()` 重新注册，确保线程安全。

### 5.4 定时器为什么选择升序链表？
- 连接数不大时，链表实现简单，插入 `O(n)`、删除 `O(1)`、扫描超时 `O(k)`（`k` 为超时节点数）。
- 若连接数极高，可升级为 **时间轮** 或 **最小堆**（如 libevent 使用最小堆），将插入降为 `O(logN)`。

### 5.5 为什么用 `mmap + writev` 而不是 `read + write`？
- `mmap` 将磁盘文件直接映射到进程虚拟地址空间，避免从内核态到用户态的额外拷贝（减少一次 `read` 拷贝）。
- `writev` 将 HTTP 头部缓冲区和 `mmap` 后的文件内容通过一次系统调用发送，进一步减少系统调用次数，提升静态资源传输效率。

---

## 六、请求处理完整流程

以 **Proactor + ET + 异步日志** 为例：

1. **启动阶段**：`main()` 解析配置 → `WebServer` 初始化 → 启动异步日志线程、MySQL 连接池、工作线程池、epoll 监听。
2. **连接建立**：客户端发起 TCP 连接，`epoll_wait` 返回 `listenfd` 可读，主线程循环 `accept4` 获取 `connfd`，创建 `http_conn`，注册 `EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP`，并挂载 15s 定时器。
3. **请求读取**：客户端发送 HTTP 请求，`epoll_wait` 返回 `connfd` 可读。
   - 主线程调用 `read_once()`，在 ET 模式下循环 `recv` 直至 `EAGAIN`，将完整请求读入 `m_read_buf`。
   - 主线程将 `http_conn` 对象通过 `append_p()` 加入线程池任务队列。
4. **业务处理**：工作线程取出任务，通过 `connectionRAII` 获取数据库连接，调用 `process()`。
   - `process_read()` 解析请求行、请求头、消息体。
   - `do_request()` 判断是静态资源还是 CGI（登录/注册），更新 `m_url`。
   - `process_write()` 组装响应头，设置 `m_iv` 指向响应头和 `mmap` 文件地址。
   - `modfd()` 重新注册 `EPOLLOUT` 事件。
5. **响应发送**：`epoll_wait` 返回 `connfd` 可写，主线程调用 `write()`，使用 `writev` 发送响应头和文件内容。
   - 若发送完毕且 `Connection: keep-alive`，则 `init()` 重置连接状态，重新注册 `EPOLLIN`。
   - 否则关闭连接，删除定时器。
6. **超时处理**：`SIGALRM` 每 5s 触发一次，主线程在事件循环末尾调用 `tick()`，关闭超时未活动的连接。

---

## 七、目录结构

```
TinyWebServer/
├── src/
│   ├── main.cpp                  # 入口
│   ├── config/                   # 配置解析
│   ├── webserver/                # 服务器核心 + epoll 事件循环
│   ├── http/                     # HTTP 协议解析 + CGI
│   ├── threadpool/               # 线程池（生产者-消费者）
│   ├── timer/                    # 升序链表定时器 + 信号处理
│   ├── log/                      # 单例日志 + 阻塞队列
│   └── CGImysql/                 # 数据库连接池 + RAII
├── root/                         # 静态资源（html/css/js/图片）
├── build/                        # CMake 构建产物
├── CMakeLists.txt
└── DesignDocument.md             # 本文档
```

---

## 八、面试速记卡片

| 问题 | 一句话回答 |
|---|---|
| 项目并发模型是什么？ | Reactor / Proactor 双模式 + epoll + 线程池。 |
| HTTP 怎么解析的？ | 主状态机 + 从状态机（有限状态机）逐行解析。 |
| 定时器怎么实现的？ | 升序双向链表 + SIGALRM + socketpair 统一事件源。 |
| 日志是同步还是异步？ | 通过阻塞队列支持同步/异步双模式，异步时单独写日志线程。 |
| 数据库连接怎么管理？ | 单例连接池 + 预创建连接 + RAII 自动归还。 |
| 静态资源怎么加速发送？ | `mmap` 内存映射 + `writev` 分散写，减少数据拷贝和系统调用。 |
| 多线程下如何避免同一 fd 被多个线程处理？ | `EPOLLONESHOT`，处理完通过 `modfd` 重新注册。 |
| 登录注册查询为什么快？ | 启动时将 `user` 表缓存到内存 `std::map`，查内存而非查库。 |

---

*文档生成时间：2026-04-14*  
*版本：基于当前项目源码整理*
