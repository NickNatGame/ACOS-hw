#include "tcp_philosophers.h"
#include "console.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

static int wait_message(TcpPeer *peer, TcpMessage *message)
{
  while (!received_signal() && !output_failed())
  {
    struct pollfd socket = {.fd = peer->fd, .events = POLLIN};
    int ready = poll(&socket, 1, 100);
    if (ready < 0 && errno != EINTR) return -1;
    if (ready > 0)
    {
      int result = tcp_receive(peer, message);
      if (result != 0) return result;
    }
  }
  return -1;
}

static int connect_to_server(int fd, const struct sockaddr_in *address)
{
  if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0) return 0;
  if (errno != EINPROGRESS) return -1;
  struct pollfd socket = {.fd = fd, .events = POLLOUT};
  if (poll(&socket, 1, 5000) <= 0) return -1;
  int error;
  socklen_t length = sizeof(error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == -1 || error) return -1;
  return 0;
}

static void print_snapshot(const int *states, const int *forks, int count)
{
  const char *names[] = {"ожидает", "готов", "думает", "голоден", "ест", "завершил"};
  print_log("Наблюдатель: ");
  for (int i = 0; i < count; i++) print_log("P%d=%s ", i + 1, names[states[i]]);
  print_log("| вилки: ");
  for (int i = 0; i < count; i++)
    if (forks[i]) print_log("F%d=P%d ", i + 1, forks[i]);
    else print_log("F%d=свободна ", i + 1);
  print_log("\n");
}

int run_tcp_observer(const struct sockaddr_in *address)
{
  TcpPeer peer = {.fd = socket(AF_INET, SOCK_STREAM, 0)};
  if (peer.fd < 0 || tcp_nonblocking(peer.fd) < 0 || connect_to_server(peer.fd, address) < 0)
  {
    if (peer.fd >= 0) close(peer.fd);
    dprintf(STDERR_FILENO, "Наблюдатель: не удалось подключиться к серверу.\n");
    return 1;
  }
  if (tcp_send(peer.fd, TCP_OBSERVER_JOIN, 0, 0, 0, 0) < 0) { close(peer.fd); return 1; }

  int states[MAX_PHILOSOPHERS] = {0}, forks[MAX_PHILOSOPHERS] = {0};
  int count = 0, received = 0, result = 1;
  while (1)
  {
    TcpMessage message;
    if (wait_message(&peer, &message) != 1) break;
    if (message.type == TCP_STOP && message.id == 0) { result = (int)message.value; break; }
    if (message.type == TCP_CONFIG && message.id == 0 && !message.value &&
        message.count >= 2 && message.count <= MAX_PHILOSOPHERS && message.cycles)
    {
      count = (int)message.count;
      print_log("Наблюдатель: подключён, философов %d, циклов %u.\n", count, message.cycles);
      continue;
    }
    if (message.type != TCP_SNAPSHOT || !count || message.id > (unsigned)count ||
        message.value > 5 || message.count > (unsigned)count || message.cycles > (unsigned)count) break;
    int index = (int)message.id - 1;
    states[index] = (int)message.value;
    forks[index] = (int)message.count;
    forks[(index + 1) % count] = (int)message.cycles;
    received++;
    if (received == count) { print_snapshot(states, forks, count); received = 0; }
  }
  close(peer.fd);
  if (received_signal()) return 128 + received_signal();
  return result;
}
