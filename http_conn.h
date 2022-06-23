#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <error.h>
#include "locker.h"
#include <sys/uio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <map>
#include <unordered_map>

class util_timer;   // 前置声明

class http_conn{
public:
    static int m_epollfd;   //
    static int m_user_count;//用户数量
    static const int READ_BUFFER_SIZE=2048; //读缓冲区大小
    static const int WRITE_BUFFER_SIZE=1024;//写缓冲区大小
    static const int FILENAME_LEN=200;//文件名的最大长度

    //http请求方法，目前支持GET
    enum METHOD {GET=0, POST, HEAD, PUT , DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
    主状态机：解析客户端请求时，主状态机的状态
    CHECK_STATE_REQUESTLINE：分析请求行
    CHECK_STATE_HEADER：分析请求头
    CHECK_STATE_CONTENT：分析请求体
    */
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE=0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    /*
    从状态机：解析某行时可能出现的状态
    1.读到完整的行
    2.行出错
    3.行数据不完整
    */
    enum LINE_STATUS {LINE_OK=0, LINE_BAD, LINE_OPEN};
    
    /*
    服务器处理http请求可能出现的结果
    NO_REQUEST:请求不完整，需要继续读取
    GET_REQUEST：获得完整的客户请求
    BAD_REQUEST：客户请求语法错误
    NO_RESOURCE：服务器没对应资源
    FORBINDDEN_REQUEST：客户对资源的访问权限不足
    FILE_REQUEST：文件请求，获取文件成功
    INTERNAL_ERROR：服务器内部错误
    CLOSED_CONNECTION：客户端已经关闭连接
    */
   enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBINDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};
public:
    util_timer* m_timer;
public:
    http_conn(){}
    ~http_conn(){}

    void process();//处理客户端请求
    void init(int sockfd, const sockaddr_in& addr);//初始化新接收的连接
    void close_conn();//关闭连接
    bool read();    //非阻塞读写，一次性读完数据
    bool write();   //一次性写完
private:
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];//读缓冲区
    int m_read_index;   //标识下一个需要读的位置

    int m_checked_index;//当前分析的字符在读缓冲区的位置
    int m_start_line; //当前正在解析行的起始位置

    char m_real_file[FILENAME_LEN];//目标文件的实际路径
    char *m_url;//请求目标文件的文件名
    char* m_version;//协议版本(仅仅支持http1.1)
    METHOD m_method;//请求方法
    char* m_host;//主机名
    bool m_linger;//判断是否持续连接
    int m_content_length;//消息体长度
    
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;//写缓存中待发送的字节数
    char* m_file_address;//客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;//目标文件的状态
    struct iovec m_iv[2];
    int m_iv_count;
    int bytes_to_send;//还需要发送的字节数
    int bytes_have_send;//已经发送的字节数

    CHECK_STATE m_checked_state;//主状态机的状态
private:
    HTTP_CODE process_read();//解析http请求，拆分为三部分
    HTTP_CODE parse_request_line(char* text); // 解析请求首部行
    HTTP_CODE parse_headers(char* text); //解析请求头
    HTTP_CODE parse_content(char* text); //解析请求体
    LINE_STATUS parse_line();//解析一行，是上面解析行为的具体细化
    
    bool process_write(HTTP_CODE ret);//
    char* get_line(){//获取一行数据
        return m_read_buf+m_start_line;
    }

    HTTP_CODE do_request();
    void init();//初始化连接其余的信息，一些状态机相关的信息
    void unmap();//释放映射
private:
    std::string get_file_type(std::string);
    bool add_status_line(int status, const char* title);
    bool add_response(const char* format,...);
    bool add_headers(int content_len);
    bool add_content_length(int content_len);
    bool add_content_type(std::string type);
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char* content);
};

#endif