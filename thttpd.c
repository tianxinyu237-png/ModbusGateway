#include "thttpd.h"
#include "custom_handle.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

/**
 * @brief 创建并初始化HTTP服务端监听套接字
 * @param _port 服务要绑定的端口号
 * @return 成功返回监听socket文件描述符，失败直接exit退出程序
 * `get_line` 采用单次 recv 读取 1 字节的方式，逐字节读取 HTTP 请求头一行；
 * 读到`\r`时使用`MSG_PEEK`预看下一个字符是否为`\n`，以此识别 HTTP 标准`\r\n`换行；
 * 读取完成追加字符串结束符。缺点是频繁调用 recv 系统调用，性能较低，仅适合教学演示。
 */
int init_server(int _port) //创建监听套接字
{
    // 创建TCP流式套接字 IPv4
    int sock=socket(AF_INET,SOCK_STREAM,0);
    if(sock<0)
    {
        perror("socket failed");
        exit(2);
    }
    // 设置端口地址重用，服务重启时不会出现端口被TIME_WAIT占用无法bind的问题
    int opt=1;                     
    setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    // 填充服务器地址结构体
    struct sockaddr_in local;
    local.sin_family=AF_INET;                // IPv4协议族
    local.sin_port=htons(_port);             // 端口转网络字节序
    local.sin_addr.s_addr=INADDR_ANY;        // 监听本机所有网卡IP
    
    // 绑定地址+端口
    if(bind(sock,(struct sockaddr*)&local,sizeof(local))<0)
    {
        // 【必须判】先把 errno 存下来：perror/printf 有可能改写 errno，
        // 之后再判断 errno 就判断不准了（这次踩过这个坑：提示一直打不出来）
        int saved_errno = errno;
        perror("bind failed");
        if(saved_errno == EADDRINUSE)
        {
            printf("\n【端口 %d 已被占用】可能的原因和处理办法：\n", _port);
            printf("  1) 上一次的 thttpd 还没退干净（或之前在终端里被 Ctrl+Z 挂起了——\n");
            printf("     挂起只是暂停，端口照样占着，要用 Ctrl+C 才是结束）\n");
            printf("       查占用：  ss -lntp | grep %d    或  sudo fuser %d/tcp\n", _port, _port);
            printf("       结束它：  pkill -f \"thttpd.out %d\"   （杀不掉就加 sudo）\n", _port);
            printf("       挂起过的：先 fg 回到前台再 Ctrl+C，或 kill -CONT <pid> 后 kill <pid>\n");
            printf("  2) 换个端口启动：      ./thttpd.out 8081\n");
            printf("  3) 只想测试不想用 sudo：用 8080 之类的高位端口，80 端口需要 root\n");
            fflush(stdout);
        }
        exit(3);
    }
    // 开始监听，第二个参数为全连接队列最大长度
    if(listen(sock,5)<0)
    {
        perror("listen failed");
        exit(4);
    }
    return sock;
}

/**
 * @brief 从socket中按行读取http报文，以'\n'作为行结束
 * @param sock 客户端连接套接字
 * @param buf 存放读取一行数据的缓冲区
 * @return 返回读到的字符个数
 * @note HTTP报文行结尾是 \r\n，这里做兼容处理
 *`get_line`逐字节 recv 读取 socket，目的读取 HTTP 一行（`\r\n`结尾），用 MSG_PEEK 预判`\r`后面是否存在`\n`；**代码存在严重笔误 bug：循环条件写成`ch!='n'`而不是`ch!='\n'`**，同时缺少 recv 错误处理、频繁单字节 recv 造成性能差，读到`\r\n`会将两个字符存入缓冲区，和上层判断空行的逻辑不匹配。
 */
static int get_line(int sock,char* buf)   //按行读取请求报头
{
    char ch='\0';
    int i=0;
    ssize_t ret=0;
    // 循环读取单个字符，直到缓冲区满或者读到换行符
    // 【必须判】i 上限要用 SIZE-1：循环结束时下面还要写 buf[i]='\0'，
    // 如果 i 能到 SIZE，那一句就写到缓冲区外面了（栈被踩1字节）
    while(i < SIZE-1 && ch!='\n')
    {
        ret=recv(sock,&ch,1,0);
        // 如果读到\r，预看下一个字符是不是\n
        if(ret>0&&ch=='\r')
        {
            ssize_t s=recv(sock,&ch,1,MSG_PEEK);
            if(s>0&&ch=='\n')
            {
                // 确认后面是\n，把\n读走
                recv(sock,&ch,1,0);
            }
            else
            {
                // 单独\r，手动置为\n结束本行
                ch='\n';
            }
        }
        buf[i++]=ch;
    }
    buf[i]='\0'; // 字符串收尾
    return i;
}

/**
 * @brief 清空HTTP剩余请求头部，读到空行为止（\r\n\r\n）
 * @param sock 客户端socket
 */
static void clear_header(int sock)    //清空消息报头
{
    char buf[SIZE];
    int ret=0;
    // 循环读行，直到读到长度为1的空行（代表头部结束）
    do
    {
        ret=get_line(sock,buf);
    }while(ret!=1||(strcmp(buf,"\n")!=0));
}

/**
 * @brief 返回404 Not Found页面给客户端
 * @param sock 客户端socket
 * `show_404` 是静态内部函数，用于返回 404 页面；
 * 首先调用`clear_header`清空 socket 缓冲区残留的 HTTP 请求头，防止报文残留干扰解析；
 * 然后 send 发送 404 状态行，通过 sendfile 零拷贝发送 404.html 文件作为响应体；
 * 代码缺少对 404.html 文件存在性校验，属于简易 web 服务器的简化实
 */
static void show_404(int sock)      //404错误处理
{
    clear_header(sock); // 先把请求剩余头部读完
    // HTTP响应状态行
    char* msg="HTTP/1.0 404    Not Found\r\n";
    send(sock,msg,strlen(msg),0);         //发送状态行
    send(sock,"\r\n",strlen("\r\n"),0);      //发送空行，分隔响应头和响应体
    struct stat st;
    int fd = -1;
//如果`wwwroot/404.html`**文件丢失**：`stat`失败，`st.st_size`是栈上随机脏值；`open`返回`-1`（无效 fd）。
//直接拿脏的`st.st_size`传给 sendfile，同时 sendfile 操作非法 fd，程序大概率崩溃。
// ✅ 必须判断：`if(stat(...) == -1)`、`if(fd < 0)`，做兜底处理。
    // stat 和 open 的返回值都要判，两个都成功才敢 sendfile
    if(stat("wwwroot/404.html",&st) == 0 && st.st_size > 0 &&
       (fd = open("wwwroot/404.html",O_RDONLY)) >= 0)
    {
        // sendfile零拷贝，直接把文件内容发给客户端
        sendfile(sock,fd,NULL,st.st_size);
        close(fd);
    }
    else
    {
        // 兜底：404页面拿不到也不能崩，直接发一段纯文本，保证客户端一定收到响应
        const char *fallback = "<h1>404 Not Found</h1>";
        send(sock,fallback,strlen(fallback),0);
    }
}

/**
 * @brief 统一错误响应分发函数，根据错误码返回对应页面
 * @param sock 客户端socket
 * @param err_code http错误码(403/404/405/500)
 * @note 目前只实现了404，其他错误分支预留
 */
void echo_error(int sock,int err_code)    //错误处理
{
    const char *status = NULL;
    const char *text = NULL;

    switch(err_code)
    {
    case 403:
        status = "HTTP/1.0 403 Forbidden\r\n";
        text   = "<h1>403 Forbidden</h1>";        // 403：文件打不开/没权限
        break;
    case 404:
        show_404(sock);                           // 404 有专门页面，交给 show_404
        return;
    case 405:
        status = "HTTP/1.0 405 Method Not Allowed\r\n";
        text   = "<h1>405 Method Not Allowed</h1>"; // 405：不是GET/POST
        break;
    case 500:
        status = "HTTP/1.0 500 Internal Server Error\r\n";
        text   = "<h1>500 Internal Server Error</h1>"; // 500：sendfile 出错等
        break;
    default:  // 原代码拼写defaut，这里顺便修正
        status = "HTTP/1.0 400 Bad Request\r\n";
        text   = "<h1>400 Bad Request</h1>";
        break;
    }

    // 注意：这里不能调 clear_header，因为 echo_www 出错进来时请求头已经被读完，
    // 再读会阻塞等客户端发数据，把线程挂死
    char head[64];
    send(sock,status,strlen(status),0);                       // 状态行
    sprintf(head,"Content-Length: %d\r\n\r\n",(int)strlen(text));
    send(sock,head,strlen(head),0);                           // 响应头 + 空行
    send(sock,text,strlen(text),0);                           // 响应体
}

/**
 * @brief 读取静态本地文件，返回200响应+文件内容给浏览器
 * @param sock 客户端socket
 * @param path 本地文件路径（wwwroot下）
 * @param s 文件大小
 * @return 0成功，非0错误码
 * `echo_www`是 static 内部函数，专门处理静态文件请求。
 * 先 open 打开本地资源，打开失败返回 403；
 * 发送 200 OK 状态行，使用 sendfile 零拷贝传输文件内容；
 * sendfile 传输异常返回 500；
 * 代码缺少 Content-Length、Content-Type 等标准 HTTP 响应头，属于教学简化版本。
 */
static int echo_www(int sock,const char * path,size_t s)  //处理非CGI的请求
{
    int fd=open(path,O_RDONLY);
    if(fd<0)
    {
        echo_error(sock,403);
        return 7;
    }
    // 200成功状态行
    char* msg="HTTP/1.0 200 OK\r\n";
    send(sock,msg,strlen(msg),0);         //发送状态行
    send(sock,"\r\n",strlen("\r\n"),0);      //发送空行
    
    //sendfile方法可以直接把文件发送到网络对端，内核零拷贝，效率高
    if(sendfile(sock,fd,NULL,s)<0)
    {
        echo_error(sock,500);
        return 8;    
    }
    close(fd);
    return 0;
}

/**
 * @brief 一次性读完整个请求头，顺便取出 Content-Length 和 Authorization
 * @param content_len 输出请求体长度，没找到返回-1
 * @param auth 输出 Authorization 头的值（形如 "Bearer xxxx"），没有则空串
 * @note 取代原来"GET用clear_header、POST内联循环"两套写法：
 *       合并成一处，而且 GET 请求也能拿到 Authorization（token 就放在请求头里）
 */
static void read_headers(int sock, int *content_len, char *auth, int auth_len)
{
    char line[SIZE];
    int ret = 0;

    if(content_len) *content_len = -1;
    if(auth && auth_len > 0) auth[0] = '\0';

    do
    {
        ret = get_line(sock, line);

        if(strncasecmp(line, "content-length:", 15) == 0 && content_len != NULL)
        {
            *content_len = atoi(line + 15);
        }
        else if(strncasecmp(line, "authorization:", 14) == 0 && auth != NULL && auth_len > 0)
        {
            char *v = line + 14;
            int i = 0;
            while(*v == ' ' || *v == '\t') v++;              // 跳过冒号后面的空格
            while(*v != '\0' && *v != '\r' && *v != '\n' && i < auth_len - 1) auth[i++] = *v++;
            auth[i] = '\0';
        }
    }while(ret != 1 && strcmp(line, "\n") != 0);
}

/**
 * @brief 处理HTTP请求的入口函数，区分GET与POST
 * @param sock 客户端连接套接字fd
 * @param method 请求方法：GET / POST
 * @param path 请求资源路径（含 wwwroot 前缀）
 * @param query_string URL问号后面的查询参数（GET参数）
 * @return int 执行结果，0代表正常
 */
static int handle_request(int sock,const char* method,
        const char* path,const char* query_string)
{
    int content_len = -1;       // POST请求体长度，初始-1代表未找到
    char auth[128] = {0};       // 请求头里的 Authorization（token）
    char req_buf[4096] = {0};   // 请求体缓冲区（表单 或 JSON），初始清零
    const char *url = path + strlen("wwwroot");   // 去掉静态根前缀，得到真正的URL

    // 一次性读完所有请求头，顺便取出 Content-Length / Authorization
    read_headers(sock, &content_len, auth, sizeof(auth));

    // 控制台打印调试信息
    printf("method = %s\n", method);
    printf("query_string = %s\n", query_string);
    printf("content_len = %d\n", content_len);
    if(auth[0] != '\0') printf("auth = %.24s...\n", auth);

    // 如果是POST请求，读取请求体（表单 或 JSON）
    if(strcasecmp(method,"POST")==0)
    {
        // 【必须判】Content-Length 是客户端给的任意数字，直接当长度传给 recv
        // 会踩爆 4096 字节的栈缓冲区，先夹到缓冲区大小以内
        if(content_len < 0) content_len = 0;
        if(content_len > (int)sizeof(req_buf)-1) content_len = sizeof(req_buf)-1;

        // 根据Content-Length读取指定长度的请求体，存入req_buf
        int len = recv(sock, req_buf, content_len, 0);
        printf("len = %d\n", len);
        printf("req_buf = %s\n", req_buf);
    }

    // REST接口(/api/xxx)由业务层自己发送完整响应（它要能返回401等状态码），
    // 所以这里不能抢先发 200 状态行；老接口照旧先发状态行再调业务
    if(strncmp(url, "/api/", 5) != 0)
    {
        // 发送HTTP 200 OK 响应状态行 + 空行（响应头结束）
        char* msg="HTTP/1.1 200 OK\r\n\r\n";
        send(sock,msg,strlen(msg),0);
    }

    // 调用业务处理函数，交给custom_handle.c里面的自定义业务逻辑
    // url：真实路径；query_string：GET参数；req_buf：请求体；auth：token
    parse_and_process(sock, url, query_string, req_buf, auth);

    return 0;
}


/**
 * @brief 单次客户端连接入口函数，解析http请求，分发静态资源/业务处理
 * @param sock 客户端accept得到的连接socket
 * @return 处理结果码
 * @note 函数末尾会关闭本次连接socket（短连接模型）
 # `handler_msg(int sock)` 整体定位
`handler_msg` 是【单次客户端连接的总入口函数】**，整个一次 HTTP 请求的**总调度中心。
当主线程`accept`拿到客户端`sock`，创建子线程，线程函数就调用 `handler_msg(sock)`。
所有上面你看到的：`get_line`、`clear_header`、`echo_www`、`handle_request`、`echo_error`、`show_404` 全部都是**被 handler_msg 调用的子函数。
 */
int handler_msg(int sock)       //浏览器请求处理函数
{
    char del_buf[SIZE] = {};
    //MSG_PEEK：偷看缓冲区数据，数据读完后还留在内核缓冲区，不拿走
    recv(sock,del_buf,SIZE,MSG_PEEK);
#if 1 //初学者强烈建议打开这个开关，看看tcp实际请求的协议格式
    puts("---------------------------------------");
    printf("recv:%s\n",del_buf);
    puts("---------------------------------------");
#endif
    //读取请求首行：GET /xxx?a=1 HTTP/1.1
    char buf[SIZE];
 //**读取请求首行**
//调用`get_line(sock,buf)`读出第一行，例：`GET /index.html?a=1 HTTP/1.0`。然后手写代码**分割字符串**：
    int count=get_line(sock,buf);
    int ret=0;
    char method[32];
    char url[SIZE];
    char *query_string=NULL;
    int i=0;
    int j=0;
    int need_handle=0; //标记是否需要交给自定义业务函数处理

    // ---------- 第 1 段：切出 method ----------
    //【解析请求方法 GET / POST】
 //第一段代码：提取 method（GET / POST）
    while(j<count)
    {
        if(isspace(buf[j]))
        {
            break;
        }
        method[i]=buf[j];    
        i++;
        j++;
    }
    method[i]='\0';
    //跳过方法和url之间的空格
 //作用：把指针 j，**移动过方法后面全部空白字符，定位到 URL 的第一个字符**
    while(isspace(buf[j])&&j<SIZE)      //过滤空格
    {
        j++;
    }
    // ---------- 第 2 段：合法性检查 ----------
    //这里开始就开始判断发过来的请求是GET还是POST了
 //如果请求方法不是 GET、POST，直接返回`405 Method Not Allowed`，跳转结束。
    if(strcasecmp(method,"POST")&&strcasecmp(method,"GET"))
    {
        printf("method failed\n");  //如果都不是，那么提示一下
        // 先把剩下的请求头读完再回405，否则 close 时接收缓冲区还有未读数据，
        // 内核会发 RST，把已经发出去的响应一起冲掉（浏览器只看到"连接被重置"）
        clear_header(sock);
        echo_error(sock,405);
        ret=5;
        goto end;
    }
    if(strcasecmp(method,"POST")==0)
    {
        need_handle=1; //POST请求，走自定义业务处理
    }    
    
    
    // ---------- 第 3 段：切出 url 和 query_string ----------
    //【解析URL，分离路径和query参数（?后面内容）】
    i=0;
    while(j<count)
    {
        if(isspace(buf[j]))
        {
            break;
        }
        if(buf[j]=='?')
        {
            //遇到?，前面是资源路径，后面是query参数
            query_string=&url[i];
            query_string++;
            url[i]='\0';
        }
        else{
            url[i]=buf[j];
        }
        j++;
        i++;
    }
    url[i]='\0';
    printf("query_string = %s\n", query_string);
    //浏览器通过http://192.168.8.208:8080/?test=1234这种形式请求
    //是携带参数的意思，那么就需要额外处理了
    if(strcasecmp(method,"GET")==0&&query_string!=NULL)
    {
        need_handle=1; //GET带?参数，交给业务处理
    }

    // REST接口(/api/xxx)：不管有没有?参数、GET还是POST，都必须进业务层
    // （不加这条的话 GET /api/realtime 会被当成静态文件去找 → 404）
    if(strncmp(url,"/api/",5)==0)
    {
        need_handle=1;
    }
    // ---------- 第 4 段：拼真实文件路径 ----------
    //拼接本地文件路径，根目录为wwwroot
 //把浏览器访问的 url 映射到服务器本地`wwwroot`文件夹；
//访问目录 `/` 自动拼接`index.html`。
    // 【必须判】防目录穿越：url 里只要出现 .. 就能顺着 wwwroot/.. 读到工程目录里的源码、
    // 数据库甚至系统文件（实测 GET /../sensor.db 能把整个数据库下载走），直接拒绝
    if(strstr(url,"..") != NULL)
    {
        printf("blocked path traversal: %s\n", url);
        clear_header(sock);
        echo_error(sock,403);
        ret=9;
        goto end;
    }

    char path[SIZE];
    sprintf(path,"wwwroot%s",url);       
    
    printf("path = %s\n", path);
    
    //如果请求地址没有携带任何资源，那么默认返回index.html
    if(path[strlen(path)-1]=='/')              //判断浏览器请求的是不是目录
    {
        strcat(path,"app/index.html");         //如果请求的是目录，则就把该目录下的首页返回回去
    }
    //到这里基本就能确定是否需要自己的程序来处理后续请求了
    printf("need progress handle:%d\n",need_handle);

    // ---------- 第 5 段：这个url在磁盘上到底有没有对应文件 ----------
    struct stat st;
    int is_static_file = (stat(path,&st)==0 && S_ISREG(st.st_mode));

    // ---------- 第 6 段：分流 ----------
    // 规则（顺序不能乱）：
    //  1) GET + 磁盘上真有这个文件 -> 发文件。带?参数也照发，
    //     否则 /index.html?v=1、/post.html?utm=x 这种参数会把静态页面变成JSON
    //  2) 剩下该走业务的走业务：POST请求；或url不是磁盘文件的带参数GET（如 /api?cmd=realtime）
    //  3) 既不是文件、又没有业务要处理 -> 404
    if(is_static_file && strcasecmp(method,"GET")==0)
    {
        clear_header(sock);
        //如果是GET方法，而且请求的是真实存在的静态文件，则直接返回静态资源
        ret=echo_www(sock,path,st.st_size);
    }

    else if(need_handle)
    {
        //动态业务的url是接口名(如 /login、/api)，磁盘上本来就没有这个文件
        ret=handle_request(sock,method,path,query_string);
    }

    else
    {
        // ---------- 确实找不到 ----------
 //`stat(path,&st)`：检查 wwwroot 下这个文件是否存在
 //stat 失败 → 文件不存在，调用`echo_error(404)`，goto end 结束。
        printf("can't find file\n");
        echo_error(sock,404);
        ret=6;
        goto end;
    }
    //end 收尾:`close(sock)`：**短连接模型，一次 HTTP 请求处理完成，直接关闭客户端 socket**
end:
    close(sock); //短连接，处理完毕关闭客户端套接字
    return ret;
}
