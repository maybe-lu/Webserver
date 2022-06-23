#include "http_conn.h" 

int http_conn::m_epollfd=-1;
int http_conn::m_user_count=0;

const char* ok_200_title="OK";
const char* error_400_title="Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
//const char* error_400_form="Your request has bad syntax or is inherently impossible";
const char* error_403_title="Forbindden";
//const char* error_403_form="You do not have permission to get file from this server.\n";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title="Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title="Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";
//const char* error_500_form="There was an unusual proble serving ther request file";

const char* doc_root="/root/web_M/resources";//资源路径

//文件类型
const std::unordered_map<std::string, std::string> SUFFIX_TYPE = {
        { ".html",  "text/html" },
        { ".xml",   "text/xml" },
        { ".xhtml", "application/xhtml+xml" },
        { ".txt",   "text/plain" },
        { ".rtf",   "application/rtf" },
        { ".pdf",   "application/pdf" },
        { ".word",  "application/nsword" },
        { ".png",   "image/png" },
        { ".gif",   "image/gif" },
        { ".jpg",   "image/jpeg" },
        { ".jpeg",  "image/jpeg" },
        { ".au",    "audio/basic" },
        { ".mpeg",  "video/mpeg" },
        { ".mpg",   "video/mpeg" },
        { ".avi",   "video/x-msvideo" },
        { ".gz",    "application/x-gzip" },
        { ".tar",   "application/x-tar" },
        { ".css",   "text/css "},
        { ".js",    "text/javascript "},
    };

//设置文件描述符为非阻塞
void setnonblocking(int fd){
    int old_flag=fcntl(fd, F_GETFL);
    int new_flag=old_flag|O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd=fd;
    //event.events = EPOLLIN | EPOLLRDHUP;//in是读，rdhup是异常处理
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;//in是读，rdhup是异常处理

    //防止一些错误事件，比如一个事件，但是被多次触动后唤醒多个线程同时处理一个socket事件
    if(one_shot){
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    //设置文件描述符非阻塞
    setnonblocking(fd);//et模式不能阻塞
}
//从epoll删除
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}
//修改文件描述符并重置oneshot事件，确保还能触发
void modfd(int epollfd, int fd, int ev){
    epoll_event event;
    event.data.fd=fd;
    event.events=ev|EPOLLONESHOT|EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//初始化
void http_conn::init(int sockfd,const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address=addr;

    //设置端口复用
    int reuse=1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;//总用户数增加+1

    init();
}
//初始化状态机
void http_conn::init(){

    bytes_to_send = 0;
    bytes_have_send = 0;

    m_checked_state=CHECK_STATE_REQUESTLINE;//初始化为解析请求行
    m_checked_index=0;
    m_start_line=0;
    m_read_index=0;
    m_write_idx=0;

    m_url=0;
    m_method=GET;
    m_version=0;
    m_host=0;
    m_linger=false;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}
//关闭连接
void http_conn::close_conn(){
    if(m_sockfd!=-1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd=-1;
        m_user_count--;//用户总数量减少-1
    }
}
//读
bool http_conn::read(){
    //循环读数据直到对方关闭或无数据可读
    if(m_read_index>=READ_BUFFER_SIZE)  return false;

    int bytes_read=0;
    while(true){
        bytes_read=recv(m_sockfd, m_read_buf+m_read_index, READ_BUFFER_SIZE-m_read_index,0);
        if(bytes_read==-1){
            if(errno==EAGAIN || errno==EWOULDBLOCK){//没有数据
                break;
            }
            return false;//出现其他错误
        }else if(bytes_read==0){//对方关闭连接
            return false;
        }
        m_read_index+=bytes_read;
    }
    return true;
}
//写
bool http_conn::write(){
    int temp=0;

    if(bytes_to_send==0){//这一次没有数据要发送
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }
    while(1){
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if(temp<=-1){
            if(errno==EAGAIN){
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_to_send-=temp;
        bytes_have_send+=temp;
        
        if( bytes_have_send>=m_iv[0].iov_len ){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);//可以把m_write_idx看成是m_iv[0]的长度
            m_iv[1].iov_len = bytes_to_send;//剩下要发送的
        }
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
//释放资源
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;

    char* text=0;
    while(  (m_checked_state == CHECK_STATE_CONTENT && line_status==LINE_OK) 
            || (line_status=parse_line())==LINE_OK ){
        //解析到了一行完整数据
        text=get_line();

        m_start_line=m_checked_index;
        //printf("got 1 http line : %s\n",text);

        switch (m_checked_state)
        {
            case CHECK_STATE_REQUESTLINE://请求行
            {
                printf("header : %s\n",text);
                ret = parse_request_line(text);
                if(ret==BAD_REQUEST)    return BAD_REQUEST;//语法错误
                break;
            }
            case CHECK_STATE_HEADER://请求头
            {
                ret=parse_headers(text);
                if(ret==BAD_REQUEST)    return BAD_REQUEST;
                else if(ret==GET_REQUEST){
                    return do_request();//解析具体请求信息
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST){
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;//内部错误
            }
        }       
    }
    return NO_REQUEST;
}
//解析http请求行——获取请求方法、URL和http版本
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    m_url=strpbrk(text, " \t");
    if (!m_url) { return BAD_REQUEST; }
    *m_url++='\0';

    char* method=text;
    if( strcasecmp(method,"GET")==0 ){
        m_method=GET;
    }else{
        return BAD_REQUEST;
    }

    m_version=strpbrk(m_url, " \t");
    if (!m_version) { return BAD_REQUEST; }
    *m_version++='\0';
    if( strcasecmp(m_version,"HTTP/1.1")!=0 ){
        return BAD_REQUEST;
    }

    if( strncasecmp(m_url,"http://",7)==0 ){
        m_url+=7;
        m_url=strchr(m_url,'/');
    }
    if(!m_url || m_url[0]!='/'){//出错
        return BAD_REQUEST;
    }

    m_checked_state=CHECK_STATE_HEADER;//主状态机改变为检查请求头状态
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_headers(char* text){
    if(text[0] == '\0'){//遇到空行表示头部解析结束
        if( m_content_length!=0 ){//判断是否还有信息体，将主状态机转入读取信息体阶段
            m_checked_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;//表示解析还没完成
        }
        return GET_REQUEST;//解析完整
    }else if( strncasecmp(text,"Connection:",11)==0 ){//解析连接
        text+=11;
        text+=strspn(text," \t");//跳过空格和\t
        if( strcasecmp(text, "keep-alive")==0 ){
            m_linger=true;//保持连接
        }
    }else if( strncasecmp(text,"Content-Length:",15)==0 ){//解析消息体头
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);//atol用来将字符串转换成长整型数
    }else if( strncasecmp(text,"Host:",5)==0 ){//解析host
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }else{//其他情况，其他字段不解析
        //printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if( m_read_index >= (m_content_length) ){
        text[m_content_length]='\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
http_conn::LINE_STATUS http_conn::parse_line(){//解析一行
    char temp;
    for(;m_checked_index<m_read_index;++m_checked_index){
        temp=m_read_buf[m_checked_index];
        if( temp=='\r' ){
            if( (m_checked_index+1)==m_read_index ){  return LINE_OPEN; }
            else if( m_read_buf[m_checked_index+1] == '\n' ){
                m_read_buf[m_checked_index++]='\0';
                m_read_buf[m_checked_index++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }else if( temp == '\n' ){
            if( (m_checked_index > 1) && (m_read_buf[m_checked_index-1] == '\r') ){
                m_read_buf[m_checked_index-1]='\0';
                m_read_buf[m_checked_index++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}
http_conn::HTTP_CODE http_conn::do_request(){
    // "/root/web_M/resources"
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    strncpy( m_real_file+len, m_url, FILENAME_LEN-len-1 );
    if( stat(m_real_file, &m_file_stat) < 0 ){//-1失败，<0表示没有这个资源
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH)){//权限判断
        return FORBINDDEN_REQUEST;
    }
    if( S_ISDIR(m_file_stat.st_mode) ){//判断是不是目录
        return BAD_REQUEST;
    }

    int fd=open(m_real_file,O_RDONLY);
    //创建文件的内存映射
    m_file_address=(char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;
}

//处理http请求的入口
void http_conn::process(){
    //解析http请求
    HTTP_CODE read_ret = process_read();
    if(read_ret==NO_REQUEST){//请求不完整
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;//重新读取
    }

    //生成响应
    bool write_ret=process_write(read_ret);
    if( !write_ret ){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT);//改为写
}

bool http_conn::process_write(HTTP_CODE ret){
    switch(ret)
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form)){
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form)){
                return false;
            }
            break;
        case FORBINDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form)){
                return false;
            }
            break;
        case FILE_REQUEST://如果是文件
            add_status_line(200,ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base=m_write_buf;//头部信息
            m_iv[0].iov_len=m_write_idx;
            m_iv[1].iov_base=m_file_address;//文件
            m_iv[1].iov_len=m_file_stat.st_size;
            m_iv_count=2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            return false;
    }

    //没有文件时
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

bool http_conn::add_blank_line(){
    return add_response("%s","\r\n");
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", (m_linger==true)?"keep-alive":"close");
}

bool http_conn::add_content_type(std::string type){
    return add_response("Content-Type:%s\r\n",type);
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    std::string type=get_file_type((std::string)m_real_file);
    add_content_type(type);
    add_linger();
    add_blank_line();
}

bool http_conn::add_response(const char* format,...){
    if( m_write_idx >= WRITE_BUFFER_SIZE ){
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx)){
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    return true;
}
//增加请求行
bool http_conn::add_status_line(int status, const char* title){//首行
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

std::string http_conn::get_file_type(std::string m_real_file){//获取文件类型
    std::string::size_type idx = m_real_file.find_last_of('.');
    if(idx == std::string::npos) {
        return "text/plain";
    }
    std::string suffix = m_real_file.substr(idx);
    if(SUFFIX_TYPE.count(suffix) == 1) {
        return SUFFIX_TYPE.find(suffix)->second;
    }
    return "text/plain";
}