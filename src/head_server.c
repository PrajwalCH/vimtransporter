#include "epoll_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define DBLOG(m) \
    printf("%s\n", m);

#define DEFAULT_PORT 2058
#define DEFAULT_BACKLOG 10
#define MAX_EVENTS 20
#define MAX_BUFFER_SIZE 20

void die_with_error(const char *msg);

struct FdCollection {
    int master_socketfd;
    int epollfd;
    int client_socketfd;
};

void
die_with_error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

bool
is_ping_msg(char *buffer)
{
    if (strlen(buffer) == 4 && strncmp(buffer, "PING", 4) == 0)
        return true;
    return false;
}


void
reply_client(int socketfd)
{
    char buffer[MAX_BUFFER_SIZE] = {0};
    ssize_t total_byte_read = 0;
    bool has_read_done = false;

    while (1) {
        total_byte_read = recv(socketfd, buffer, MAX_BUFFER_SIZE, 0);

        if (total_byte_read == -1) {
            // probably full data is received
            if (errno == EAGAIN) {
                has_read_done = true;
                break;
            }

            if (errno != EAGAIN) {
                has_read_done = false;
                break;
            }
        }
    }

    if (!has_read_done)
        return;

    // send PONG as a response, if we got PING msg
    bool has_ping_msg = is_ping_msg(buffer);
    if (has_ping_msg) {
        send(socketfd, "PONG", 5, 0);
        return;
    }

    // sene notok as a response msg for now, if it was not a ping msg
    send(socketfd, "NOOK", 5, 0);
    return;
}

bool
set_socket_nonblocking(int socketfd)
{
    int flags = fcntl(socketfd, F_GETFL, 0);
    if (flags == -1)
        return false;

    int status = fcntl(socketfd, F_SETFL, flags | O_NONBLOCK);
    if (status == -1)
        return false;

    return true;
}

// set respective client socket fd who is recently active/ready for un-active/hang up
static inline void
FdColl_mod_client_socketfd(struct FdCollection *fds_coll, int new_socketfd)
{
    fds_coll->client_socketfd = new_socketfd;
}

void
start_event_loop(struct FdCollection *fds_coll)
{
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));

    fds_coll->epollfd = epoll_create_instance();

    struct epoll_event pollevent, pollevents[MAX_EVENTS];
    // add master socket fd for monitoring
    pollevent.events = EPOLLIN|EPOLLET;
    epoll_ctl_add_fd(fds_coll->epollfd, fds_coll->master_socketfd, &pollevent);

    while(1) {
        DBLOG("Event: loop started...");
        int num_ready_sockfds = epoll_wait(fds_coll->epollfd, pollevents, MAX_EVENTS, -1);

        if (num_ready_sockfds == -1)
            die_with_error("epoll_wait");

        for (int n_fd = 0; n_fd < num_ready_sockfds; n_fd++) {

            // some error occured or maybe client just hangup/close the connection
            if ((pollevents[n_fd].events & (EPOLLERR | EPOLLRDHUP | EPOLLHUP)) ||
                (!(pollevents[n_fd].events & EPOLLIN))) {

                FdColl_mod_client_socketfd(fds_coll, pollevents[n_fd].data.fd);
                // remove the client from monitor list
                epoll_ctl_delete_fd(fds_coll->epollfd, fds_coll->client_socketfd, &pollevent);
                printf("Event: client '%i' hangup or some error occured\n", fds_coll->client_socketfd);
                continue;

            } else if (pollevents[n_fd].data.fd == fds_coll->master_socketfd) { // new client connection, just accept it
                DBLOG("Event: new client connection");
                socklen_t clientaddr_len = sizeof(struct sockaddr_in);
                fds_coll->client_socketfd = accept(fds_coll->master_socketfd,
                                                   (struct sockaddr *)&client_addr,
                                                   &clientaddr_len);

                if (fds_coll->client_socketfd == -1)
                    die_with_error("failed to accept a new socket connection");

                set_socket_nonblocking(fds_coll->client_socketfd);

                /*
                 * add READ and WRITE (edge triggered) events to newly accepted client socket
                 * to monitor and handle the events later for I/O
                 *
                 * Quick note on EDGE TRIGGERED event
                 *
                 * an event will be fired only, when data will change from one state to another
                 * for ex: 0 to 1, 1 to 2 and so on
                 *
                 * */
                pollevent.events = EPOLLIN|EPOLLRDHUP|EPOLLET;

                epoll_ctl_add_fd(fds_coll->epollfd,
                                 fds_coll->client_socketfd,
                                 &pollevent);
            } else if (pollevents[n_fd].events & EPOLLIN) { // got a new read event, just read the data and response back to client
                FdColl_mod_client_socketfd(fds_coll, pollevents[n_fd].data.fd);
                printf("Event: client '%i' send some data\n", fds_coll->client_socketfd);
                reply_client(fds_coll->client_socketfd);
            }
        }
    }
}

void
socket_bind_a_name(int socketfd)
{
    // internet address setup
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_in));
    server_addr.sin_family = AF_INET; // it should be same as when socket was created
    server_addr.sin_port = htons(DEFAULT_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // let system decide the ip address

    if (bind(socketfd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) == -1)
        die_with_error("failed to bind the address");

}

int
socket_create_endpoint()
{
    int socketfd = socket(AF_INET, SOCK_STREAM, 0);

    if (socketfd == -1)
        die_with_error("failed to create new socket");
    return socketfd;
}

void
start_head_server(void)
{
    // different file descriptors collection
    // for easy access on later
    struct FdCollection fds_coll;

    // create new socket
    fds_coll.master_socketfd = socket_create_endpoint();
    socket_bind_a_name(fds_coll.master_socketfd);

    if (!set_socket_nonblocking(fds_coll.master_socketfd))
        die_with_error("failed to set socket as non-blocking");

    if (listen(fds_coll.master_socketfd, DEFAULT_BACKLOG) == -1)
        die_with_error("failed to listen the socket connection");

    start_event_loop(&fds_coll);
    close(fds_coll.master_socketfd);
}

int
main(void)
{
    start_head_server();
}