#include "tcp_philosophers.h"
#include "console.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

/* Время ожидания не блокирует получение STOP и обнаружение закрытого сокета */
static int wait_message(TcpPeer *peer, TcpMessage *m, int timeout) {
  long long start = tcp_now();
  if (start < 0) return -1;

  while (!received_signal() && !output_failed()) {
    long long now = tcp_now();
    if (now < 0) return -1;

    if (timeout >= 0 && now - start >= timeout) return 0;

    struct pollfd item = {.fd = peer->fd, .events = POLLIN};
    int ready = poll(&item, 1, 10);

    if (ready < 0 && errno != EINTR) return -1;
    if (ready > 0) {
      int result = tcp_receive(peer, m);
      if (result) return result;
    }
  }
  return -1;
}

static int stop_code(const TcpMessage *m, int completed, int cycles) {
  if (m->count || m->cycles || (m->value == 0 && (!cycles || completed != cycles))) return 1;
  if (m->value == 0 || m->value == 1 || m->value == 2 ||
      m->value == 130 || m->value == 143)
    return (int)m->value;
  return 1;
}

int run_tcp_client(const struct sockaddr_in *address, int id) {
  TcpPeer peer = {.fd = socket(AF_INET, SOCK_STREAM, 0)};
  if (peer.fd < 0) { perror("socket"); return 1; }

  int result = 1, completed = 0, cycles = 0, count = 0;
  if (tcp_nonblocking(peer.fd) < 0) goto cleanup;

  if (connect(peer.fd, (const struct sockaddr *)address, sizeof(*address)) < 0) {
    if (errno != EINPROGRESS) goto cleanup;
    struct pollfd item = {.fd = peer.fd, .events = POLLOUT};
    long long start = tcp_now();
    int connected = 0;

    while (start >= 0 && !received_signal()) {
      long long now = tcp_now();
      if (now < 0 || now - start >= 5000) break;
      int ready = poll(&item, 1, 10);

      if (ready < 0 && errno != EINTR) break;
      if (ready > 0) {
        int error;
        socklen_t length = sizeof(error);
        if (getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && !error)
          connected = 1;
        break;
      }
    }
    if (!connected) goto cleanup;
  }
  if (tcp_send(peer.fd, TCP_JOIN, id, 0, 0, 0) < 0) goto cleanup;

  TcpMessage m;
  if (wait_message(&peer, &m, 65000) != 1 || m.id != (unsigned)id) goto cleanup;
  if (m.type == TCP_STOP) { result = stop_code(&m, completed, cycles); goto cleanup; }
  if (m.type != TCP_CONFIG || m.value > m.cycles || m.count < 2 || m.count < (unsigned)id || !m.cycles)
    goto cleanup;
  
  count = (int)m.count;
  cycles = (int)m.cycles;
  completed = (int)m.value;
  print_log("Философ %d: подключён, участников %d, циклов %d.\n", id, count, cycles);
  int expected = TCP_START;
  while (1) {
    int timeout = -1;
    if (expected == TCP_START && completed < cycles)
      timeout = 65000;

    if (wait_message(&peer, &m, timeout) != 1 ||
        m.id != (unsigned)id || m.count || m.cycles) break;

    if (m.type == TCP_STOP) { result = stop_code(&m, completed, cycles); break; }
    if (m.type != (unsigned)expected || completed >= cycles) break;

    print_log("Философ %d: начал размышление на %u ms.\n", id, m.value);
    int received = wait_message(&peer, &m, (int)m.value);
    if (received != 0) {
      if (received == 1 && m.id == (unsigned)id && m.type == TCP_STOP)
        result = stop_code(&m, completed, cycles);
      break;
    }
    print_log("Философ %d: закончил размышление, проголодался; запросил левую вилку %d и правую %d.\n",
              id, id, id % count + 1);
    long long hungry = tcp_now();
    if (hungry < 0 || tcp_send(peer.fd, TCP_HUNGRY, id, 0, 0, 0) < 0) break;
    if (wait_message(&peer, &m, -1) != 1 || m.id != (unsigned)id || m.count || m.cycles) break;
    if (m.type == TCP_STOP) { result = stop_code(&m, completed, cycles); break; }
    if (m.type != TCP_EAT || !m.value) break;

    long long now = tcp_now();
    if (now < 0) break;
    print_log("Философ %d: получил левую вилку %d и правую %d; начал есть на %u ms, ожидание %lld ms.\n",
              id, id, id % count + 1, m.value, now - hungry);
              
    received = wait_message(&peer, &m, (int)m.value);
    if (received != 0) {
      print_log("Философ %d: еда прервана, цикл не засчитан.\n", id);
      if (received == 1 && m.id == (unsigned)id && m.type == TCP_STOP)
        result = stop_code(&m, completed, cycles);
      break;
    }
    completed++;
    print_log("Философ %d: закончил есть, отправляет освобождение вилок %d и %d; цикл %d/%d.\n",
              id, id, id % count + 1, completed, cycles);
    if (tcp_send(peer.fd, TCP_FINISH, id, 0, 0, 0) < 0) break;
    expected = TCP_CONTINUE;
  }
cleanup:
  if (received_signal()) result = 128 + received_signal();
  if (output_failed()) result = 1;
  if (result) dprintf(2, "Философ %d: остановлен (код %d): ошибка подключения/протокола, обрыв, лимит или сигнал.\n", id, result);
  else print_log("Философ %d: завершён по сообщению сервера.\n", id);
  close(peer.fd);
  return result;
}
