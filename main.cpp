#include "webserver.h"
#include<string.h>
int main(int argc ,char*argv[]){
   
    int port =9006;
    
    string user="test";
    string password="test";
    string dbName="test";
    //日志写入方式：默认同步写入
    int LOGWrite=0;
    //数据库校验方式，默认同步
    int SQLVerify=0;
    //保优雅的关闭连接，默认不适用
    int OPT_LINGER=0;
    //listenfd 出发模式 默认LT;
    int LISTENTrigmode = 0;
    //connfd触发模式，默认LT
    int CONNTrigmode = 0;
    // 触发组合模式,默认listenfd LT + connfd LT
    int TRIGMode=0;
    int sql_num=8;
    int thread_num=8;
    //默认不关闭日志
    int close_log=0;
    //并发模型，默认是proactor
    int actor_model=0;

    WebServer server;
    server.init(port,user,password,dbName,LOGWrite,SQLVerify,  \
        OPT_LINGER,TRIGMode,sql_num,thread_num,close_log,actor_model);
    
    //写入日志
    server.log_write();

    //TODO:线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();
    //运行
    server.eventLoop();
    return 0;
}

