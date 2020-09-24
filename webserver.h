#ifndef WEBSERVER_H
#define WEBSERVER_H
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_server.h"
#include "./timer/lst_timer.h"

const int MAX_FD=65535;  //最大文件描述符
const int MAX_EVENT_NUMBER=10000; //最大的事件数
const int TIMESLOT=5; //最小超时单位


class WebServer{
public:
    WebServer();
    ~WebServer();
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int sqlverify, int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);
    void thread_pool();
    // void sql_pool();
    void log_write();
    // 监听事件
    void trig_mode();
    void eventListen();
    void eventLoop();
    //时间相关
    void timer(int connfd,struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer,int sockfd);
    //事件处理
    bool dealClientdata();
    bool dealwithSignal(bool &timeout,bool& stopserver);

    void dealwithRead(int sockfd);
    void dealwithWrite(int sockfd);

public:
    //基础属性
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;
    //通道
    int m_pipefd[2];
    int m_epollfd;
    http_server *users;

    //数据库相关
    char *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;
    int m_SQLVerify;
    
    //线程池相关
    threadpool<http_server> *m_pool;
    int m_thread_num;

    //epoll
    epoll_event events[MAX_EVENT_NUMBER];
    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    //定时器相关
    client_data *users_timer;
    Utils utils;
};

#endif