#include "webserver.h"
#include "./timer/lst_timer.h"

WebServer::WebServer(){
    //初始化MAXFD个http响应对象
    users=new http_server[MAX_FD];

    char server_path[2000];
    getcwd(server_path,200);

    char root[6]="/root";
    m_root=(char *)malloc(strlen(server_path)+strlen(root)+1);
    strcpy(m_root,server_path);
    strcat(m_root,root);

    users_timer=new client_data[MAX_FD];
}

WebServer::~WebServer(){
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}


void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, int sqlverify,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_SQLVerify = sqlverify;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode(){
    //LT+LT
    if(0==m_TRIGMode){
        m_LISTENTrigmode=0;
        m_CONNTrigmode=0;
    }
    //LT+ET
    if(1==m_TRIGMode){
        m_LISTENTrigmode=0;
        m_CONNTrigmode=1;
    }
    //ET+LT
    if(2==m_TRIGMode){
        m_LISTENTrigmode=1;
        m_CONNTrigmode=0;
    }
    //ET+ET
    if(3==m_TRIGMode){
        m_LISTENTrigmode=1;
        m_CONNTrigmode=1;
    }
}

//开启日志线程
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

//启动线程池
void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_server>(m_actormodel, NULL, m_thread_num);
}

//事件监听

void WebServer::eventListen(){
    //监听socet连接
    m_listenfd=socket(PF_INET,SOCK_STREAM,0);
    assert(m_listenfd>0);

    //提前配置好socket的设置
    if(0==m_OPT_LINGER){
        struct  linger tmp={0,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }
    else if(1==m_OPT_LINGER){
        struct linger tmp={1,1};
        setsockopt(m_listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));
    }

    int ret=0;
    struct sockaddr_in address;
    bzero(&address,sizeof(address));
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_ANY);
    address.sin_port=htons(m_port);

    int flag=1;
    setsockopt(m_listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));

    ret=bind(m_listenfd,(struct sockaddr *)&address,sizeof(address));
    assert(ret>=0);
    ret=listen(m_listenfd,5);
    assert(ret>=0);

    //添加信号，工具类
    Utils::u_pipefd=m_pipefd;
    Utils::u_epollfd=m_epollfd;

    utils.init(TIMESLOT);

    //epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd=epoll_create(5);
    assert(m_epollfd!=-1);
    utils.addfd(m_epollfd,m_listenfd,false,m_LISTENTrigmode);

    http_server::m_epollfd=m_epollfd;
    //TODO:socketpair是？
    ret=socketpair(PF_UNIX,SOCK_STREAM,0,m_pipefd);

    assert(ret!=-1);

    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd,m_pipefd[0],false,0);

    utils.addsig(SIGPIPE,SIG_IGN);
    utils.addsig(SIGALRM,utils.sig_handler,false);
    utils.addsig(SIGTERM,utils.sig_handler,false);
    alarm(TIMESLOT); 
}


//初始化client_data数据
//创建定时器，社子和回调函数和超时事件，绑定用户数据，将定时器添加到链表中
void WebServer::timer(int connfd,struct sockaddr_in client_address){
    users[connfd].init(connfd, client_address, m_root, m_SQLVerify, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    //创建定时器
    util_timer *timer=new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    //添加定时器到list中
    utils.m_timer_lst.add_timer(timer);
}

//socket上有数据传输时，则将定时器往后推迟3各单位
//并对新的定时器在链表中的位置进行调整

void WebServer::adjust_timer(util_timer *timer){
    time_t cur=time(NULL);
    timer->expire=cur+3*TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
   
    // LOG_INFO("%s","adjust timer once");
    printf("adjust timer once\n");
}
//关闭连接时删除定时器
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    printf("close fd %d", users_timer[sockfd].sockfd);
    // LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//接受用户的数据
bool WebServer::dealClientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    //监听模式0:LT 1:ET
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            // LOG_ERROR("%s:errno is:%d", "accept error", errno);
            printf("errno is:%d\n accept error", errno);
            return false;
        }
        if (http_server::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            // LOG_ERROR("%s", "Internal server busy");
            printf("Internal server busy \n");
            return false;
        }
        //t初始化client_data的数据，并创建定时器和超时时间
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                // LOG_ERROR("%s:errno is:%d", "accept error", errno);
                printf("errno is:%d\n accept error", errno);
                break;
            }
            if (http_server::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                // LOG_ERROR("%s", "Internal server busy");
                printf("Internal server busy \n");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

//处理定时器发送来的信号
bool WebServer::dealwithSignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

//处理读socket事件
void WebServer::dealwithRead(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }
        printf("deal with the client(%s) by reactor\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        //若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            // LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
            printf("deal with the client(%s) by proactor\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

//处理写socket事件
void WebServer::dealwithWrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }
        printf("send to the client(%s) by reactor\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            // LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
            printf("send to the client(%s) by proactor\n", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout =false;
    bool stop_server=false;
    while(!stop_server){
        int number=epoll_wait(m_epollfd,events,MAX_EVENT_NUMBER,-1);
        if(number<0 && errno !=EINTR){
            printf("epoll failure");
            break;
        }
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)
            {
                bool flag = dealClientdata();
                if (false == flag)
                {
                    printf("read clientdata failed\n");
                    continue;
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithSignal(timeout, stop_server);
                if (false == flag)
                {
                    printf("dealwithSignal failed\n");
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                printf("go to dealwithRead()\n");
                dealwithRead(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                printf("go to dealwithWrite()\n");
                dealwithWrite(sockfd);
            }
        }
        if(timeout){
            utils.timer_handler();
            printf("timer tick,now connect client num=%d\n",http_server::m_user_count);
            timeout=false;
        }
    }
}






