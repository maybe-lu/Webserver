#ifndef LST_TIMER
#define LST_TIMER

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include "http_conn.h"

class http_conn;

//定时器类
class util_timer{
public:
    time_t expire;   // 超时时间,这里使用绝对时间
    http_conn* user_data;//指向的用户
    util_timer* prev;
    util_timer* next;
    void (*cb_func)(http_conn*); // 超时处理函数
public:
    util_timer():prev(NULL),next(NULL){}
};

//升序双链表实现定时器链表
class sort_timer_lst{
private:
    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点
public:
    sort_timer_lst() : head( NULL ), tail( NULL ) {}//构造函数
    ~sort_timer_lst() {//析构全部节点
        util_timer* tmp = head;
        while( tmp ) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }
public:
    //将定时器添加到链表
    void add_timer( util_timer* timer ){
        if( !timer ) {
            return;
        }
        if( !head ) {
            head = tail = timer;
            return; 
        }

        if( timer->expire < head->expire ) {//插入的位置是头部
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);//插入位置不是头部
    }
    //调整定时器（超时延长的情况）
    void adjust_timer(util_timer* timer){ 
        if( !timer )  {
            return;
        }
        util_timer* tmp = timer->next;
        //如果定时器在链表尾部，或定时器新的超时时间值仍然小于其下一个定时器的超时时间则不用调整
        if( !tmp || ( timer->expire < tmp->expire ) ) {
            return;
        }
        //如果要调整的是头节点
        if( timer == head ) {
            head = head->next;  //调整头节点
            head->prev = NULL;
            timer->next = NULL;
            add_timer( timer, head );   //以新头节点为准调整链表
        } else {
            // 如果不是链表的头节点，则将该定时器从链表中取出，然后插入其原来所在位置后的部分链表中
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer( timer, timer->next );
        }
    }
    //删除定时器
    void del_timer( util_timer* timer ){
        if( !timer ) {
            return;
        }
        if( ( timer == head ) && ( timer == tail ) ) {//如果只有一个定时器
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if( timer == head ) {//如果删除的是头节点
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if( timer == tail ) {//如果删除的是尾巴节点
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }
    //当SIGALARM信号被触发时，判断是否出现到期任务
    void tick(){
        if( !head ) {
            return;
        }
        //printf( "timer tick\n" );
        time_t cur = time( NULL );  // 获取当前系统时间
        util_timer* tmp = head;
        while( tmp ) {//遍历链表
            //因为链表是从小到大，所以出现第一个没超时的后面都没超时
            if( cur < tmp->expire ) {
                break;
            }

            //超时处理，调用定时器的回调函数，以执行定时任务
            tmp->cb_func( tmp->user_data );
            //执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
            head = tmp->next;
            if( head ) {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }
private:
    void add_timer(util_timer* timer, util_timer* lst_head)  {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        /* 遍历 list_head 节点之后的部分链表，直到找到一个超时时间大于目标定时器的超时时间节点
        并将目标定时器插入该节点之前 */
        while(tmp) {
            if( timer->expire < tmp->expire ) {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        //插入到链表尾部
        if( !tmp ) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }
};

#endif