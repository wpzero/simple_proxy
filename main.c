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

typedef enum {TRUE = 1, FALSE = 0} bool;

int create_socket(int port);
void sigchld_handler(int signal);
void sigterm_handler(int signal);
void server_loop();
void handle_client(int client_sock, struct sockaddr_in client_addr);
void handle_client2(int client_sock);
void forward_data(int source_sock, int destination_sock);
void forward_data_ext(int source_sock, int destination_sock, char *cmd);
int create_connection();
int parse_options(int argc, char *argv[]);
void plog(int priority, const char *format, ...);

int server_sock, client_sock, remote_sock, remote_port = 0;
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

        signal(SIGCHLD, sigchld_handler); // prevent ended children from becoming zombies
        signal(SIGTERM, sigterm_handler); // handle KILL signal

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

/* term CTRL+C signal */
void sigterm_handler(int signal) {
        close(client_sock);
        close(server_sock);
        exit(0);
}

/* Main server loop */
void server_loop() {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);

#ifdef USE_SYSTEMD
        sd_notify(0, "READY=1\n");
#endif

        while (TRUE) {
                update_connection_count();
                client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addrlen);
                if (fork() == 0) { // handle client connection in a separate process
                        close(server_sock);
                        handle_client2(client_sock);
                        /* handle_client(client_sock, client_addr); */
                        exit(0);
                } else
                        connections_processed++;
                close(client_sock);
        }

}

void handle_client2(int client_sock)
{
        int maxfd;
        fd_set fdsc, fds;

        if ((remote_sock = create_connection()) < 0) {
                plog(LOG_ERR, "Cannot connect to host: %m");
                goto cleanup;
        }
        maxfd = client_sock > remote_sock ? client_sock : remote_sock;

        /* fdsc 用于保存需要监视的文件 */
        FD_ZERO(&fdsc);
        FD_SET(client_sock, &fdsc);
        FD_SET(remote_sock, &fdsc);

        while(1) {
                /* 每一次初始化一下 fds, 因为select会修改这个值 */
                memcpy(&fds, &fdsc, sizeof(fds));

                struct timeval timeout = {.tv_sec = 60*15, .tv_usec = 0};
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

                int infd = FD_ISSET(client_sock, &fds) ? client_sock : remote_sock;
                int outfd = infd == client_sock ? remote_sock : client_sock;
                char buf[BUF_SIZE];
                ssize_t sent = 0, n = read(infd, buf, sizeof buf);
                while(sent < n) {
                        ssize_t m = write(outfd, buf+sent, n-sent);
                        if(m < 0) return;
                        sent += m;
                }
        }
cleanup:
        close(remote_sock);
        close(client_sock);
}

/* Handle client connection */
void handle_client(int client_sock, struct sockaddr_in client_addr)
{
        if ((remote_sock = create_connection()) < 0) {
                plog(LOG_ERR, "Cannot connect to host: %m");
                goto cleanup;
        }

        /* a process forwarding data from client to remote socket */
        if (fork() == 0) {
                if (cmd_out) {
                        forward_data_ext(client_sock, remote_sock, cmd_out);
                } else {
                        forward_data(client_sock, remote_sock);
                }
                exit(0);
        }

        /* a process forwarding from remote host to client socket */
        if (fork() == 0) {
                if (cmd_in) {
                        forward_data_ext(remote_sock, client_sock, cmd_in);
                } else {
                        forward_data(remote_sock, client_sock);
                }
                exit(0);
        }

cleanup:
        close(remote_sock);
        close(client_sock);
}

/* Forward data between sockets */
void forward_data(int source_sock, int destination_sock) {
        ssize_t n;

#ifdef USE_SPLICE
        int buf_pipe[2];

        if (pipe(buf_pipe) == -1) {
                plog(LOG_ERR, "pipe: %m");
                exit(CREATE_PIPE_ERROR);
        }

        while ((n = splice(source_sock, NULL, buf_pipe[WRITE], NULL, SSIZE_MAX, SPLICE_F_NONBLOCK|SPLICE_F_MOVE)) > 0) {
                if (splice(buf_pipe[READ], NULL, destination_sock, NULL, SSIZE_MAX, SPLICE_F_MOVE) < 0) {
                        plog(LOG_ERR, "write: %m");
                        exit(BROKEN_PIPE_ERROR);
                }
        }
#else
        char buffer[BUF_SIZE];

        while ((n = recv(source_sock, buffer, BUF_SIZE, 0)) > 0) { // read data from input socket
                send(destination_sock, buffer, n, 0); // send data to output socket
        }
#endif

        if (n < 0) {
                plog(LOG_ERR, "read: %m");
                exit(BROKEN_PIPE_ERROR);
        }

#ifdef USE_SPLICE
        close(buf_pipe[0]);
        close(buf_pipe[1]);
#endif

        shutdown(destination_sock, SHUT_RDWR); // stop other processes from using socket
        close(destination_sock);

        shutdown(source_sock, SHUT_RDWR); // stop other processes from using socket
        close(source_sock);
}

/* Forward data between sockets through external command */
void forward_data_ext(int source_sock, int destination_sock, char *cmd) {
        char buffer[BUF_SIZE];
        int n, i, pipe_in[2], pipe_out[2];

        if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) { // create command input and output pipes
                plog(LOG_CRIT, "Cannot create pipe: %m");
                exit(CREATE_PIPE_ERROR);
        }

        if (fork() == 0) {
                dup2(pipe_in[READ], STDIN_FILENO); // replace standard input with input part of pipe_in
                dup2(pipe_out[WRITE], STDOUT_FILENO); // replace standard output with output part of pipe_out
                close(pipe_in[WRITE]); // close unused end of pipe_in
                close(pipe_out[READ]); // close unused end of pipe_out
                n = system(cmd); // execute command
                exit(n);
        } else {
                close(pipe_in[READ]); // no need to read from input pipe here
                close(pipe_out[WRITE]); // no need to write to output pipe here

                while ((n = recv(source_sock, buffer, BUF_SIZE, 0)) > 0) { // read data from input socket
                        if (write(pipe_in[WRITE], buffer, n) < 0) { // write data to input pipe of external command
                                plog(LOG_ERR, "Cannot write to pipe: %m");
                                exit(BROKEN_PIPE_ERROR);
                        }
                        if ((i = read(pipe_out[READ], buffer, BUF_SIZE)) > 0) { // read command output
                                send(destination_sock, buffer, i, 0); // send data to output socket
                        }
                }

                shutdown(destination_sock, SHUT_RDWR); // stop other processes from using socket
                close(destination_sock);

                shutdown(source_sock, SHUT_RDWR); // stop other processes from using socket
                close(source_sock);
        }
}

/* Create client connection */
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
