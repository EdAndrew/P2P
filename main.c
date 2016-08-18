#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "config.h"
#include "thpool.h"

extern sqlite3 *db;
typedef struct
{
	int fd;
	char ipaddr[128];
} p2p_t;

int loc_fd, inc_fd;
struct sockaddr_storage inc_addr;
socklen_t inc_len = sizeof(inc_addr);

threadpool thpool;
pthread_t net_thread;

int num_threads = NUM_THREADS;
int pidfile;
char *port = (char *)DEFAULT_PORT;
int queue_length = QUEUE_LENGTH;

char clientaddr[128] = { '\0' };
sqlite3 *db;
time_t start_time;
char *term;
static int c_count = 0;

void clean_string(char *);//字符串处理，去除诸如\b之类的转义符
int client_count(int);//自加一，计算客户端数量
void console_help();//打印帮助信息
void *get_in_addr(struct sockaddr *);//获取IP地址
int recv_msg(int, char *);
int send_msg(int, char *);
int validate_int(char *);
void print_stats();
void stat_handler();
void shutdown_handler();
void *p2p(void *);
void *tcp_listen();

int main(int argc, char *argv[])
{
    //======系统环境设置======
    struct addrinfo hints, *result;
    int yes = 1;
    char command[512] = { '\0' };
    int i = 0;
    sqlite3_stmt *stmt;
    char query[256] = { '\0' };

    //调用shutdown_handler处理三种情况
    signal(SIGHUP, shutdown_handler);//关闭终端
    signal(SIGINT, shutdown_handler);//按下CTRL+C
    signal(SIGTERM, shutdown_handler);//kill命令

    // 注册自定义信号
    signal(SIGUSR1, stat_handler);
    signal(SIGUSR2, stat_handler);

    term = strdup(ttyname(1));

    fprintf(stdout, "%s: %s 正在初始化%s...  \n", SERVER_NAME, INFO_MSG, SERVER_NAME);

    start_time = time(NULL);//开始计时

    //======处理执行参数======

    for(i = 1; i < argc; ++i)
    {
        if(strcmp("-h", argv[i]) == 0 || strcmp("--help", argv[i]) == 0)
        {
            fprintf(stdout, "usage: %s [-h | --help] [-p | --port port] [-q | --queue queue_length] [-t | --threads thread_count]\n\n", SERVER_NAME);
            fprintf(stdout, "%s 参数说明:\n", SERVER_NAME);
            fprintf(stdout, "\t-h | --help:            help - 展示帮助信息\n");
            fprintf(stdout, "\t-p | --port:            port - 为服务器指定一个端口号(默认: %s)\n", DEFAULT_PORT);
            fprintf(stdout, "\t-q | --queue:   queue_length - 为服务器指定连接队列的长度(默认: %d)\n", QUEUE_LENGTH);
            fprintf(stdout, "\t-t | --threads: thread_count - 为服务器指定连接池的长度(也就是最大支持的客户端数量) (默认: %d)\n", NUM_THREADS);
            fprintf(stdout, "\n");
            //退出
            exit(0);
        }
        else if(strcmp("-p", argv[i]) == 0 || strcmp("--port", argv[i]) == 0)
        {
            if(argv[i+1] != NULL)
            {
                if(validate_int(argv[i+1]))
                {
                    if(atoi(argv[i+1]) >= 0 && atoi(argv[i+1]) <= MAX_PORT)
                    {
                        port = argv[i+1];
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s 端口号不在范围内(0-%d), 恢复默认端口号 %s\n", SERVER_NAME, ERROR_MSG, MAX_PORT, DEFAULT_PORT);
                }
                else
                {
                    fprintf(stderr, "%s: %s 指定的端口号非法, 恢复默认端口号 %s\n", SERVER_NAME, ERROR_MSG, DEFAULT_PORT);
                }
            }
            else
            {
                fprintf(stderr, "%s: %s 没有在port参数后找到端口值, 恢复默认端口号 %s\n", SERVER_NAME, ERROR_MSG, DEFAULT_PORT);
            }
        }
        else if(strcmp("-q", argv[i]) == 0 || strcmp("--queue", argv[i]) == 0)
        {
            if(argv[i+1] != NULL)
            {
                if(validate_int(argv[i+1]))
                {
                    if(atoi(argv[i+1]) >= 1)
                    {
                        queue_length = atoi(argv[i+1]);
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s 队列不能为非正数, 恢复默认队列长度 %d\n", SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
                }
                else
                {
                    fprintf(stderr, "%s: %s 队列长度参数非法, 恢复默认队列长度 %d\n", SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
                }
            }
            else
            {
                // Print error and use default queue length if no length was specified after the flag
                fprintf(stderr, "%s: %s 没有在queue参数后找到队列长度, 恢复默认队列长度 %d\n", SERVER_NAME, ERROR_MSG, QUEUE_LENGTH);
            }
        }
        else if(strcmp("-t", argv[i]) == 0 || strcmp("--threads", argv[i]) == 0)
        {
            if(argv[i+1] != NULL)
            {
                if(validate_int(argv[i+1]))
                {
                    if(atoi(argv[i+1]) >= 1)
                    {
                        num_threads = atoi(argv[i+1]);
                        i++;
                    }
                    else
                        fprintf(stderr, "%s: %s 线程数不能为非正数, 恢复默认 %d 线程数\n", SERVER_NAME, ERROR_MSG, NUM_THREADS);
                }
                else
                {
                    fprintf(stderr, "%s: %s 线程数参数非法, 恢复默认 %d 线程数\n", SERVER_NAME, ERROR_MSG, NUM_THREADS);
                }
            }
            else
            {
                fprintf(stderr, "%s: %s 没有在thread参数后找到线程数, 恢复默认 %d 线程数\n", SERVER_NAME, ERROR_MSG, NUM_THREADS);
            }
        }
        else
        {
            fprintf(stderr, "%s: %s 检测到未知参数'%s' , 输入 '%s -h' 查看usage \n", SERVER_NAME, ERROR_MSG, argv[i], SERVER_NAME);
            exit(-1);
        }
    }

    //======准备数据库======

    sqlite3_open(DB_FILE, &db);
    if(db == NULL)
    {
        fprintf(stderr, "%s: %s sqlite: 不能打开SQLite %s\n", SERVER_NAME, ERROR_MSG, DB_FILE);
        exit(-1);
    }
    sprintf(query, "DELETE FROM files");
    sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
    if(sqlite3_step(stmt) != SQLITE_DONE)
    {
        fprintf(stderr, "%s: %s sqlite: 操作失败！ \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    sqlite3_finalize(stmt);

    //======初始化TCP连接======

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if((getaddrinfo(NULL, port, &hints, &result)) != 0)
    {
        fprintf(stderr, "%s: %s 调用getaddrinfo()失败, 程序中断 \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    if((loc_fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol)) == -1)
    {
        fprintf(stderr, "%s: %s socket创建失败, 程序中断 \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }
    if(setsockopt(loc_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
    {
        fprintf(stderr, "%s: %s 不能允许socket重新绑定(SO_REUSEADDR), 程序中断 \n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    //绑定socket
    if((bind(loc_fd, result->ai_addr, result->ai_addrlen)) == -1)
    {
        if(atoi(port) < PRIVILEGED_PORT)
            fprintf(stderr, "%s: %s 绑定socket失败，权限不足 \n", SERVER_NAME, ERROR_MSG);
        else
            fprintf(stderr, "%s: %s 绑定socket失败，请检查当前端口是否被占用 \n", SERVER_NAME, ERROR_MSG);

        // Exit on failure
        exit(-1);
    }
    freeaddrinfo(result);
    listen(loc_fd, queue_length);//设置socket为listen模式

    //初始化一个线程池
    thpool = thpool_init(num_threads);
    pthread_create(&net_thread, NULL, &tcp_listen, NULL);

    fprintf(stdout, "%s: %s 服务器初始化成功 配置信息如下： [PID: %d] [端口号: %s] [队列长度: %d] [线程数: %d]\n", SERVER_NAME, OK_MSG, getpid(), port, queue_length, num_threads);
	fprintf(stdout, "%s: %s 你可以通过输入'help' 获取帮助信息 \n", SERVER_NAME, INFO_MSG);
    fprintf(stdout, "%s: %s 你可以通过输入'stop' 或者使用快捷键 Ctrl+C 来停止运行 \n", SERVER_NAME, INFO_MSG);

    //======用户输入处理======

    while(1)
    {
        fgets(command, sizeof(command), stdin);
        clean_string((char *)&command);
        if(strcmp(command, "clear") == 0)
            system("clear");
        else if(strcmp(command, "help") == 0)
            console_help();
        else if(strcmp(command, "stat") == 0)
            print_stats();
        else if(strcmp(command, "stop") == 0)
            break;
        else
            fprintf(stderr, "%s: %s 命令'%s'未知, 输入'help'获取帮助 \n", SERVER_NAME, ERROR_MSG, command);
    }
    kill(getpid(), SIGINT);
}

void clean_string(char *str)
{
	int i = 0;
	int index = 0;
	char buffer[1024];
	for(i = 0; i < strlen(str); i++)
	{
		if(str[i] != '\b' && str[i] != '\n' && str[i] != '\r')
			buffer[index++] = str[i];
	}
	memset(str, 0, sizeof(str));
	buffer[index] = '\0';
	strcpy(str, buffer);
}

int client_count(int change)
{
	c_count += change;
	return c_count;
}

void console_help()
{
	fprintf(stdout, "%s 帮助:\n", SERVER_NAME);
	fprintf(stdout, "\tclear - 清除终端信息\n");
	fprintf(stdout, "\t help - 获取帮助信息\n");
	fprintf(stdout, "\t stat - 获取当前状态\n");
	fprintf(stdout, "\t stop - 停止服务器\n");
}

void *get_in_addr(struct sockaddr *sa)
{
        if (sa->sa_family == AF_INET)
                return &(((struct sockaddr_in*)sa)->sin_addr);
        else
                return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int recv_msg(int fd, char *message)
{
	int b_received = 0;
	int b_total = 0;
	char buffer[1024];
	memset(buffer, '\0', sizeof(buffer));

	b_received = recv(fd, buffer, sizeof(buffer), 0);
	b_total += b_received;
	strcpy(message, buffer);
	return b_total;
}

int send_msg(int fd, char *message)
{
	return send(fd, message, strlen(message), 0);
}

int validate_int(char *string)
{
	int isInt = 1;
	int j = 0;
        for(j = 0; j < strlen(string); j++)
        {
        	if(isInt == 1)
	        {
           		if(!isdigit(string[j]))
                	isInt = 0;
            }
        }
	return isInt;
}

void print_stats()
{
    //打印运行时间
    int hours, minutes, seconds;
    char runtime[32] = { '\0' };
    char tpusage[32] = { '\0' };
    seconds = (int)difftime(time(NULL), start_time);
    minutes = seconds / 60;
    hours = minutes / 60;
    minutes = minutes % 60;
    seconds = seconds % 60;
    sprintf(runtime, "%02d:%02d:%02d", hours, minutes, seconds);

    //打印连接池状态

    //连接池容量绰绰有余时
    if(client_count(0) < (num_threads * TP_UTIL))
    {
        fprintf(stdout, "%s: %s ", SERVER_NAME, OK_MSG);
        sprintf(tpusage, "[在线用户数: %d/%d]", client_count(0), num_threads);
    }
    // 连接池快满了或者已经饱和时
    else if(((double)client_count(0) >= ((double)num_threads * TP_UTIL)) && client_count(0) <= num_threads)
    {
        //转为警告
        fprintf(stdout, "%s: %s ", SERVER_NAME, WARN_MSG);
        sprintf(tpusage, "\033[1;33m[在线用户数: %d/%d]\033[0m", client_count(0), num_threads);
    }
    // 连接池已经超负荷时
    else
    {
        // 转为错误
        fprintf(stdout, "%s: %s ", SERVER_NAME, ERROR_MSG);
        sprintf(tpusage, "\033[1;31m[在线用户数: %d/%d]\033[0m", client_count(0), num_threads);
    }
    fprintf(stdout, "服务器运行中： [PID: %d] [运行时间: %s] [运行端口: %s] [queue: %d] %s\n", getpid(), runtime, port, queue_length, tpusage);
}


// 当产生SIGUSR1/SIGUSR2信号时，向客户端报告服务器状态
void stat_handler()
{
    freopen(term, "w", stdout);

    // 打印服务器状态
    print_stats();

    // Return stdout to /dev/null
    freopen("/dev/null", "w", stdout);
}

void shutdown_handler()
{
    // 关闭net_thread，停止接收新的请求
    pthread_cancel(net_thread);
    fprintf(stdout, "\n");

    // 关闭SQLite数据库
    if(sqlite3_close(db) != SQLITE_OK)
    {
        // 失败时
        fprintf(stderr, "%s: %s sqlite: 未能关闭SQLite数据库.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    // 尝试从容关闭socket
    if(shutdown(loc_fd, 2) == -1)
    {
        // 失败时
        fprintf(stderr, "%s: %s 未能成功shutdown本机的socket.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    // 尝试暴力关闭socket
    if(close(loc_fd) == -1)
    {
        // 失败时
        fprintf(stderr, "%s: %s 未能成功close本机的socket.\n", SERVER_NAME, ERROR_MSG);
        exit(-1);
    }

    // 关闭所有创建的连接池
    thpool_destroy(thpool);

    fprintf(stdout, "%s: %s 成功剔除 %d 台客户端设备，服务器中断。\n", SERVER_NAME, OK_MSG, client_count(0));

    exit(0);
}

void *p2p(void *args)
{
	char in[512],out[512] = { '\0' };
	p2p_t params = *((p2p_t *)(args));
	char *filename, *filehash, *filesize;
	long int f_size = 0;
	char peeraddr[128] = { '\0' };
	strcpy(peeraddr, params.ipaddr);
	int user_fd = params.fd;
	char query[256];
	int status;
	int flag=0;
	sqlite3_stmt *stmt;

	sprintf(out, "%s: %s \n", SERVER_NAME, USER_MSG);
	send_msg(user_fd, out);

	// 等待客户端发来消息
	//fix: 剔除长时间挂机用户
	while((strcmp(in, "CONNECT")) != 0 && (strcmp(in, "QUIT") != 0))
	{
		//获取消息
		recv_msg(user_fd, (char *)&in);
		clean_string((char *)&in);

		//如果发来的是握手消息CONNECT，返回确认信息ACCEPT
		if(strcmp(in, "CONNECT") == 0)
		{
			fprintf(stdout, "%s: %s 检测到 %s 向服务器发送了一个握手消息，返回确认消息 [句柄: %d]\n", SERVER_NAME, OK_MSG,peeraddr, user_fd);
			sprintf(out, "ACCEPT\n");
			send_msg(user_fd, out);
		}
	}

	//服务端已经发送确认信息，等待客户端发来进一步的消息
	while(strcmp(in, "QUIT") != 0)
	{
		memset(in, 0, sizeof(in));
		memset(out, 0, sizeof(out));
		memset(query, 0, sizeof(query));

		//获取消息
		recv_msg(user_fd, (char *)&in);
		clean_string((char *)&in);

		// 格式: ADD <文件名> <Hash值> <文件大小>
		if(strncmp(in, "ADD", 3) == 0)
		{
			strtok(in, " ");
			filename = strtok(NULL, " ");
			flag=0;

			if(filename != NULL)
			{
				filehash = strtok(NULL, " ");
				if(filehash != NULL)
				{
					filesize = strtok(NULL, " ");
					if((filesize != NULL) && (validate_int(filesize) == 1))
					{
						f_size = atoi(filesize);
						sprintf(query, "INSERT INTO files VALUES('%s', '%s', '%ld', '%s')", filename, filehash, f_size, peeraddr);
						sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
						if((status = sqlite3_step(stmt)) != SQLITE_DONE)
						{
							if(status == SQLITE_CONSTRAINT)
							{
								fprintf(stderr, "%s: %s sqlite: 添加文件失败，服务器数据库中已经存在当前文件\n", SERVER_NAME, ERROR_MSG);
								sprintf(out, "ERROR 添加文件失败，服务器数据库中已经存在当前文件\n");
								send_msg(user_fd, out);
							}
							else
							{
								fprintf(stderr, "%s: %s sqlite: 添加文件失败 \n", SERVER_NAME, ERROR_MSG);
								sprintf(out, "ERROR 添加文件信息到数据库失败，原因未知\n");
								send_msg(user_fd, out);
							}
						}
						sqlite3_finalize(stmt);

						if(status == SQLITE_DONE)
						{
							fprintf(stdout, "%s: %s  客户端%s 向服务器添加了文件 %20s [hash值: %20s] [大小: %10ld]\n", SERVER_NAME, INFO_MSG, peeraddr, filename, filehash, f_size);

							//返回OK
							sprintf(out, "OK\n");
							send_msg(user_fd, out);
						}
					}
					else
						flag=1;
				}
				else
					flag=1;
			}
			else
				flag=1;

			//传入参数的格式错误
			if(flag)
			{
				fprintf(stderr, "%s: %s 添加文件失败，传入参数的格式错误 \n", SERVER_NAME, ERROR_MSG);
				sprintf(out, "ERROR 添加文件失败，传入参数的格式错误\n");
				send_msg(user_fd, out);
			}

		}

		// 格式: DELETE [文件名] [HASH值]
		else if(strncmp(in, "DELETE", 6) == 0)
		{
			strtok(in, " ");
			filename = strtok(NULL, " ");
			flag=0;

			if(filename != NULL)
			{
				filehash = strtok(NULL, " ");
				if(filehash != NULL)
				{
					sprintf(query, "DELETE FROM files WHERE file='%s' AND hash='%s' AND peer='%s'", filename, filehash, peeraddr);
					sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
					if(sqlite3_step(stmt) != SQLITE_DONE)
					{
						fprintf(stderr, "%s: %s sqlite: 删除文件失败 \n", SERVER_NAME, ERROR_MSG);
						sprintf(out, "ERROR 从数据库中删除文件失败，原因未知 \n");
						send_msg(user_fd, out);
					}
					sqlite3_finalize(stmt);

					fprintf(stdout, "%s: %s 客户端%s 向服务器删除了文件 '%s'('%s') \n", SERVER_NAME, OK_MSG, peeraddr, filename, filehash);
					sprintf(out, "OK\n");
					send_msg(user_fd, out);
				}
				else
					flag=1;
			}
			else
				flag=1;
			//传入参数的格式错误
			if(flag)
			{
				fprintf(stderr, "%s: %s 删除文件失败，传入参数的格式错误 \n", SERVER_NAME, ERROR_MSG);
				sprintf(out, "ERROR 删除文件失败，传入参数的格式错误\n");
				send_msg(user_fd, out);
			}
		}

		// LIST
		else if(strcmp(in, "LIST") == 0)
		{
			sprintf(query, "SELECT DISTINCT file,size,peer FROM files ORDER BY file ASC");
			sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
			while((status = sqlite3_step(stmt)) != SQLITE_DONE)
			{
				if(status == SQLITE_ERROR)
				{
					fprintf(stderr, "%s: %s sqlite: 未能获得所有记录，数据库错误 \n", SERVER_NAME, ERROR_MSG);
					sprintf(out, "ERROR 未能获得所有记录，服务端数据库错误 \n");
					send_msg(user_fd, out);
				}
				else if(strcmp(peeraddr,(char *) sqlite3_column_text(stmt, 2)))
				{
					sprintf(out, "%s %d\n", sqlite3_column_text(stmt, 0), sqlite3_column_int(stmt, 1));
					send_msg(user_fd, out);
				}
			}
		sqlite3_finalize(stmt);
		sprintf(out, "OK\n");
		send_msg(user_fd, out);
		}

		// QUIT
		else if(strcmp(in, "QUIT") == 0)
		{
			continue;
		}

		// syntax: REQUEST [文件名]
		else if(strncmp(in, "REQUEST", 7) == 0)
		{
			strtok(in, " ");
			filename = strtok(NULL, " ");
			if(filename != NULL)
			{
				sprintf(query, "SELECT peer,size FROM files WHERE file='%s' ORDER BY peer ASC", filename);
				sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
				while((status = sqlite3_step(stmt)) != SQLITE_DONE)
				{
					if(status == SQLITE_ERROR)
					{
						fprintf(stderr, "%s: %s sqlite: 未能成功获取文件信息，数据库错误 '%s'\n", SERVER_NAME, ERROR_MSG, filename);
						sprintf(out, "ERROR 未能成功获取文件信息，数据库错误\n");
						send_msg(user_fd, out);
					}
					else
					{
						sprintf(out, "%s %ld\n", sqlite3_column_text(stmt, 0), (long int)sqlite3_column_int(stmt, 1));
						send_msg(user_fd, out);
					}
				}
				sqlite3_finalize(stmt);

				sprintf(out, "OK\n");
				send_msg(user_fd, out);
			}
			else
			{
				sprintf(out, "ERROR 没能成功获得请求的文件名 \n");
				send_msg(user_fd, out);
			}
		}
		else
		{
			sprintf(out, "ERROR 参数错误\n");
			send_msg(user_fd, out);
		}
	}

	memset(out, 0, sizeof(out));

	sprintf(out, "GOODBYE\n");
	send_msg(user_fd, out);

	fprintf(stdout, "%s: %s 客户端 %s 已经从服务器注销登录 [在线用户数: %d/%d]\n", SERVER_NAME, OK_MSG, peeraddr, client_count(-1), NUM_THREADS);

	sprintf(query, "DELETE FROM files WHERE peer='%s'", peeraddr);
	sqlite3_prepare_v2(db, query, strlen(query) + 1, &stmt, NULL);
	if(sqlite3_step(stmt) != SQLITE_DONE)
	{
		fprintf(stderr, "%s: %s 客户端 %s 剔除失败 [句柄: %d]\n", SERVER_NAME, ERROR_MSG, peeraddr, user_fd);
		return (void *)-1;
	}
	sqlite3_finalize(stmt);

	if(close(user_fd) == -1)
	{
		fprintf(stderr, "%s: %s 关闭套接字失败 [句柄: %d]\n", SERVER_NAME, ERROR_MSG, user_fd);
		return (void *)-1;
	}

	return (void *)0;
}

//建立TCP连接
void *tcp_listen()
{
    p2p_t params;
    char out[512] = { '\0' };

    while(1)
    {
        if((inc_fd = accept(loc_fd, (struct sockaddr *)&inc_addr, &inc_len)) == -1)
        {
            fprintf(stderr, "%s: %s 未能成功接收连接 \n", SERVER_NAME, ERROR_MSG);
            return (void *)-1;
        }
        else
        {
            inet_ntop(inc_addr.ss_family, get_in_addr((struct sockaddr *)&inc_addr), clientaddr, sizeof(clientaddr));

            fprintf(stdout, "%s: %s 监测到 %s 正在尝试连接到服务器 [socket编号: %d] [在线用户数: %d/%d]\n", SERVER_NAME, INFO_MSG, clientaddr, inc_fd, client_count(1), num_threads);

            if(((double)client_count(0) >= ((double)num_threads * TP_UTIL)) && (client_count(0) <= num_threads))
            {
                if(client_count(0) == num_threads)
                    fprintf(stdout, "%s: %s 连接池资源耗尽 [在线用户数: %d/%d]\n", SERVER_NAME, WARN_MSG, client_count(0), num_threads);
                else
                    fprintf(stdout, "%s: %s 连接池资源即将耗尽 [在线用户数: %d/%d]\n", SERVER_NAME, WARN_MSG, client_count(0), num_threads);
            }
            else if((client_count(0)) > num_threads)
            {
                fprintf(stderr, "%s: %s 连接池资源耗尽，仍然有新用户尝试连接 [在线用户数: %d/%d]\n", SERVER_NAME, ERROR_MSG, client_count(0), num_threads);
                sprintf(out, "%s: %s 服务器负载过大 , 请稍后再试 \n", SERVER_NAME, USER_MSG);
                send_msg(inc_fd, out);
            }
            params.fd = inc_fd;
            strcpy(params.ipaddr, clientaddr);
            thpool_add_work(thpool, &p2p, (void*)&params);//添加到线程池
        }
    }
}
