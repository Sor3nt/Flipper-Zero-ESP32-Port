#pragma once
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <unistd.h>
#define lwip_socket socket
#define lwip_connect connect
#define lwip_recv recv
#define lwip_send send
#define lwip_close close
#define lwip_ioctl ioctl
#define lwip_sendto sendto
#define lwip_recvfrom recvfrom
#define lwip_listen listen
#define lwip_accept accept
int test_bind(int fd,const struct sockaddr *address,socklen_t size);
int test_setsockopt(int fd,int level,int option,const void *value,socklen_t size);
#define lwip_bind test_bind
#define lwip_setsockopt test_setsockopt
