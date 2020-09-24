#include"http_server.h"
#include<fstream>
#include<string>
#include<map>

const char *ok_200_title="OK";
const char *error_400_title="Bad Request";
const char *error_400_form="request has bad syntax";
const char *error_403_title="Forbidden";
const char *error_403_form="you do not have permission to get file ";
const char *error_404_title="Not Found";
const char * error_404_form="the request file was not found on this server";
const char *error_500_title="Internal Error";
const char *error_500_form="there was an unusual problem serving the request";

locker m_lock;
map<string ,string> users;

//设置文件描述符为非阻塞
int setnonblocking(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//将内核事件表注册为读事件，ET事件，选择开启EPOLLONESHOT
void addfd(int epollfd,int fd,bool one_shot,int TRIGMode){
    epoll_event event;
    event.data.fd=fd;
    if(1==TRIGMode){
        event.events=EPOLLIN | EPOLLET | EPOLLRDHUP;
    }else{
        event.events=EPOLLIN || EPOLLRDHUP;
    }
    if(one_shot){
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

int http_server::m_user_count=0;
int http_server::m_epollfd=-1;


//在内核中关闭sockfd，删除对epollfd的占用。
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

void http_server::close_conn(bool real_close){
    if(real_close && (m_sockfd!=-1)){
        printf("close sockfd=%d\n",m_sockfd);
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}


void modfd(int epollfd,int fd,int ev,int TRIGMode){
    epoll_event event;
    event.data.fd=fd;
    if(1==TRIGMode){
        event.events=ev|EPOLLET| EPOLLRDHUP| EPOLLONESHOT;
    }else{
        event.events=ev| EPOLLONESHOT | EPOLLRDHUP;
    }
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}
void http_server::init(int sockfd,const sockaddr_in &addr,char *root,int SQLVerify,int TRIGMode,int close_log,string user,string passwd,string sqlname){
    m_sockfd=sockfd;
    m_address=addr;

    addfd(m_epollfd,sockfd,true,m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能时网站根目录出错，http响应格式错误，或者访问内容为空。
    doc_root=root;
    m_SQLVerify=SQLVerify;
    m_TRIGMode=TRIGMode;
    m_close_log=close_log;

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_server::init()
{
    // mysql = NULL;
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

//从状态机，用于分析一行的内容
//返回值为行的读取状态，LINE_XX

http_server::LINE_STATUS http_server::parse_line(){
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r'){
            if((m_checked_idx+1)==m_read_idx){
                return LINE_OPEN;
            }else if(m_read_buf[m_checked_idx+1]=='\n'){
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp='\n'){
            
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//ET模式下，仅支持一次性把数据全部读完
//循环都客户数据，知道没有数据可以读

bool http_server::read_once(){
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read=0;
    //LT模式
    if(0 == m_TRIGMode){
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx+=bytes_read;
        if(bytes_read<=0){
            return false;
        }
        return true;
    }
    //ET模式读取数据
    else{
        while(true){
            bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
            if(bytes_read==-1){
                if(errno==EAGAIN || errno==EWOULDBLOCK)
                    printf("read later\n");
                    break;
                return false;
            }
            else if(bytes_read==0){
                return false;
            }
            m_read_idx+=bytes_read;
        }
        return true;
    }
}


//解析请求行，获取请求方法，目标url，以及http版本号

http_server::HTTP_CODE http_server::parse_request_line(char *text){
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析头部信息
http_server::HTTP_CODE http_server::parse_headers(char *text){
    if(text[0]=='\0'){
        if(m_content_length!=0){
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0){
        text+=11;
        text+=strspn(text,"\t");
        if(strcasecmp(text,"keep-alive")==0){
            //保持长连接
            m_linger=true;
        }
    }
    else if(strncasecmp(text,"Content-length:",15)==0){
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0){
        //获取host
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else{
        // LOG_INFO("uhh！error header :%s",text);
        printf("uhh！error header :%s",text);
    }
    return NO_REQUEST;

}

//判断http请求是否被完整的读入
http_server::HTTP_CODE http_server::parse_content(char *text){
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        m_string =text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//处理从客户端返回的数据
http_server::HTTP_CODE http_server::process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char *text=NULL;
    while((m_content_length==CHECK_STATE_CONTENT && line_status==LINE_OK) || ((line_status=parse_line())==LINE_OK)){
        text=get_line();
        m_start_line=m_checked_idx;
        // LOG_INFO("%s",text);
        printf("%s\n",text);
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE: //解析行是否正常
            {
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: //解析头
            {
                ret=parse_headers(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST){
                    //TODO:new function
                    return do_request();
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
                printf("INternal error\n");
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}


//处理http请求
http_server::HTTP_CODE http_server::do_request(){

    //TODO:存在地址越界的危险
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);
    const char *p=strrchr(m_url,'/');
    //处理POST请求
    if(cgi==1 && (*(p+1)=='2' || *(p+1)=='3')){
        if(*(p+1)==2){
            printf("req event 2 on cgi\n");
        }
        if(*(p+1)==3){
            printf("req event 3 on cgi\n");
        }
    }
    if(*(p+1)=='0'){
        printf("req event 0\n");
    }else if(*(p+1)=='1'){
        printf("req event 1 \n");
    }else if(*(p+1)=='5'){
        printf("req event 5\n");
    }else if(*(p+1)=='6'){
        printf("req event 6\n");
    }else if(*(p+1)=='7'){
        printf("req event 7\n");
    }
    //默认返回文件
    char *default_url=(char * )malloc(sizeof(char)*200);
    strcpy(default_url,"/index.html");
    strncpy(m_real_file+len,default_url,strlen(default_url));
    free(default_url);
    if(stat(m_real_file, &m_file_stat)<0){
        return NO_RESOURCE;
    }
    if(!(m_file_stat.st_mode & S_IROTH))
        return FORBINDDEN_REQUEST;

    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }

    int fd=open(m_real_file,O_RDONLY);
    //把资源文件读入到m_file_address;
    m_file_address=(char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;

}

//释放资源
void http_server::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=NULL;
    }
}

//写到socket资源中
bool http_server::write(){
    int temp=0;
    if(bytes_to_send==0){
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_iv_count);
        init();
        return 0;
    }
    while(1){
        temp=writev(m_sockfd,m_iv,m_iv_count);
        if(temp<0){
            if(errno==EAGAIN){
                modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }
        //TODO：发送缓冲数据的，需要再理解一下
        bytes_have_send+=temp;
        bytes_to_send-=temp; 
        if(bytes_have_send>=m_iv[0].iov_len){
            m_iv[0].iov_len=0;
            m_iv[1].iov_base=m_file_address+(bytes_have_send-m_write_idx);
            m_iv[1].iov_len=bytes_to_send;
        }else{
            m_iv[0].iov_base=m_file_address+bytes_have_send;
            m_iv[0].iov_len=m_iv[0].iov_len-bytes_have_send;
        }

        if(bytes_to_send<=0){
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
            if(m_linger){
                init();
                return true;
            }else{
                return false;
            }
        }   

    }
}

//处理响应的http数据
bool http_server::add_response(const char *format,...){
    if(m_write_idx>=WRITE_BUFFER_SIZE){
        return false;    
    }
    //TODO: va_list ?
    va_list arg_list;

    va_start(arg_list,format);
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    if(len>= (WRITE_BUFFER_SIZE-1-m_write_idx)){
        va_end(arg_list);
        return false;
    }
    m_write_idx+=len;
    va_end(arg_list);
    // LOG_INFO("response:%s",m_write_buf);
    printf("response:%s\n",m_write_buf);
    return true;
}

bool http_server::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_server::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_server::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_server::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_server::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_server::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_server::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_server::process_write(http_server::HTTP_CODE ret){
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBINDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            printf("file size=%ld\n",m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
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

//主调函数
void http_server::process(){
    HTTP_CODE read_ret=process_read();
    if(read_ret==NO_REQUEST){
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        return;
    }
    bool write_ret=process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
}

