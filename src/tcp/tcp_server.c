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
  DONE
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
  TcpPeer observer;
  int forks[MAX_PHILOSOPHERS], queue[MAX_PHILOSOPHERS];
  int head, size, joined, finished, started;
  long long start;
} Server;

static void send_snapshot(Server *s)
{
  if (s->observer.fd < 0) return;
  for (int i = 0; i < s->p.count; i++)
  {
    int right = (i + 1) % s->p.count;
    if (tcp_send(s->observer.fd, TCP_SNAPSHOT, i + 1, s->clients[i].state,
                 s->forks[i], s->forks[right]) < 0)
    {
      close(s->observer.fd);
      s->observer.fd = -1;
      return;
    }
  }
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
      return 1;
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
    if (m->id || m->value || m->count || m->cycles || s->observer.fd >= 0)
    {
      tcp_send(c->peer.fd, TCP_STOP, 0, 1, 0, 0);
      close(c->peer.fd);
      c->peer.fd = -1;
      return 0;
    }
    s->observer = c->peer;
    c->peer.fd = -1;
    if (tcp_send(s->observer.fd, TCP_CONFIG, 0, 0, s->p.count, s->p.cycles) < 0)
    {
      close(s->observer.fd);
      s->observer.fd = -1;
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
      duplicate |= s->clients[i].id == (int)m->id;

    if (m->type != TCP_JOIN || m->id > (unsigned)s->p.count || m->value ||
        m->count || m->cycles || duplicate || s->started)
    {
      tcp_send(c->peer.fd, TCP_STOP, (int)m->id, 1, 0, 0);
      close(c->peer.fd);
      c->peer.fd = -1;
      print_log("Сервер: отклонено подключение с неверным или занятым ID.\n");
      return 0;
    }

    c->id = (int)m->id;
    c->state = READY;
    s->joined++;

    print_log("Сервер: подключён философ %d.\n", c->id);
    if (tcp_send(c->peer.fd, TCP_CONFIG, c->id, 0, s->p.count, s->p.cycles) < 0)
      return 1;
    if (s->joined == s->p.count)
    {
      s->started = 1;
      s->start = now;
      for (int i = 0; i < s->p.count; i++)
        if (think(s, &s->clients[i], TCP_START, now) < 0)
          return 1;
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
      return 1;
    send_snapshot(s);


    return give_forks(s);
  }
  return 1;
}

int run_tcp_server(const struct sockaddr_in *address, const Parameters *parameters)
{
  Server s = {.p = *parameters};
  s.observer.fd = -1;
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

    if (!s.started && now - waiting >= 10000)
    {
      result = 1;
      reason = "не все философы подключились за 10 секунд";
      break;
    }
    if (limit_reached(&s, now))
    {
      result = 2;
      reason = "достигнут лимит времени или ожидания вилок";
      break;
    }

    struct pollfd items[MAX_PHILOSOPHERS + 3] = {{.fd = listener, .events = POLLIN},
                                                  {.fd = s.observer.fd, .events = POLLIN}};
    for (int i = 0; i <= parameters->count; i++)
      items[i + 2] = (struct pollfd){.fd = s.clients[i].peer.fd, .events = POLLIN};
    
    if (poll(items, (nfds_t)parameters->count + 3, 10) < 0)
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
        int slot = -1;
        for (int i = 0; i <= parameters->count; i++)
          if (s.clients[i].peer.fd < 0)
          {
            slot = i;
            break;
          }
        if (slot < 0 || tcp_nonblocking(fd) < 0)
          close(fd);
        else
          s.clients[slot] = (Client){.peer = {.fd = fd}};
      }
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
      {
        result = 1;
        reason = "ошибка accept";
        break;
      }
    }
    if (items[1].revents)
    {
      TcpMessage message;
      if (tcp_receive(&s.observer, &message) != 0)
      {
        close(s.observer.fd);
        s.observer.fd = -1;
      }
    }
    for (int i = 0; i <= parameters->count; i++)
    {
      if (!items[i + 2].revents)
        continue;
      TcpMessage message;
      int received = tcp_receive(&s.clients[i].peer, &message);
      if (received < 0 && !s.clients[i].id)
      {
        close(s.clients[i].peer.fd);
        s.clients[i].peer.fd = -1;
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
  if (s.observer.fd >= 0)
  {
    tcp_send(s.observer.fd, TCP_STOP, 0, result, 0, 0);
    close(s.observer.fd);
  }
  for (int i = 0; i < parameters->count; i++)
  {
    Client *c = &s.clients[i];
    if (c->peer.fd >= 0)
    {
      if (c->id && answer(c, TCP_STOP, result) < 0 && !result)
        result = 1;
      close(c->peer.fd);
    }
    if (c->state == EATING)
    {
      s.forks[c->id - 1] = s.forks[c->id % parameters->count] = 0;
      print_log("Сервер: еда философа %d прервана, обе вилки освобождены, цикл не засчитан.\n", c->id);
    }
  }
  close(listener);
  if (output_failed())
    return 1;
  return result;
}
