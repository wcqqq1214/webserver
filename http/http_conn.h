#ifndef HTTP_CONNECTION_H
#define HTTP_CONNECTION_H

#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn {
public:
    /** @brief 文件名最大长度 */
    static const int FILENAME_LEN = 200;

    /** @brief 读缓冲区大小 */
    static const int READ_BUFFER_SIZE = 2048;

    /** @brief 写缓冲区大小 */
    static const int WRITE_BUFFER_SIZE = 1024;
    
    /** @brief HTTP 请求方法类 */ 
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATCH
    };

    /** @brief 解析请求的状态 */ 
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    /** @brief HTTP 请求处理结果的状态码 */ 
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION   
    };

    /** @brief 行解析状态，表示当前行是否完整 */
    enum LINE_STATUS {
        LINE_OK = 0,        // 当前行完整
        LINE_BAD,           // 当前行有错误或格式不正确
        LINE_OPEN           // 当前行尚未完全读取
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    /** @brief 初始化连接对象（带多个参数的版本）*/
    void init(int sockfd, const sockaddr_in& addr, char* root, int TRIGMode, int close_log, string user, string passwd, string sqlname);
    
    /** @brief 关闭客户端连接 */
    void close_conn(bool real_close = true);
    
    /** @brief 处理客户端请求，调用读写操作 */
    void process();

    /** @brief 从客户端 socket 一次性读取数据到读缓冲区 */
    bool read_once();

    /** @brief 响应客户请求，将缓冲区中的数据写入 socket */
    bool write();

    /**
     * @brief 获取客户端的地址信息
     * 
     * @return 返回客户端的 sockaddr_in 地址结构指针
     */
    sockaddr_in* get_address() { return &m_address; }

    /** @brief 初始化 MySQL 用户验证结果，加载用户账户信息到内存 */
    void initmysql_result(connection_pool* connPool);
    
    /** @brief 用于定时器相关控制 */
    int timer_flag;         // 定时器标志位，用于标识连接状态或定时器行为（如超时、关闭）
    int improv;             // 是否对定时器行为进行干预的标记，用于同步定时器与工作线程

private:
    /** @brief 内部初始化函数，重置连接对象的所有状态和变量 */
    void init();

    /** @brief 处理客户端发送的 HTTP 请求数据，解析请求行、请求头和请求体 */
    HTTP_CODE process_read();

    /** @brief 根据解析结果构造 HTTP 响应报文，并写入写缓冲区 */
    bool process_write(HTTP_CODE ret);

    /** @brief 解析 HTTP 请求行 */
    HTTP_CODE parse_request_line(char* text);

    /** @brief 解析 HTTP 请求头字段 */
    HTTP_CODE parse_headers(char* text);

    /** @brief 解析 HTTP 请求体内容 */
    HTTP_CODE parse_content(char* text);

    /** @brief 执行 HTTP 请求的实际处理逻辑 */
    HTTP_CODE do_request();

    /**
     * @brief 获取当前正在解析行的起始位置指针
     * 
     * @return char* 指向当前行起始位置的指针
     */
    char* get_line() { return m_read_buf + m_start_line; }
    
    /** @brief 解析一行 HTTP 请求数据，判断该行是否完整 */
    LINE_STATUS parse_line();

    /** @brief 取消内存映射，释放由 mmap 映射的文件资源 */
    void unmap();

    /** @brief 向写缓冲区添加格式化响应数据 */
    bool add_response(const char* format, ...);

    /** @brief 添加正文内容到响应中 */
    bool add_content(const char* content);

    /** @brief 添加响应状态行 */
    bool add_status_line(int status, const char* title);

    /** @brief 添加 HTTP 响应头 */
    bool add_headers(int content_length);

    /** @brief 添加 Content-Type 到响应头 */
    bool add_content_type();

    /** @brief 添加 Content-Length 字段到响应头 */
    bool add_content_length(int content_length);

    /** @brief 添加 Connection 字段到响应头 */
    bool add_linger();

    /** @brief 添加空行分隔符 */
    bool add_blank_line();

public:
    /** @brief epoll 文件描述符，用于 I/O 多路复用 */
    static int m_epollfd;

    /** @brief 当前用户连接数 */
    static int m_user_count;
    
    /** @brief MySQL 连接句柄 */
    MYSQL* mysql;
    
    /** @brief 当前连接状态，读为 0，写为 1 */
    int m_state;

private:

    /** @brief 客户端 socket 文件描述符 */
    int m_sockfd;

    /** @brief 客户端地址结构体 */
    sockaddr_in m_address;
    
    /** @brief 读缓冲区 */
    char m_read_buf[READ_BUFFER_SIZE];
    
    /** @brief 已读取字节数 */
    long m_read_idx;
    
    /** @brief 已检查字节数 */
    long m_checked_idx;
    
    /** @brief 当前行起始位置 */
    int m_start_line;
    
    /** @brief 写缓冲区 */
    char m_write_buf[WRITE_BUFFER_SIZE];
    
    /** @brief 已写入字节数 (当前写入位置) */
    int m_write_idx;


    /** @brief 当前解析状态 */
    CHECK_STATE m_check_state;
    
    /** @brief 请求方法 */
    METHOD m_method;
    
    /** @brief 请求的文件路径 */
    char m_real_file[FILENAME_LEN];
    
    /** @brief 请求 URL */
    char* m_url;
    
    /** @brief HTTP 版本号 */
    char* m_version;
    
    /** @brief 请求主机 */
    char* m_host;
    
    /** @brief 请求体长度 */
    long m_content_length;
    
    /** @brief 是否保持连接 */
    bool m_linger;
    
    /** @brief 映射文件地址 */
    char* m_file_address;
    
    /** @brief 文件状态信息 */
    struct stat m_file_stat;
    
    /** @brief 用于 writev 的结构数组 */
    struct iovec m_iv[2];
    
    /** @brief writev 使用的 iovec 数量 */
    int m_iv_count;
    
    /** @brief 是否启用 CGI 模式 */
    int cgi;
    
    /** @brief 存储 POST 请求数据 */
    char* m_string;
    
    /** @brief 待发送字节数 */
    int bytes_to_send;
    
    /** @brief 已发送字节数 */
    int bytes_have_send;
    
    /** @brief 网站根目录路径 */
    char* doc_root;

    /** @brief 用户账户信息映射（用户名和密码） */
    map<string, string> m_users;
    
    /** @brief 触发模式（边沿触发 ET / 水平触发 LT） */
    int m_TRIGMode;
    
    /** @brief 是否关闭日志记录 */
    int m_close_log;

    
    /** @brief 数据库用户名 */
    char sql_user[100];
    
    /** @brief 数据库密码 */
    char sql_passwd[100];
    
    /** @brief 数据库名称 */
    char sql_name[100];
};

#endif // !HTTP_CONNECTION_H