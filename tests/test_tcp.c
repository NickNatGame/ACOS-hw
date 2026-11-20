#include "soft_assert.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

static int fl_err;
static const char *server_binary, *client_binary, *observer_binary;
static char directory[] = TEST_LOG_DIRECTORY "/run-XXXXXX";
static char port[16];
static int serial;

enum { JOIN = 1, CONFIG, START, HUNGRY, EAT, FINISH, CONTINUE, STOP };
typedef struct { pid_t pid; char path[sizeof(directory) + 32]; } Process;
static Process processes[32];
static int process_count;
static int sockets[32], socket_count;

static void pause_test(int milliseconds)
{
  struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}

static int contains(const Process *process, const char *text)
{
  FILE *file = fopen(process->path, "r");
  if (!file) return 0;
  char line[16384];
  int found = 0;
  while (fgets(line, sizeof(line), file))
    if (strstr(line, text)) found = 1;
  fclose(file);
  return found;
}

static void wait_text(Process *process, const char *text)
{
  for (int i = 0; i < 600; i++)
  {
    if (contains(process, text)) return;
    pause_test(5);
  }
  SOFT_ASSERT_ERR(0);
  fprintf(stderr, "Не найдено '%s' в %s\n", text, process->path);
}

static Process *launch(char *const arguments[])
{
  if (process_count == 32) { SOFT_ASSERT_ERR(0); exit(1); }
  Process *process = &processes[process_count++];
  snprintf(process->path, sizeof(process->path), "%s/%d.txt", directory, serial++);
  process->pid = fork();
  SOFT_ASSERT_ERR(process->pid >= 0);
  if (process->pid < 0) exit(1);
  if (process->pid == 0)
  {
    int out = open(process->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0 || dup2(out, 1) < 0 || dup2(out, 2) < 0) _exit(127);
    close(out);
    /* Тестовые сокеты не должны наследоваться дочерними программами. */
    for (int i = 0; i < socket_count; i++) close(sockets[i]);
    execv(arguments[0], arguments);
    _exit(127);
  }
  return process;
}

static int finish(Process *process)
{
  int status;
  for (int i = 0; i < 13000; i++)
  {
    pid_t result = waitpid(process->pid, &status, WNOHANG);
    if (result == process->pid)
    {
      process->pid = 0;
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
      return -1;
    }
    if (result < 0 && errno != EINTR) break;
    pause_test(5);
  }
  SOFT_ASSERT_ERR(0);
  fprintf(stderr, "Процесс завис: %s\n", process->path);
  if (process->pid > 0)
  {
    kill(process->pid, SIGKILL);
    waitpid(process->pid, &status, 0);
    process->pid = 0;
  }
  return -1;
}

static void expect_code(Process *process, int expected)
{
  int actual = finish(process);
  SOFT_ASSERT_ERR(actual == expected);
  if (actual != expected)
    fprintf(stderr, "%s: код %d, ожидался %d\n", process->path, actual, expected);
}

static void cleanup(void)
{
  for (int i = 0; i < process_count; i++)
    if (processes[i].pid > 0)
    {
      kill(processes[i].pid, SIGKILL);
      int status;
      while (waitpid(processes[i].pid, &status, 0) < 0 && errno == EINTR) {}
    }
  for (int i = 0; i < socket_count; i++)
    if (sockets[i] >= 0) close(sockets[i]);
  process_count = socket_count = 0;
}

static int local_socket(int listener)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  SOFT_ASSERT_ERR(fd >= 0);
  if (fd < 0) return -1;
  struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
  if (listener)
  {
    SOFT_ASSERT_ERR(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    SOFT_ASSERT_ERR(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
    snprintf(port, sizeof(port), "%u", ntohs(address.sin_port));
    SOFT_ASSERT_ERR(listen(fd, 5) == 0);
  }
  else
  {
    address.sin_port = htons((uint16_t)atoi(port));
    SOFT_ASSERT_ERR(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  }
  struct timeval timeout = {2, 0};
  SOFT_ASSERT_ERR(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
  SOFT_ASSERT_ERR(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
  sockets[socket_count++] = fd;
  return fd;
}

static void close_socket(int fd)
{
  close(fd);
  for (int i = 0; i < socket_count; i++)
    if (sockets[i] == fd) sockets[i] = -1;
}

static Process *start_server(int count, int cycles, int think, int eat, int max_wait, int limit)
{
  int probe = local_socket(1);
  close_socket(probe);
  char numbers[6][16];
  int values[] = {count, cycles, think, eat, max_wait, limit};
  for (int i = 0; i < 6; i++) snprintf(numbers[i], sizeof(numbers[i]), "%d", values[i]);
  char *args[] = {(char *)server_binary, "127.0.0.1", port, numbers[0], numbers[1],
      numbers[2], numbers[2], numbers[3], numbers[3], numbers[4], "0", "42", "fifo", numbers[5], NULL};
  Process *server = launch(args);
  wait_text(server, "ГОТОВО");
  return server;
}

static Process *start_client(int id)
{
  char number[16];
  snprintf(number, sizeof(number), "%d", id);
  char *args[] = {(char *)client_binary, "127.0.0.1", port, number, NULL};
  return launch(args);
}

static Process *start_observer(void)
{
  char *args[] = {(char *)observer_binary, "127.0.0.1", port, NULL};
  return launch(args);
}

/* Формат кодируем независимо от tcp_io.c, чтобы проверять сам протокол. */
static void frame(uint32_t words[6], int version, int type, int id, int value, int count, int cycles)
{
  int values[] = {version, type, id, value, count, cycles};
  for (int i = 0; i < 6; i++) words[i] = htonl((uint32_t)values[i]);
}

static void send_bytes(int fd, const void *data, size_t length)
{
  size_t sent = 0;
  while (sent < length)
  {
    ssize_t n = send(fd, (const char *)data + sent, length - sent, MSG_NOSIGNAL);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { SOFT_ASSERT_ERR(0); return; }
    sent += (size_t)n;
  }
}

static void send_packet(int fd, int type, int id)
{
  uint32_t words[6];
  frame(words, 1, type, id, 0, 0, 0);
  send_bytes(fd, words, sizeof(words));
}

static void receive_packet(int fd, uint32_t words[6])
{
  memset(words, 0, 24);
  size_t used = 0;
  while (used < 24)
  {
    ssize_t n = recv(fd, (char *)words + used, 24 - used, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { SOFT_ASSERT_ERR(0); return; }
    used += (size_t)n;
  }
  for (int i = 0; i < 6; i++) words[i] = ntohl(words[i]);
  SOFT_ASSERT_ERR(words[0] == 1);
}

static void expect_packet(int fd, int type, int value)
{
  uint32_t words[6];
  receive_packet(fd, words);
  SOFT_ASSERT_ERR(words[1] == (unsigned)type);
  if (value >= 0) SOFT_ASSERT_ERR(words[3] == (unsigned)value);
}

static int raw_client(int id, int fragmented)
{
  int fd = local_socket(0);
  uint32_t words[6];
  frame(words, 1, JOIN, id, 0, 0, 0);
  if (fragmented)
  {
    send_bytes(fd, words, 3);
    pause_test(20);
    send_bytes(fd, (char *)words + 3, 21);
  }
  else send_bytes(fd, words, 24);
  return fd;
}

static void check_history(Process *server, int count)
{
  FILE *file = fopen(server->path, "r");
  SOFT_ASSERT_ERR(file != NULL);
  if (!file) return;
  int owners[256] = {0}, queue[1024], head = 0, size = 0, meals = 0;
  char line[4096];
  while (fgets(line, sizeof(line), file))
  {
    int id, left, right;
    if (sscanf(line, "Сервер: философ %d сообщил о голоде", &id) == 1 && strstr(line, "сообщил о голоде"))
      queue[size++] = id;
    if (sscanf(line, "Сервер: выдал философу %d вилки %d и %d", &id, &left, &right) == 3)
    {
      SOFT_ASSERT_ERR(head < size && queue[head] == id);
      head++;
      SOFT_ASSERT_ERR(left >= 1 && left <= count && right >= 1 && right <= count);
      if (left < 1 || left > count || right < 1 || right > count) continue;
      SOFT_ASSERT_ERR(!owners[left - 1] && !owners[right - 1]);
      owners[left - 1] = owners[right - 1] = id;
    }
    if (sscanf(line, "Сервер: философ %d сообщил об окончании еды; освободил вилки %d и %d", &id, &left, &right) == 3)
    {
      SOFT_ASSERT_ERR(left >= 1 && left <= count && right >= 1 && right <= count);
      if (left < 1 || left > count || right < 1 || right > count) continue;
      SOFT_ASSERT_ERR(owners[left - 1] == id && owners[right - 1] == id);
      owners[left - 1] = owners[right - 1] = 0;
      meals++;
    }
  }
  fclose(file);
  SOFT_ASSERT_ERR(meals == count * 3 && head == size);
  for (int i = 0; i < count; i++) SOFT_ASSERT_ERR(owners[i] == 0);
}

static void test_cycles_fifo_and_forks(void)
{
  for (int count = 2; count <= 5; count += 3)
  {
    Process *server = start_server(count, 3, 0, 20, 1000, 5000);
    Process *clients[5];
    for (int i = 0; i < count; i++) clients[i] = start_client(i + 1);
    expect_code(server, 0);
    for (int i = 0; i < count; i++)
    {
      expect_code(clients[i], 0);
      SOFT_ASSERT_ERR(contains(clients[i], "цикл 3/3"));
    }
    check_history(server, count);
    cleanup();
  }
}

static void test_fragmented_and_parallel(void)
{
  Process *server = start_server(5, 1, 0, 300, 1000, 5000);
  int fds[5];
  for (int i = 0; i < 5; i++) fds[i] = raw_client(i + 1, i == 0);
  for (int i = 0; i < 5; i++) { expect_packet(fds[i], CONFIG, 0); expect_packet(fds[i], START, 0); }
  send_packet(fds[0], HUNGRY, 1); expect_packet(fds[0], EAT, 300);
  send_packet(fds[2], HUNGRY, 3); expect_packet(fds[2], EAT, 300);
  kill(server->pid, SIGTERM);
  expect_code(server, 143);
  for (int i = 0; i < 5; i++) expect_packet(fds[i], STOP, 143);
}

static void test_duplicate(void)
{
  Process *server = start_server(2, 2, 100, 20, 1000, 5000);
  Process *first = start_client(1);
  wait_text(server, "подключён философ 1");
  expect_code(start_client(1), 1);
  Process *second = start_client(2);
  expect_code(server, 0); expect_code(first, 0); expect_code(second, 0);
}

static void test_observer(void)
{
  Process *server = start_server(2, 1, 0, 30, 1000, 5000);
  Process *observer = start_observer();
  Process *first = start_client(1), *second = start_client(2);
  expect_code(server, 0); expect_code(first, 0); expect_code(second, 0); expect_code(observer, 0);
  SOFT_ASSERT_ERR(contains(observer, "Наблюдатель: подключён"));
  SOFT_ASSERT_ERR(contains(observer, "P1=") && contains(observer, "F1="));
}

static void test_late_observer(void)
{
  Process *server = start_server(2, 1, 100, 20, 1000, 5000);
  Process *first = start_client(1), *second = start_client(2);
  wait_text(first, "начал размышление");
  Process *observer = start_observer();
  expect_code(server, 0); expect_code(first, 0); expect_code(second, 0); expect_code(observer, 0);
  SOFT_ASSERT_ERR(contains(observer, "Наблюдатель: подключён"));
}

static void test_limits(void)
{
  for (int kind = 0; kind < 2; kind++)
  {
    int think = 500, eat = 20, max_wait = 1000, limit = 30;
    if (kind == 1) { think = 0; eat = 500; max_wait = 30; limit = 5000; }
    Process *server = start_server(2, 2, think, eat, max_wait, limit);
    Process *first = start_client(1), *second = start_client(2);
    expect_code(server, 2); expect_code(first, 2); expect_code(second, 2);
    cleanup();
  }
}

static void test_client_return(void)
{
  Process *server = start_server(2, 2, 2000, 20, 1000, 5000);
  Process *first = start_client(1), *second = start_client(2);
  wait_text(server, "философ 1 сообщил об окончании еды; освободил вилки 1 и 2; цикл 1/2");
  kill(first->pid, SIGKILL); expect_code(first, 137);
  wait_text(server, "отключён");
  Process *replacement = start_client(1);
  expect_code(server, 0); expect_code(second, 0); expect_code(replacement, 0);
}

static void test_multiple_observers(void)
{
  Process *server = start_server(2, 1, 300, 20, 1000, 5000);
  Process *observers[3];
  for (int i = 0; i < 3; i++) observers[i] = start_observer();
  Process *first = start_client(1), *second = start_client(2);
  for (int i = 0; i < 3; i++) wait_text(observers[i], "P1=");
  kill(observers[0]->pid, SIGTERM); expect_code(observers[0], 143);
  Process *replacement = start_observer();
  expect_code(server, 0); expect_code(first, 0); expect_code(second, 0);
  for (int i = 1; i < 3; i++)
  {
    expect_code(observers[i], 0);
    SOFT_ASSERT_ERR(contains(observers[i], "P1=завершил P2=завершил"));
  }
  expect_code(replacement, 0);
  SOFT_ASSERT_ERR(contains(replacement, "P1=завершил P2=завершил"));
}

static void test_server_signal(void)
{
  Process *server = start_server(2, 2, 2000, 20, 1000, 5000);
  Process *a = start_observer(), *b = start_observer();
  Process *first = start_client(1), *second = start_client(2);
  wait_text(a, "P1=думает"); wait_text(b, "P1=думает");
  kill(server->pid, SIGTERM);
  expect_code(server, 143); expect_code(a, 143); expect_code(b, 143);
  expect_code(first, 143); expect_code(second, 143);
}

static void test_return_during_eating(void)
{
  Process *server = start_server(2, 2, 0, 50, 1000, 5000);
  int first = raw_client(1, 0), second = raw_client(2, 0);
  expect_packet(first, CONFIG, 0); expect_packet(first, START, 0);
  expect_packet(second, CONFIG, 0); expect_packet(second, START, 0);
  send_packet(first, HUNGRY, 1); expect_packet(first, EAT, 50);
  pause_test(60); send_packet(first, FINISH, 1); expect_packet(first, CONTINUE, 0);
  send_packet(first, HUNGRY, 1); expect_packet(first, EAT, 50);
  close_socket(first); wait_text(server, "отключён");
  int returned = raw_client(1, 0);
  expect_packet(returned, CONFIG, 1); expect_packet(returned, START, 0);
  send_packet(second, HUNGRY, 2); expect_packet(second, EAT, 50);
  kill(server->pid, SIGTERM);
  expect_packet(returned, STOP, 143); expect_packet(second, STOP, 143); expect_code(server, 143);
}

static void test_server_death(void)
{
  Process *server = start_server(2, 2, 2000, 20, 1000, 5000);
  Process *first = start_client(1), *second = start_client(2);
  wait_text(first, "начал размышление");
  kill(server->pid, SIGKILL); expect_code(server, 137);
  expect_code(first, 1); expect_code(second, 1);
}

static void test_reset_during_config(void)
{
  Process *server = start_server(2, 2, 0, 20, 1000, 5000);
  int first = raw_client(1, 0), second = raw_client(2, 0);
  expect_packet(first, CONFIG, 0); expect_packet(first, START, 0);
  expect_packet(second, CONFIG, 0); expect_packet(second, START, 0);
  send_packet(first, HUNGRY, 1); expect_packet(first, EAT, 20);
  pause_test(30); send_packet(first, FINISH, 1); expect_packet(first, CONTINUE, 0);
  close_socket(first); wait_text(server, "отключён; завершено циклов 1");

  /* RST должен попасть в очередь до чтения JOIN и отправки CONFIG сервером. */
  SOFT_ASSERT_ERR(kill(server->pid, SIGSTOP) == 0);
  int status;
  pid_t stopped;
  do { stopped = waitpid(server->pid, &status, WUNTRACED); }
  while (stopped < 0 && errno == EINTR);
  SOFT_ASSERT_ERR(stopped == server->pid && WIFSTOPPED(status));
  int reset = raw_client(1, 0);
  struct linger linger = {1, 0};
  SOFT_ASSERT_ERR(setsockopt(reset, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == 0);
  close_socket(reset);
  SOFT_ASSERT_ERR(kill(server->pid, SIGCONT) == 0);

  int returned = raw_client(1, 0);
  expect_packet(returned, CONFIG, 1); expect_packet(returned, START, 0);
  send_packet(returned, HUNGRY, 1); expect_packet(returned, EAT, 20);
  pause_test(30); send_packet(returned, FINISH, 1);
  for (int cycle = 0; cycle < 2; cycle++)
  {
    send_packet(second, HUNGRY, 2); expect_packet(second, EAT, 20);
    pause_test(30); send_packet(second, FINISH, 2);
    if (cycle == 0) expect_packet(second, CONTINUE, 0);
  }
  expect_packet(returned, STOP, 0); expect_packet(second, STOP, 0);
  expect_code(server, 0);
}

static void test_early_finish(void)
{
  Process *server = start_server(2, 2, 0, 2000, 1000, 5000);
  int first = raw_client(1, 0), second = raw_client(2, 0);
  expect_packet(first, CONFIG, 0); expect_packet(first, START, 0);
  expect_packet(second, CONFIG, 0); expect_packet(second, START, 0);
  uint32_t words[12];
  frame(words, 1, HUNGRY, 1, 0, 0, 0); frame(words + 6, 1, FINISH, 1, 0, 0, 0);
  send_bytes(first, words, sizeof(words));
  expect_packet(first, EAT, 2000); expect_packet(first, STOP, 1);
  expect_code(server, 1); SOFT_ASSERT_ERR(contains(server, "цикл не засчитан"));
}

static void test_missing_clients(void)
{
  Process *server = start_server(2, 2, 0, 20, 1000, 5000);
  expect_code(server, 1); SOFT_ASSERT_ERR(contains(server, "60 секунд"));
}

static void test_wrong_id_and_version(void)
{
  for (int kind = 0; kind < 2; kind++)
  {
    Process *server = start_server(2, 2, 0, 20, 1000, 5000);
    int first = raw_client(1, 0), second = raw_client(2, 0);
    expect_packet(first, CONFIG, 0); expect_packet(first, START, 0);
    expect_packet(second, CONFIG, 0); expect_packet(second, START, 0);
    uint32_t words[6];
    if (kind == 0) frame(words, 1, HUNGRY, 2, 0, 0, 0);
    else frame(words, 2, HUNGRY, 1, 0, 0, 0);
    send_bytes(first, words, 24);
    expect_packet(second, STOP, 1); expect_code(server, 1);
    cleanup();
  }
}

static void test_client_fragmented_replies(void)
{
  int listener = local_socket(1);
  Process *client = start_client(1);
  struct pollfd item = {.fd = listener, .events = POLLIN};
  int ready = poll(&item, 1, 2000);
  SOFT_ASSERT_ERR(ready > 0);
  if (ready <= 0) return;
  int fd = accept(listener, NULL, NULL);
  SOFT_ASSERT_ERR(fd >= 0);
  if (fd < 0) return;
  sockets[socket_count++] = fd;
  struct timeval timeout = {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  expect_packet(fd, JOIN, 0);
  uint32_t words[12];
  frame(words, 1, CONFIG, 1, 0, 2, 1); frame(words + 6, 1, START, 1, 0, 0, 0);
  send_bytes(fd, words, sizeof(words)); expect_packet(fd, HUNGRY, 0);
  frame(words, 1, EAT, 1, 1, 0, 0);
  send_bytes(fd, words, 5); pause_test(20); send_bytes(fd, (char *)words + 5, 19);
  expect_packet(fd, FINISH, 0); send_packet(fd, STOP, 1); expect_code(client, 0);
}

static void test_invalid_arguments(void)
{
  char *server_args[][5] = {{"127.0.0.1", "0", NULL}, {"bad", "1234", NULL},
      {"127.0.0.1", "65536", NULL}, {"127.0.0.1", "1234", "1", NULL}};
  for (int i = 0; i < 4; i++)
  {
    char *args[] = {(char *)server_binary, server_args[i][0], server_args[i][1], server_args[i][2], NULL};
    expect_code(launch(args), 1);
  }
  char *invalid[] = {"0", "-1", "257", "1abc"};
  for (int i = 0; i < 4; i++)
  {
    char *args[] = {(char *)client_binary, "127.0.0.1", "1234", invalid[i], NULL};
    expect_code(launch(args), 1);
  }
}

int main(int argc, char **argv)
{
  if (argc != 4) { fprintf(stderr, "Нужны пути SERVER CLIENT OBSERVER\n"); return 1; }
  server_binary = argv[1]; client_binary = argv[2]; observer_binary = argv[3];
  if (mkdir(TEST_LOG_DIRECTORY, 0700) < 0 && errno != EEXIST)
  { perror("mkdir logs"); return 1; }
  if (!mkdtemp(directory)) { perror("mkdtemp"); return 1; }
  atexit(cleanup);
  void (*tests[])(void) = {test_cycles_fifo_and_forks, test_fragmented_and_parallel,
      test_duplicate, test_observer, test_late_observer, test_limits, test_client_return,
      test_multiple_observers, test_server_signal, test_return_during_eating, test_server_death,
      test_early_finish, test_missing_clients, test_wrong_id_and_version,
      test_client_fragmented_replies, test_invalid_arguments, test_reset_during_config};
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
  {
    int before = fl_err;
    int first_log = serial;
    tests[i]();
    cleanup();
    printf("Тест %zu: ", i + 1);
    if (fl_err == before) printf("OK\n");
    else printf("FAIL\n");
    printf("  Логи процессов: %d.txt..%d.txt\n", first_log, serial - 1);
  }
  printf("Ошибок: %d. Вывод процессов: %s\n", fl_err, directory);
  if (fl_err) return 1;
  return 0;
}
