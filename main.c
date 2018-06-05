#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <resolv.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include "sblist.h"
#ifdef USE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define BUF_SIZE 16384

#define READ  0
#define WRITE 1

#define SERVER_SOCKET_ERROR -1
#define SERVER_SETSOCKOPT_ERROR -2
#define SERVER_BIND_ERROR -3
#define SERVER_LISTEN_ERROR -4
#define CLIENT_SOCKET_ERROR -5
#define CLIENT_RESOLVE_ERROR -6
#define CLIENT_CONNECT_ERROR -7
#define CREATE_PIPE_ERROR -8
#define BROKEN_PIPE_ERROR -9
#define SYNTAX_ERROR -10

union sockaddr_union {
        struct sockaddr_in  v4;
        struct sockaddr_in6 v6;
};

struct client {
        union sockaddr_union addr;
        int fd;
};

struct thread {
        pthread_t pt;
        struct client client;
        int remote_sock;
        volatile int done;
};

typedef enum {TRUE = 1, FALSE = 0} bool;

int create_socket(int port);
void sigchld_handler(int signal);
void server_loop();
int create_connection();
int parse_options(int argc, char *argv[]);
void plog(int priority, const char *format, ...);
static void* clientthread(void *data);
int execcmd(int *writefd, int *readfd, char *cmd);
int server_sock, remote_port = 0;
int connections_processed = 0;
char *bind_addr, *remote_host, *cmd_in, *cmd_out;
bool foreground = FALSE;
bool use_syslog = FALSE;

int main(int argc, char *argv[]) {
        int local_port;
        pid_t pid;

        bind_addr = NULL;

        local_port = parse_options(argc, argv);

        if (local_port < 0) {
                printf("Syntax: %s [-b bind_address] -l local_port -h remote_host -p remote_port [-i \"input parser\"] [-o \"output parser\"] [-f (stay in foreground)] [-s (use syslog)]\n", argv[0]);
                return local_port;
        }

        if (use_syslog)
                openlog("proxy", LOG_PID, LOG_DAEMON);

        if ((server_sock = create_socket(local_port)) < 0) { // start server
                plog(LOG_CRIT, "Cannot run server: %m");
                return server_sock;
        }

        signal(SIGCHLD, sigchld_handler);

        if (foreground) {
                server_loop();
        } else {
                switch(pid = fork()) {
                case 0: // deamonized child
                        server_loop();
                        break;
                case -1: // error
                        plog(LOG_CRIT, "Cannot daemonize: %m");
                        return pid;
                default: // parent
                        close(server_sock);
                }
        }

        if (use_syslog)
                closelog();

        return EXIT_SUCCESS;
}

/* Parse command line options */
int parse_options(int argc, char *argv[]) {
        int c, local_port = 0;

        while ((c = getopt(argc, argv, "b:l:h:p:i:o:fs")) != -1) {
                switch(c) {
                case 'l':
                        local_port = atoi(optarg);
                        break;
                case 'b':
                        bind_addr = optarg;
                        break;
                case 'h':
                        remote_host = optarg;
                        break;
                case 'p':
                        remote_port = atoi(optarg);
                        break;
                case 'i':
                        cmd_in = optarg;
                        break;
                case 'o':
                        cmd_out = optarg;
                        break;
                case 'f':
                        foreground = TRUE;
                        break;
                case 's':
                        use_syslog = TRUE;
                        break;
                }
        }

        if (local_port && remote_host && remote_port) {
                return local_port;
        } else {
                return SYNTAX_ERROR;
        }
}

/* Create server socket */
int create_socket(int port) {
        int server_sock, optval = 1;
        struct sockaddr_in server_addr;

        if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                return SERVER_SOCKET_ERROR;
        }

        if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
                return SERVER_SETSOCKOPT_ERROR;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        if (bind_addr == NULL) {
                server_addr.sin_addr.s_addr = INADDR_ANY;
        } else {
                server_addr.sin_addr.s_addr = inet_addr(bind_addr);
        }

        if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
                return SERVER_BIND_ERROR;
        }

        if (listen(server_sock, 20) < 0) {
                return SERVER_LISTEN_ERROR;
        }

        return server_sock;
}

/* Send log message to stderr or syslog */
void plog(int priority, const char *format, ...)
{
        va_list ap;

        va_start(ap, format);

        if (use_syslog)
                vsyslog(priority, format, ap);
        else {
                vfprintf(stderr, format, ap);
                fprintf(stderr, "\n");
        }

        va_end(ap);
}

/* Update systemd status with connection count */
void update_connection_count()
{
#ifdef USE_SYSTEMD
        sd_notifyf(0, "STATUS=Ready. %d connections processed.\n", connections_processed);
#endif
}

/* 处理child process wait */
/* 这是一个比经典的处理方法 */
void sigchld_handler(int signal) {
        while (waitpid(-1, NULL, WNOHANG) > 0);
}

/* 用于system来执行命令，同时设置pipe_in, pipe_out */
int execcmd(int *writefd, int *readfd, char *cmd) {
        int pipe_in[2], pipe_out[2];
        if(pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
                return 0;
        }
        if(fork() == 0) {
                dup2(pipe_in[READ], STDIN_FILENO);
                dup2(pipe_out[WRITE], STDOUT_FILENO);
                close(pipe_in[WRITE]);
                close(pipe_out[READ]);
                int n = system(cmd);
                exit(n);
        } else {
                *writefd = pipe_in[WRITE];
                *readfd = pipe_out[READ];
                close(pipe_in[READ]);
                close(pipe_out[WRITE]);
        }
        return 1;
}

/* thread 函数 */
static void* clientthread(void *data)
{
        struct thread *t = data;
        int maxfd, client_sock, remote_sock, cmdin_writefd, cmdin_readfd, cmdout_writefd, cmdout_readfd;
        fd_set fdsc, fds;

        if ((remote_sock = create_connection()) < 0) {
                plog(LOG_ERR, "Cannot connect to host: %m");
                goto cleanup;
        }

        t->remote_sock = remote_sock;
        client_sock = t->client.fd;

        maxfd = client_sock > remote_sock ? client_sock : remote_sock;

        /* fdsc 用于保存需要监视的文件 */
        FD_ZERO(&fdsc);
        FD_SET(client_sock, &fdsc);
        FD_SET(remote_sock, &fdsc);

        /* 利用pipe来process间通信*/
        if(cmd_in && 0 == execcmd(&cmdin_writefd, &cmdin_readfd, cmd_in)) {
                plog(LOG_CRIT, "Cannot create pipe: %m");
                exit(CREATE_PIPE_ERROR);
        }

        if(cmd_out && 0 == execcmd(&cmdout_writefd, &cmdout_readfd, cmd_out)) {
                plog(LOG_CRIT, "Cannot create pipe: %m");
                exit(CREATE_PIPE_ERROR);
        }

        while(1) {
                /* 每一次初始化一下 fds, 因为select会修改这个值 */
                memcpy(&fds, &fdsc, sizeof(fds));

                struct timeval timeout = {.tv_sec = 60*3, .tv_usec = 0};
                switch (select(maxfd+1, &fds, 0, 0, &timeout)) {
                case 0: {
                        goto cleanup;
                        break;
                }
                case -1:
                        if(errno == EINTR)
                                continue;
                        else
                                plog(LOG_ERR, "select: %s",strerror(errno));
                        goto cleanup;
                }

                int infd, outfd, pipe_in, pipe_out;
                char *cmd = NULL;

                if(FD_ISSET(client_sock, &fds)) {
                        infd = client_sock;
                        outfd = remote_sock;
                        if(cmd_out) {
                                cmd = cmd_out;
                                pipe_in = cmdout_writefd;
                                pipe_out = cmdout_readfd;
                        }

                } else {
                        outfd = client_sock;
                        infd = remote_sock;
                        if(cmd_in) {
                                cmd = cmd_in;
                                pipe_in = cmdin_writefd;
                                pipe_out = cmdin_readfd;
                        }
                }

                char buf[BUF_SIZE];
                ssize_t sent = 0, n = read(infd, buf, sizeof buf);
                /*　有一个socket断掉了 */
                /* 这个很重要，如果read 0 说明对方链接关掉了 */
                if(n <= 0) {
                        goto cleanup;
                }

                if(cmd) {
                        while(sent < n) {
                                ssize_t m = write(pipe_in, buf+sent, n-sent);
                                if(m < 0) goto cleanup;
                                sent += m;
                        }

                        sent = 0, n = read(pipe_out, buf, sizeof buf);
                }

                while(sent < n) {
                        ssize_t m = write(outfd, buf+sent, n-sent);
                        if(m < 0) goto cleanup;
                        sent += m;
                }
        }
cleanup:
        if(remote_sock > 0)
                close(remote_sock);
        if(client_sock > 0)
                close(client_sock);
        t->done = 1;
        return 0;
}

static void collect(sblist *threads) {
        size_t i;
        for(i=0;i<sblist_getsize(threads);) {
                struct thread* thread = *((struct thread**)sblist_get(threads, i));
                if(thread->done) {
                        pthread_join(thread->pt, 0);
                        sblist_delete(threads, i);
                        free(thread);
                } else
                        i++;
        }
}

/* 监听和accept循环 */
void server_loop() {
        int client_sock;
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        sblist *threads = sblist_new(sizeof (struct thread*), 8);

#ifdef USE_SYSTEMD
        sd_notify(0, "READY=1\n");
#endif
        size_t stacksz = 512 * 1024;

        while(TRUE) {
                collect(threads);
                struct thread *curr = malloc(sizeof (struct thread));
                if(!curr) goto oom;
                curr->done = 0;
                client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addrlen);
                if(client_sock < 0) {
                        free(curr);
                        plog(LOG_ERR, "connect %m\n");
                        continue;
                }

                curr->client.fd = client_sock;
                curr->client.addr.v4 = client_addr;
                if(!sblist_add(threads, &curr)) {
                        close(curr->client.fd);
                        free(curr);
                        oom:
                        plog(LOG_ERR, "rejecting connection due to OOM\n");
                        usleep(16); /* prevent 100% CPU usage in OOM situation */
                        continue;
                }

                pthread_attr_t *a = 0, attr;
                if(pthread_attr_init(&attr) == 0) {
                        a = &attr;
                        pthread_attr_setstacksize(a, stacksz);
                }
                if(pthread_create(&curr->pt, a, clientthread, curr) != 0)
                        plog(LOG_ERR, "pthread_create failed. OOM?\n");
                if(a) pthread_attr_destroy(&attr);
        }
}

/* 创建一个链接到remote的socket */
int create_connection() {
        struct sockaddr_in server_addr;
        struct hostent *server;
        int sock;

        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
                return CLIENT_SOCKET_ERROR;
        }

        if ((server = gethostbyname(remote_host)) == NULL) {
                errno = EFAULT;
                return CLIENT_RESOLVE_ERROR;
        }

        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
        server_addr.sin_port = htons(remote_port);

        if (connect(sock, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
                return CLIENT_CONNECT_ERROR;
        }

        return sock;
}
