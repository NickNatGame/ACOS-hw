#include "tcp_philosophers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

long long tcp_now(void)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) == -1)
    return -1;
  return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

int tcp_address(const char *ip, const char *port, struct sockaddr_in *address)
{
  char *end;
  errno = 0;
  long number = strtol(port, &end, 10);
  if (*port < '0' || *port > '9' || *end || errno || number < 1 || number > 65535)
    return -1;

  memset(address, 0, sizeof(*address));
  address->sin_family = AF_INET;
  address->sin_port = htons((uint16_t)number);
  if (inet_pton(AF_INET, ip, &address->sin_addr) == 1)
    return 0;
  return -1;
}

int tcp_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int tcp_send(int fd, int type, int id, int value, int count, int cycles)
{
  uint32_t words[] = {1, (uint32_t)type, (uint32_t)id, (uint32_t)value,
                      (uint32_t)count, (uint32_t)cycles};
  for (int i = 0; i < 6; i++)
    words[i] = htonl(words[i]);

  size_t sent = 0;
  long long start = tcp_now();
  if (start < 0)
    return -1;
  while (sent < sizeof(words))
  {
    long long now = tcp_now();
    if (now < 0 || now - start >= 100)
      return -1;
    ssize_t written = send(fd, (unsigned char *)words + sent, sizeof(words) - sent,
                           MSG_NOSIGNAL | MSG_DONTWAIT);
    if (written > 0)
      sent += (size_t)written;
    else if (written < 0 && errno == EINTR)
      continue;
    else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      struct pollfd socket = {.fd = fd, .events = POLLOUT};
      if (poll(&socket, 1, (int)(100 - (now - start))) < 0 && errno != EINTR)
        return -1;
    }
    else
      return -1;
  }
  return 0;
}

int tcp_receive(TcpPeer *peer, TcpMessage *message)
{
  ssize_t received = recv(peer->fd, peer->input + peer->used,
                          sizeof(peer->input) - peer->used, MSG_DONTWAIT);
  if (received == 0)
    return -1;
  if (received < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return 0;
    return -1;
  }
  peer->used += (size_t)received;
  if (peer->used != sizeof(peer->input))
    return 0;

  uint32_t words[6];
  memcpy(words, peer->input, sizeof(words));
  for (int i = 0; i < 6; i++)
    words[i] = ntohl(words[i]);

  peer->used = 0;
  *message = (TcpMessage){words[0], words[1], words[2], words[3], words[4], words[5]};
  
  int zero_id_allowed = message->type == TCP_OBSERVER_JOIN ||
                        message->type == TCP_CONFIG || message->type == TCP_STOP;
  if (message->version != 1 || message->type < TCP_JOIN || message->type > TCP_SNAPSHOT ||
      (message->id < 1 && !(message->id == 0 && zero_id_allowed)) ||
      message->id > MAX_PHILOSOPHERS || message->value > INT_MAX ||
      message->count > MAX_PHILOSOPHERS || message->cycles > INT_MAX)
    return -1;
  return 1;
}
