#include "tcp_philosophers.h"
#include "console.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum
{
  UNJOINED,
  READY,
  THINKING,
  HUNGRY,
  EATING,
  DONE,
  STOPPED
};

typedef struct
{
  TcpPeer peer;
  int id, state, completed;
  long long deadline, hungry_since;
} Client;

typedef struct
{
  Parameters p;
  Client clients[MAX_PHILOSOPHERS + 1];
  TcpPeer observers[32];
  int forks[MAX_PHILOSOPHERS], queue[MAX_PHILOSOPHERS];
  int head, size, joined, finished, started;
  long long start;
} Server;

static void send_snapshot(Server *s)
{
  for (int observer = 0; observer < 32; observer++)
  {
    if (s->observers[observer].fd < 0) continue;
    for (int i = 0; i < s->p.count; i++)
    {
      int right = (i + 1) % s->p.count;
      if (tcp_send(s->observers[observer].fd, TCP_SNAPSHOT, i + 1, s->clients[i].state,
                   s->forks[i], s->forks[right]) < 0)
      {
        close(s->observers[observer].fd);
        s->observers[observer].fd = -1;
        break;
      }
    }
  }
}

static void disconnect_client(Server *s, int slot)
{
  Client *c = &s->clients[slot];
  close(c->peer.fd);
  c->peer.fd = -1;
  c->peer.used = 0;
  if (!c->id) return;
  for (int i = 0; i < s->p.count; i++)
    if (s->forks[i] == c->id) s->forks[i] = 0;
  int queue[MAX_PHILOSOPHERS], size = 0;
  for (int i = 0; i < s->size; i++)
  {
    int entry = s->queue[(s->head + i) % s->p.count];
    if (entry != slot) queue[size++] = entry;
  }
  memcpy(s->queue, queue, (size_t)size * sizeof(queue[0]));
  s->head = 0;
  s->size = size;
  if (c->state != DONE) c->state = UNJOINED;
  print_log("Сервер: философ %d отключён; завершено циклов %d, вилки освобождены.\n", c->id, c->completed);
  send_snapshot(s);
}

static int duration(int minimum, int maximum)
{
  return minimum + (int)(rand() % ((long long)maximum - minimum + 1));
}

static int answer(Client *c, int type, int value)
{
  return tcp_send(c->peer.fd, type, c->id, value, 0, 0);
}

static int think(Server *s, Client *c, int type, long long now)
{
  int delay = duration(s->p.think_min, s->p.think_max);
  c->state = THINKING;
  c->deadline = now + delay;
  return answer(c, type, delay);
}

static int limit_reached(const Server *s, long long now);

static int give_forks(Server *s)
{
  while (s->size)
  {
    long long now = tcp_now();
    if (now < 0)
      return 1;
    if (limit_reached(s, now))
      return 2;

    Client *c = &s->clients[s->queue[s->head]];
    
    int left = c->id - 1, right = c->id % s->p.count;
    if (s->forks[left] || s->forks[right])
      break;

    s->head = (s->head + 1) % s->p.count;
    s->size--;
    s->forks[left] = s->forks[right] = c->id;
    c->state = EATING;
    int delay = duration(s->p.eat_min, s->p.eat_max);
    c->deadline = now + delay;

    print_log("Сервер: выдал философу %d вилки %d и %d, ожидание %lld ms, еда %d ms.\n",
              c->id, left + 1, right + 1, now - c->hungry_since, delay);
    if (answer(c, TCP_EAT, delay) < 0)
      disconnect_client(s, c->id - 1);
    send_snapshot(s);
  }
  return 0;
}

static int limit_reached(const Server *s, long long now)
{
  if (!s->started)
    return 0;
  if (s->p.time_limit && now - s->start >= s->p.time_limit)
    return 1;

  for (int i = 0; i < s->p.count; i++)
    if (s->clients[i].state == HUNGRY && now - s->clients[i].hungry_since > s->p.max_wait)
      return 1;
  return 0;
}

static int handle(Server *s, int slot, const TcpMessage *m, long long now)
{
  Client *c = &s->clients[slot];
  if (!c->id && m->type == TCP_OBSERVER_JOIN)
  {
    int observer = 0;
    while (observer < 32 && s->observers[observer].fd >= 0) observer++;
    if (m->id || m->value || m->count || m->cycles || observer == 32)
    {
      tcp_send(c->peer.fd, TCP_STOP, 0, 1, 0, 0);
      close(c->peer.fd);
      c->peer.fd = -1;
      return 0;
    }
    s->observers[observer] = c->peer;
    c->peer.fd = -1;
    if (tcp_send(s->observers[observer].fd, TCP_CONFIG, 0, 0, s->p.count, s->p.cycles) < 0)
    {
      close(s->observers[observer].fd);
      s->observers[observer].fd = -1;
      return 0;
    }
    print_log("Сервер: подключён наблюдатель.\n");
    send_snapshot(s);
    return 0;
  }
  if (!c->id)
  {
    int duplicate = 0;
    for (int i = 0; i < s->p.count; i++)
      duplicate |= s->clients[i].id == (int)m->id && s->clients[i].peer.fd >= 0;

    if (m->type != TCP_JOIN || m->id > (unsigned)s->p.count || m->value ||
        m->count || m->cycles || duplicate)
    {
      tcp_send(c->peer.fd, TCP_STOP, (int)m->id, 1, 0, 0);
      close(c->peer.fd);
      c->peer.fd = -1;
      print_log("Сервер: отклонено подключение с неверным или занятым ID.\n");
      return 0;
    }

    Client *target = &s->clients[m->id - 1];
    target->peer = c->peer;
    c->peer.fd = -1;
    c = target;
    if (!c->id) s->joined++;
    c->id = (int)m->id;
    c->state = READY;

    print_log("Сервер: подключён философ %d.\n", c->id);
    if (tcp_send(c->peer.fd, TCP_CONFIG, c->id, c->completed, s->p.count, s->p.cycles) < 0)
    {
      disconnect_client(s, c->id - 1);
      return 0;
    }
    if (s->started)
    {
      if (c->completed == s->p.cycles) c->state = DONE;
      else if (think(s, c, TCP_START, now) < 0) disconnect_client(s, c->id - 1);
      send_snapshot(s);
    }
    else if (s->joined == s->p.count)
    {
      s->started = 1;
      s->start = now;
      for (int i = 0; i < s->p.count; i++)
        if (s->clients[i].peer.fd >= 0 && think(s, &s->clients[i], TCP_START, now) < 0)
          disconnect_client(s, i);
      send_snapshot(s);
    }
    return 0;
  }

  if (m->id != (unsigned)c->id || m->value || m->count || m->cycles || !s->started)
    return 1;

  if (m->type == TCP_HUNGRY && c->state == THINKING && now >= c->deadline)
  {
    c->state = HUNGRY;
    c->hungry_since = now;
    s->queue[(s->head + s->size) % s->p.count] = slot;
    s->size++;
    print_log("Сервер: философ %d сообщил о голоде, запросил вилки %d и %d.\n",
              c->id, c->id, c->id % s->p.count + 1);
    send_snapshot(s);
    return give_forks(s);
  }

  if (m->type == TCP_FINISH && c->state == EATING && now >= c->deadline)
  {
    s->forks[c->id - 1] = s->forks[c->id % s->p.count] = 0;
    c->completed++;
    print_log("Сервер: философ %d сообщил об окончании еды; освободил вилки %d и %d; цикл %d/%d.\n",
              c->id, c->id, c->id % s->p.count + 1, c->completed, s->p.cycles);
    if (c->completed == s->p.cycles)
    {
      c->state = DONE;
      s->finished++;
    }
    else if (think(s, c, TCP_CONTINUE, now) < 0)
      disconnect_client(s, c->id - 1);
    send_snapshot(s);


    return give_forks(s);
  }
  return 1;
}

int run_tcp_server(const struct sockaddr_in *address, const Parameters *parameters)
{
  Server s = {.p = *parameters};
  for (int i = 0; i < 32; i++) s.observers[i].fd = -1;
  for (int i = 0; i <= parameters->count; i++)
    s.clients[i].peer.fd = -1;

  int listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0)
  {
    perror("socket");
    return 1;
  }

  int reuse = 1;
  if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0 ||
      tcp_nonblocking(listener) < 0 || bind(listener, (const struct sockaddr *)address, sizeof(*address)) < 0 ||
      listen(listener, parameters->count) < 0)
  {
    perror("TCP-сервер");
    close(listener);
    return 1;
  }

  srand((unsigned)parameters->seed);
  print_log("ГОТОВО: TCP %s:%u, ожидаем %d философов, FIFO.\n",
            inet_ntoa(address->sin_addr), ntohs(address->sin_port), parameters->count);

  long long waiting = tcp_now();
  int result = 0;
  const char *reason = "все философы выполнили заданные циклы";
  while (s.finished < parameters->count)
  {
    long long now = tcp_now();
    if (now < 0 || waiting < 0)
    {
      result = 1;
      reason = "ошибка часов";
      break;
    }

    if (received_signal() || output_failed())
    {
      if (received_signal())
        result = 128 + received_signal();
      else
        result = 1;
      reason = "прерывание или ошибка вывода сервера";
      break;
    }

    if (!s.started && now - waiting >= 60000)
    {
      result = 1;
      reason = "не все философы подключились за 60 секунд";
      break;
    }
    if (limit_reached(&s, now))
    {
      result = 2;
      reason = "достигнут лимит времени или ожидания вилок";
      break;
    }
    Client *pending = &s.clients[parameters->count];
    if (pending->peer.fd >= 0 && now >= pending->deadline)
      disconnect_client(&s, parameters->count);

    struct pollfd items[MAX_PHILOSOPHERS + 34] = {{.fd = listener, .events = POLLIN}};
    if (pending->peer.fd >= 0) items[0].events = 0;
    for (int i = 0; i < 32; i++)
      items[i + parameters->count + 2] = (struct pollfd){.fd = s.observers[i].fd, .events = POLLIN};
    for (int i = 0; i <= parameters->count; i++)
      items[i + 1] = (struct pollfd){.fd = s.clients[i].peer.fd, .events = POLLIN};
    
    if (poll(items, (nfds_t)parameters->count + 34, 10) < 0)
    {
      if (errno == EINTR)
        continue;
      result = 1;
      reason = "ошибка poll";
      break;
    }
    
    if (items[0].revents & POLLIN)
    {
      int fd = accept(listener, NULL, NULL);
      if (fd >= 0)
      {
        int slot = parameters->count;
        if (s.clients[slot].peer.fd >= 0 || tcp_nonblocking(fd) < 0)
          close(fd);
        else
          s.clients[slot] = (Client){.peer = {.fd = fd}, .deadline = now + 2000};
      }
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      {
        result = 1;
        reason = "ошибка accept";
        break;
      }
    }
    for (int i = 0; i < 32; i++)
    {
      if (!items[i + parameters->count + 2].revents || s.observers[i].fd < 0) continue;
      TcpMessage message;
      if (tcp_receive(&s.observers[i], &message) != 0)
      {
        close(s.observers[i].fd);
        s.observers[i].fd = -1;
      }
    }
    for (int i = 0; i <= parameters->count; i++)
    {
      if (!items[i + 1].revents || items[i + 1].fd != s.clients[i].peer.fd)
        continue;
      TcpMessage message;
      errno = 0;
      int received = tcp_receive(&s.clients[i].peer, &message);
      if (received < 0 && !s.clients[i].id)
      {
        close(s.clients[i].peer.fd);
        s.clients[i].peer.fd = -1;
        continue;
      }
      if (received < 0 && s.clients[i].id && errno != EPROTO)
      {
        disconnect_client(&s, i);
        result = give_forks(&s);
        if (result) break;
        continue;
      }
      now = tcp_now();
      if (now < 0 || received < 0)
        result = 1;
      else if (limit_reached(&s, now))
        result = 2;
      else if (received == 1)
        result = handle(&s, i, &message, now);
      if (result)
      {
        if (result == 2)
          reason = "достигнут лимит времени или ожидания вилок";
        else
          reason = "обрыв связи, ошибка протокола или отправки";
        break;
      }
    }
    if (result)
      break;
  }
  print_log("Сервер: завершение — %s (код %d).\n", reason, result);
  for (int i = 0; i < parameters->count; i++)
  {
    if (s.clients[i].state == EATING)
      print_log("Сервер: еда философа %d прервана, обе вилки освобождены, цикл не засчитан.\n", i + 1);
    s.forks[i] = 0;
    if (s.clients[i].state != DONE) s.clients[i].state = STOPPED;
  }
  send_snapshot(&s);
  for (int i = 0; i < 32; i++)
  {
    if (s.observers[i].fd < 0) continue;
    tcp_send(s.observers[i].fd, TCP_STOP, 0, result, 0, 0);
    close(s.observers[i].fd);
  }
  for (int i = 0; i <= parameters->count; i++)
  {
    Client *c = &s.clients[i];
    if (c->peer.fd >= 0)
    {
      if (c->id && answer(c, TCP_STOP, result) < 0 && !result)
        result = 1;
      close(c->peer.fd);
    }
  }
  close(listener);
  if (output_failed())
    return 1;
  return result;
}
