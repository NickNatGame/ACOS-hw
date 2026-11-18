#ifndef TCP_PHILOSOPHERS_H
#define TCP_PHILOSOPHERS_H

#include "philosophers.h"
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

enum
{
  TCP_JOIN = 1,
  TCP_CONFIG,
  TCP_START,
  TCP_HUNGRY,
  TCP_EAT,
  TCP_FINISH,
  TCP_CONTINUE,
  TCP_STOP,
  TCP_OBSERVER_JOIN,
  TCP_SNAPSHOT
};

/* Шесть uint32_t в сетевом порядке: версия, тип, ID (1..N), значение, N, циклы */
typedef struct {
  uint32_t version;
  uint32_t type;
  uint32_t id;
  uint32_t value;
  uint32_t count;
  uint32_t cycles;
} TcpMessage;

typedef struct {
  int fd;
  unsigned char input[24];
  size_t used;
} TcpPeer;

long long tcp_now(void);
int tcp_address(const char *ip, const char *port, struct sockaddr_in *address);
int tcp_nonblocking(int fd);
int tcp_send(int fd, int type, int id, int value, int count, int cycles);
/* 1 полное сообщение, 0 пока не полное, -1 обрыв/ошибка/неверная версия */
int tcp_receive(TcpPeer *peer, TcpMessage *message);
int run_tcp_server(const struct sockaddr_in *address, const Parameters *parameters);
int run_tcp_client(const struct sockaddr_in *address, int id);
int run_tcp_observer(const struct sockaddr_in *address);

#endif
