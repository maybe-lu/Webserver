#include <stdio.h>
#include <cstdlib>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include <signal.h>
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65535    //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 //最大监听事件数量
#define TIMESLOT 5  //时间限制

static int pipefd[2];   //管道
static sort_timer_lst timer_lst;    //时间链表

//添加信号捕捉
void addsig(int sig, void(*handler)(int)){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler=handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

//添加epoll监听
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll删除
extern void removefd(int epollfd, int fd);
//修改epoll
extern void modfd(int epollfd, int fd, int ev);
//满载exit
extern void refuse(int fd);
//设置文件描述符为非阻塞
extern int setnonblocking(int fd);

//时间相关函数
void addfd( int epollfd, int fd )//ET
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}
void sig_handler( int sig ){//信号处理
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}
void addsig( int sig ){//添加时间信号的信号捕捉
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    sigaction(sig,&sa,NULL);
}
void timer_handler(){//定时轮询
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}
void cb_func(http_conn* user){//关闭连接
    //printf("断开连接\n");
    user->close_conn();
}

int main(int argc, char* argv[]){
    
    //参数不能小于2个
    if(argc<=1){
        printf("按照如下格式运行： %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port=atoi(argv[1]);

    //对SIGIPE信号处理,忽略
    //当服务器close一个连接时，若client端接着发数据。根据TCP协议的规定，会收到一个RST响应，client再往这个服务器发送数据时，系统会发出一个SIGPIPE信号给进程，告诉进程这个连接已经断开了，不要再写了。
    //因为这个信号的缺省处理方法是退出进程，为了不让主进程退出，所以需要重载处理方法
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池然后初始化
    threadpool<http_conn> *pool = NULL;
    try{
        pool=new threadpool<http_conn>;
    }catch(...){
        exit(-1);//创建失败直接退出
    }

    //创建数组保存客户信息
    http_conn* users=new http_conn[MAX_FD];

    int listenfd=socket(PF_INET, SOCK_STREAM, 0);
    //设置端口复用
    int reuse=1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    //绑定
    struct sockaddr_in address;
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port=htons(port);
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    //监听
    listen(listenfd, 5);

    //epoll代码,创建对象和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd=epoll_create(5);
    
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd=epollfd;

    //创建管道
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipefd[0]);//监听管道读端

    // 设置时间相关信号处理函数
    addsig(SIGALRM);
    addsig(SIGTERM);
    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(!stop_server){
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        //调用epoll失败
        if( (num<0) && (errno!=EINTR) ){
            printf("epoll failure\n");
            break;
        }

        for(int i=0; i<num; ++i){
            int sockfd=events[i].data.fd;
            //有客户端连接进来
            if(sockfd==listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlen=sizeof(client_address);
                int connfd=accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);

                if(http_conn::m_user_count>=MAX_FD){//目前连接数量满了,提醒客户端
                    //这里需要新的函数来调用关闭整个流程
                    
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_address);//将客户数据初始化后放入数组users中

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].m_timer = timer;
                timer_lst.add_timer( users[connfd].m_timer );   //添加到时间链表中
            }else if( ( sockfd == pipefd[0]) && (events[i].events & EPOLLIN) ){//管道读端
                int sig;
                char signals[1024];
                int ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){//对方异常断开
                users[sockfd].close_conn();
            }else if(events[i].events & EPOLLIN){//读事件发生
                util_timer* timer = users[sockfd].m_timer;
                if( users[sockfd].read() ){//读成功
                    if(timer) {//更新断开连接的时间
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer( timer );
                    }
                    pool->append(users+sockfd); //读完后交给工作线程
                }else{//读失败了
                    cb_func(&users[sockfd]);
                    if(timer)   timer_lst.del_timer( timer );
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){//写事件发生
                if( !users[sockfd].write() ){//写失败
                    users[sockfd].close_conn();
                }
            }
        }

        if( timeout ) {//处理超时连接
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );
    delete[] users;
    delete pool;

    return 0;
}
