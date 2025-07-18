#include "http_conn.h"

#include <mysql/mysql.h>

#include <fstream>

// 定义 http 响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

/** @brief 对文件描述符设置非阻塞 */
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/**
 * @brief 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
 */
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    // 若 TRIGMode == 1 启用 ET 模式，否则使用默认的 LT 模式
    if (1 == TRIGMode) {
        // EPOLLIN：    监听读事件
        // EPOLLET：    使用 ET 模式，仅当状态变化时触发通知
        // EPOLLRDHUP： 监听对端关闭连接事件
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    // 启用一次性监听（防止多个线程同时处理同一连接）
    // 如果要重新启用监听，使用 epoll_ctl(..., EPOLL_CTL_MOD, ...) 手动重新注册事件
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

/**
 * @brief 从内核时间表删除描述符
 */
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

/**
 * @brief 将事件重置为 EPOLLONESHOT
 */
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode) {
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    } else {
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/**
 * @brief 初始化连接对象（带多个参数的版本）
 *
 * @param sockfd        客户端套接字描述符
 * @param addr          客户端地址结构体
 * @param root          网站根目录路径
 * @param TRIGMode      触发模式（0 表示 LT 模式，1 表示 ET 模式）
 * @param close_log     是否关闭日志功能（0 启用日志，1 关闭日志）
 * @param user          数据库用户名
 * @param passwd        数据库密码
 * @param sqlname       数据库名称
 */
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root, int TRIGMode, int close_log, string user,
                     string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是 网站根目录出错 或 http响应格式出错 或者 访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

/**
 * @brief 关闭客户端连接
 *
 * @param real_close 是否实际关闭连接（true 表示关闭 socket，false 表示仅标记关闭）
 */
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/**
 * @brief 处理客户端请求，调用读写操作
 */
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    // 如果请求未完整读取，重新监听读事件并返回
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // 修改 epoll 监听事件为写事件，继续等待下一次写操作
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

/**
 * @brief 从客户端 socket 一次性读取数据到读缓冲区
 *
 * 循环读取客户数据，直到无数据可读或对方关闭连接
 * 非阻塞 ET 工作模式下，需要一次性将数据读完
 *
 * @return 成功读取返回 true，读取失败或连接关闭返回 false
 */
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    // LT 读取数据
    if (0 == m_TRIGMode) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0) {
            return false;
        }

        return true;
    } else {  // ET 读取数据
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                return false;
            } else if (bytes_read == 0) {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

/**
 * @brief 响应客户请求，将缓冲区中的数据写入 socket
 *
 * @return 成功写入返回 true，写入失败或连接关闭返回 false
 */
bool http_conn::write() {
    int temp = 0;

    if (bytes_to_send == 0) {
        // 将 epoll 的监听事件从 可写 改回 可读
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        // 重置连接状态，准备接收下一个请求
        init();
        return true;
    }

    while (true) {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            // 如果 TCP 写缓冲区满了
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();  // 释放内存映射
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        // 第一个 iovec 指向的缓冲区（通常是 HTTP 响应头）已经全部发送完毕
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

/**
 * @brief 初始化 MySQL 用户验证结果，加载用户账户信息到内存
 *
 * @param connPool 数据库连接池对象指针
 */
void http_conn::initmysql_result(connection_pool* connPool) {
    // 先从连接池中取一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlconn(&mysql, connPool);

    // 在 user 表中检索 username，passwd 数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username, passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入 map 中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/**
 * @brief 内部初始化函数，重置连接对象的所有状态和变量
 *
 * 初始化新接受的连接
 * check_state 默认为分析请求行状态
 */
void http_conn::init() {
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/**
 * @brief 处理客户端发送的 HTTP 请求数据，解析请求行、请求头和请求体
 *
 * @return HTTP_CODE 表示解析结果状态码，指示是否成功获取完整请求
 */
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

/**
 * @brief 根据解析结果构造 HTTP 响应报文，并写入写缓冲区
 *
 * @param ret 由 process_read 返回的 HTTP 请求处理结果状态码
 * @return 成功构造响应并准备发送返回 true，否则返回 false
 */
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR: {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST: {
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: {
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST: {
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
        }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

/**
 * @brief 解析 HTTP 请求行，获得请求方法，目标 url 及 http 版本号
 *
 * @param text 指向当前解析行的起始位置
 * @return HTTP_CODE 返回解析状态码，指示是否成功解析请求行
 */
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // strpbrk : 在一个字符串中查找第一个 匹配另一个字符串中任意字符的 字符位置
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    // strspn : 计算字符串开头连续包含指定字符集合的字符数量 (在这里为了跳过前导空格)
    m_url += strspn(m_url, " \t");

    char* method = text;
    // strcasecmp : 大小写不敏感 的字符串比较函数
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else {
        return BAD_REQUEST;
    }

    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // strncasecmp : 在指定长度内，忽略大小写地比较两个字符串
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        // strchr : 在字符串中查找某个字符第一次出现的位置
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 当 url 为 / 时，显示判断界面
    if (strlen(m_url) == 1) {
        // strcat : 将一个字符串拼接到另一个字符串的末尾
        strcat(m_url, "judge.html");
    }

    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

/**
 * @brief 解析 HTTP 请求头字段
 *
 * @param text 指向当前解析行的起始位置
 * @return HTTP_CODE 返回解析状态码，指示是否成功解析请求头
 */
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        LOG_INFO("Unknown header: %s", text);
    }
    return NO_REQUEST;
}

/**
 * @brief 解析 HTTP 请求体内容
 *
 * @param text 指向当前解析行的起始位置
 * @return HTTP_CODE 返回解析状态码，指示是否成功解析完整请求体
 */
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_checked_idx + m_content_length)) {
        text[m_content_length] = '\0';
        // POST 请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/**
 * @brief 执行 HTTP 请求的实际处理逻辑
 *
 * @return HTTP_CODE 返回请求处理结果状态码
 */
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // strrchr : 从后往前找第一个 '/'
    const char* p = strrchr(m_url, '/');

    // LOG_INFO("doc_root: %s", doc_root);
    // LOG_INFO("m_url: %s", m_url);

    // 处理 cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i) {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        if (*(p + 1) == '3') {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res) {
                    strcpy(m_url, "/log.html");
                } else {
                    strcpy(m_url, "/registerError.html");
                }
            } else {
                strcpy(m_url, "/registerError.html");
            }
        } else if (*(p + 1) == '2') {
            // 如果是登录，直接判断
            // 若浏览器端输入的用户名和密码在表中可以查找到，返回 1，否则返回 0
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
            } else {
                strcpy(m_url, "/logError.html");
            }
        }
    }

    if (*(p + 1) == '0') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '1') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '5') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '6') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '7') {
        char* m_url_real = (char*)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // stat : 尝试获取 m_real_file 路径对应文件的信息，并存入 m_file_stat 结构体
    // 如果函数返回负值 (< 0)，意味着 这个文件或路径不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // m_file_stat.st_mode : 包含了文件的类型和权限信息
    // S_IROTH : 表示其他人可读的权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // S_ISDIR : 用于判断给定的路径是否为一个目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // LOG_INFO("m_real_file: %s", m_real_file);

    int fd = open(m_real_file, O_RDONLY);
    // mmap : 将文件直接映射到进程的虚拟内存空间中
    // PROT_READ : 这块内存区域是可读的
    // MAP_PRIVATE : 创建一个私有的、写时复制 (Copy-on-Write) 的映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

/**
 * @brief 解析一行 HTTP 请求数据，判断该行是否完整（从状态机）
 *
 * @return LINE_STATUS 表示当前行的解析状态（LINE_OK / LINE_BAD / LINE_OPEN）
 */
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            // '\r' 是缓冲区的最后一个字符，可能 '\n' 还没收到
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            // '\r' 的下一个字符是 '\n'，找到了完整的行
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                // 将 '\r' 和 '\n' 替换为字符串结束符 '\0'
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;

        } else if (temp == '\n') {
            // 检查前一个字符是不是 '\r'
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                // 将 '\r' 和 '\n' 替换为字符串结束符 '\0'
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }

    return LINE_OPEN;
}

/**
 * @brief 取消内存映射，释放由 mmap 映射的文件资源
 */
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = nullptr;
    }
}

/**
 * @brief 向写缓冲区添加格式化响应数据（支持类似 printf 的格式化方式）
 *
 * @param format 格式字符串
 * @return 成功添加返回 true，缓冲区满或出错返回 false
 */
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);

    // vsnprintf 的返回值是如果缓冲区足够大，本应该写入的字符总数（不包括结尾的 '\0'）
    int len = vsnprintf(m_write_idx + m_write_buf, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 检查是否发生截断
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("response:%s", m_write_buf);

    return true;
}

/**
 * @brief 添加正文内容到响应中
 *
 * @param content 要添加的正文字符串
 * @return 成功添加返回 true，缓冲区满或出错返回 false
 */
bool http_conn::add_content(const char* content) { return add_response("%s", content); }

/**
 * @brief 添加响应状态行（HTTP 协议版本 + 状态码 + 状态描述）
 *
 * @param status 状态码（如 200、404）
 * @param title 状态描述（如 OK、Not Found）
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/**
 * @brief 添加 HTTP 响应头
 *
 * @param content_length 正文长度
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_content_type() && add_linger() && add_blank_line();
}

/**
 * @brief 添加 Content-Type 到响应头，表示响应正文的 MIME 类型
 *
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_content_type() { return add_response("Content-Type:%s\r\n", "text/html"); }

/**
 * @brief 添加 Content-Length 字段到响应头
 *
 * @param content_length 正文长度
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_content_length(int content_len) { return add_response("Content-Length:%d\r\n", content_len); }

/**
 * @brief 添加 Connection 字段到响应头，控制是否保持连接（keep-alive 或 close）
 *
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_linger() { return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close"); }

/**
 * @brief 添加空行分隔符，标志 HTTP 头部结束
 *
 * @return 成功添加返回 true，失败返回 false
 */
bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }
