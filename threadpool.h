#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include "locker.h"
#include <exception>
#include <cstdio>

//代码复用，所以使用模板
template<typename T>
class threadpool
{
private:
    /* data */
    int m_thread_number;//线程数量
    pthread_t* m_threads;//存放线程的数组,动态分配大小
    
    int m_max_requests;//最多可以处理的请求数量,请求队列大小
    std::list<T*> m_workqueue;//请求队列

    locker m_queuelocker;//请求队列互斥锁
    sem m_queuestat;//判断是否有任务是否需要处理
    bool m_stop;//结束线程控制
public:
    threadpool(int thread_number=8, int max_requests=10000);
    ~threadpool();
    bool append(T* request);//添加任务到队列中
private:
    static void* worker(void* arg);//线程跑的代码块
    void run();//线程工作
};

//构造函数，申请并获取线程
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),m_stop(false),m_threads(NULL)
{
    if(thread_number<=0 || max_requests<=0) throw std::exception();//数据错误

    m_threads=new pthread_t[m_thread_number];//动态创建线程池
    if(!m_threads)   throw std::exception();

    //在线程池里创建m_thread_number线程，并设置线程脱离
    for(int i=0;i<m_thread_number;++i){
        printf("create thread %dth pthread\n",i);

        if(pthread_create(m_threads+i, NULL, worker, this)!=0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        
        if(pthread_detach(m_threads[i])){
            delete[] m_threads;
            throw std::exception();
        }
    }
}

//删除掉申请的线程
template<typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
    m_stop=true;//线程根据这个值判断是否停止
}

//往请求对列里添加任务
template<typename T>
bool threadpool<T>::append(T* request){
    m_queuelocker.lock();//加锁并放入任务
    if(m_workqueue.size()>m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();//用信号量提醒有任务可以执行
    return true;
}

//线程跑的代码块，主要负责调用run函数
template<typename T>
void* threadpool<T>::worker(void* arg){
    threadpool* pool=(threadpool*)(arg);
    pool->run();//线程启动
    return pool;
}

//不断从请求对列里取出请求来处理
template<typename T>
void threadpool<T>::run()
{
    while(!m_stop){
        m_queuestat.wait();//等待有工作出现
        
        m_queuelocker.lock();
        if(m_workqueue.empty()){
            m_queuelocker.unlock();
            continue;
        }

        T* request=m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if(!request){
            continue;
        }

        request->process();//任务类需要有process来执行
    }
}

#endif