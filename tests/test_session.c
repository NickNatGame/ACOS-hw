#include "soft_assert.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static int fl_err;
static const char *coordinator;
static const char *participant;
static int session_number;

static void pause_test(void)
{
  struct timespec delay = {0, 5000000L};
  nanosleep(&delay, NULL);
}

static int contains(const char *path, const char *text)
{
  FILE *file = fopen(path, "r");
  if (!file)
  {
    return 0;
  }
  char line[4096];
  int found = 0;
  while (fgets(line, sizeof(line), file))
  {
    found |= strstr(line, text) != NULL;
  }
  fclose(file);
  return found;
}

static pid_t launch(char *const arguments[], int elsewhere)
{
  pid_t pid = fork();
  SOFT_ASSERT_ERR(pid >= 0);
  if (pid == 0)
  {
    int out = open("/dev/null", O_WRONLY);
    int errors = open("errors.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (out < 0 || errors < 0 || dup2(out, STDOUT_FILENO) == -1 ||
        dup2(errors, STDERR_FILENO) == -1)
    {
      _exit(127);
    }
    close(out);
    close(errors);
    if (elsewhere && chdir("/tmp") == -1)
    {
      _exit(127);
    }
    execv(arguments[0], arguments);
    _exit(127);
  }
  return pid;
}

static int finish(pid_t pid)
{
  int status;
  for (int i = 0; i < 3000; i++)
  {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
    {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (result == -1)
    {
      SOFT_ASSERT_ERR(0);
      return -1;
    }
    pause_test();
  }
  SOFT_ASSERT_ERR(0); /* Процесс не должен зависать. */
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return -1;
}

static void new_session(char name[64])
{
  snprintf(name, 64, "test_%ld_%d", (long)getpid(), session_number++);
}

static pid_t start_coordinator(const char *name, int count, int slow, int limit)
{
  char number[16], time_limit[16];
  snprintf(number, sizeof(number), "%d", count);
  snprintf(time_limit, sizeof(time_limit), "%d", limit);
  char *arguments[] = {(char *)coordinator, (char *)name, number,
      slow ? "100" : "2", "2", "5", slow ? "20" : "3",
      slow ? "20" : "5", slow == 2 ? "1" : "2000", "0", "42", "fifo", time_limit, NULL};
  pid_t pid = launch(arguments, 0);
  char log[100];
  snprintf(log, sizeof(log), "philosophers-%s.log", name);
  int ready = 0;
  for (int i = 0; i < 400; i++)
  {
    if (contains(log, "ГОТОВО"))
    {
      ready = 1;
      break;
    }
    pause_test();
  }
  SOFT_ASSERT_ERR(ready);
  return pid;
}

static pid_t start_participant(const char *name, const char *id)
{
  char *arguments[] = {(char *)participant, (char *)name, (char *)id, NULL};
  return launch(arguments, 1);
}

static void check_cleanup(const char *name)
{
  char path[100];
  snprintf(path, sizeof(path), "/philosophers-session-%s", name);
  int fd = shm_open(path, O_RDWR, 0);
  SOFT_ASSERT_ERR(fd == -1 && errno == ENOENT);
  if (fd >= 0)
  {
    close(fd);
  }
  DIR *directory = opendir("/dev/shm");
  SOFT_ASSERT_ERR(directory != NULL);
  if (!directory)
  {
    return;
  }
  snprintf(path, sizeof(path), "sem.philosophers-session-%s-", name);
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL)
  {
    SOFT_ASSERT_ERR(strncmp(entry->d_name, path, strlen(path)) != 0);
  }
  closedir(directory);
}

static void check_no_children(pid_t pid)
{
  char path[100];
  snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children", (long)pid, (long)pid);
  FILE *file = fopen(path, "r");
  SOFT_ASSERT_ERR(file != NULL);
  if (file)
  {
    long child;
    SOFT_ASSERT_ERR(fscanf(file, "%ld", &child) == EOF);
    fclose(file);
  }
}

/* Независимо восстанавливаем занятость вилок и порядок FIFO по журналу */
static void check_events(const char *name, int count)
{
  char path[100], line[4096];
  snprintf(path, sizeof(path), "philosophers-%s.log", name);
  FILE *file = fopen(path, "r");
  SOFT_ASSERT_ERR(file != NULL);
  if (!file)
  {
    return;
  }
  int owners[256], queue[1024], head = 0, tail = 0, meals = 0;
  for (int i = 0; i < count; i++)
  {
    owners[i] = -1;
  }
  while (fgets(line, sizeof(line), file))
  {
    long long time;
    int id, fork_id;
    if (sscanf(line, "[%lld ms] Философ %d:", &time, &id) != 2)
    {
      continue;
    }
    SOFT_ASSERT_ERR(id >= 1 && id <= count);
    if (id < 1 || id > count)
    {
      continue;
    }
    if (strstr(line, "проголодался"))
    {
      SOFT_ASSERT_ERR(tail < 1024);
      if (tail < 1024)
      {
        queue[tail++] = id;
      }
    }
    if (strstr(line, "получил левую"))
    {
      SOFT_ASSERT_ERR(head < tail && queue[head] == id);
      head++;
    }
    if (sscanf(line, "[%lld ms] Философ %d: получил левую вилку %d",
               &time, &id, &fork_id) == 3 ||
        sscanf(line, "[%lld ms] Философ %d: получил правую вилку %d",
               &time, &id, &fork_id) == 3)
    {
      SOFT_ASSERT_ERR(fork_id >= 1 && fork_id <= count);
      if (fork_id >= 1 && fork_id <= count)
      {
        SOFT_ASSERT_ERR(owners[fork_id - 1] == -1);
        owners[fork_id - 1] = id;
      }
    }
    if (strstr(line, "освободил вилки"))
    {
      SOFT_ASSERT_ERR(owners[id - 1] == id && owners[id % count] == id);
      owners[id - 1] = owners[id % count] = -1;
    }
    meals += strstr(line, "закончил есть") != NULL;
  }
  fclose(file);
  SOFT_ASSERT_ERR(head == tail && meals == count * 2);
  for (int i = 0; i < count; i++)
  {
    SOFT_ASSERT_ERR(owners[i] == -1);
  }
}

static void test_normal(int count)
{
  char name[64];
  new_session(name);
  pid_t parent = start_coordinator(name, count, 0, 3000);
  pid_t philosophers[5];
  for (int i = count - 1; i >= 0; i--)
  {
    char id[8];
    snprintf(id, sizeof(id), "%d", i + 1);
    philosophers[i] = start_participant(name, id);
  }
  check_no_children(parent);
  for (int i = 0; i < count; i++)
  {
    SOFT_ASSERT_ERR(finish(philosophers[i]) == 0);
  }
  SOFT_ASSERT_ERR(finish(parent) == 0);
  check_events(name, count);
  check_cleanup(name);
}

static void test_duplicates(void)
{
  char name[64];
  new_session(name);
  pid_t parent = start_coordinator(name, 2, 0, 3000);
  char *duplicate[] = {(char *)coordinator, name, NULL};
  SOFT_ASSERT_ERR(finish(launch(duplicate, 0)) == 1);
  pid_t first = start_participant(name, "1");
  char log[100];
  snprintf(log, sizeof(log), "philosophers-%s.log", name);
  for (int i = 0; i < 400 && !contains(log, "философ 1:"); i++)
  {
    pause_test();
  }
  SOFT_ASSERT_ERR(contains(log, "ГОТОВО"));
  SOFT_ASSERT_ERR(finish(start_participant(name, "1")) == 1);
  SOFT_ASSERT_ERR(finish(start_participant(name, "3")) == 1);
  pid_t second = start_participant(name, "2");
  SOFT_ASSERT_ERR(finish(first) == 0);
  SOFT_ASSERT_ERR(finish(second) == 0);
  SOFT_ASSERT_ERR(finish(parent) == 0);
  check_events(name, 2);
  check_cleanup(name);
}

static void test_stop(int target, int number)
{
  char name[64];
  new_session(name);
  pid_t parent = start_coordinator(name, 2, 1, 3000);
  pid_t first = start_participant(name, "1");
  pid_t second = start_participant(name, "2");
  char log[100];
  snprintf(log, sizeof(log), "philosophers-%s.log", name);
  for (int i = 0; i < 400 && !contains(log, "Старт моделирования"); i++)
  {
    pause_test();
  }
  SOFT_ASSERT_ERR(contains(log, "Старт моделирования"));
  SOFT_ASSERT_ERR(kill(target ? first : parent, number) == 0);
  int first_result = finish(first);
  int second_result = finish(second);
  int parent_result = finish(parent);
  SOFT_ASSERT_ERR(first_result != 0 && second_result != 0 && parent_result != 0);
  if (!target)
  {
    SOFT_ASSERT_ERR(parent_result == 128 + number);
  }
  check_cleanup(name);
}

static void test_missing(int with_participant)
{
  char name[64];
  new_session(name);
  pid_t parent = start_coordinator(name, 2, 0, 3000);
  pid_t first = with_participant ? start_participant(name, "1") : 0;
  if (with_participant)
  {
    SOFT_ASSERT_ERR(finish(parent) == 1);
    SOFT_ASSERT_ERR(finish(first) == 1);
  }
  else
  {
    SOFT_ASSERT_ERR(kill(parent, SIGINT) == 0);
    SOFT_ASSERT_ERR(finish(parent) == 130);
  }
  check_cleanup(name);
}

static void test_foreign_semaphore(void)
{
  char name[64], path[100];
  new_session(name);
  snprintf(path, sizeof(path), "/philosophers-session-%s-wake-1", name);
  sem_t *foreign = sem_open(path, O_CREAT | O_EXCL, 0600, 7);
  SOFT_ASSERT_ERR(foreign != SEM_FAILED);
  if (foreign == SEM_FAILED)
  {
    return;
  }
  char *arguments[] = {(char *)coordinator, name, "2", NULL};
  SOFT_ASSERT_ERR(finish(launch(arguments, 0)) == 1);
  sem_t *opened = sem_open(path, 0);
  int value = 0;
  SOFT_ASSERT_ERR(opened != SEM_FAILED);
  if (opened != SEM_FAILED)
  {
    SOFT_ASSERT_ERR(sem_getvalue(opened, &value) == 0 && value == 7);
    sem_close(opened);
  }
  sem_close(foreign);
  sem_unlink(path);
  check_cleanup(name);
}

static void test_limits(int wait_limit)
{
  char name[64];
  new_session(name);
  pid_t parent = start_coordinator(name, 2, wait_limit ? 2 : 1, wait_limit ? 3000 : 50);
  pid_t first = start_participant(name, "1");
  pid_t second = start_participant(name, "2");
  SOFT_ASSERT_ERR(finish(first) == 2);
  SOFT_ASSERT_ERR(finish(second) == 2);
  SOFT_ASSERT_ERR(finish(parent) == 2);
  check_cleanup(name);
}

static void test_parallel(void)
{
  char names[2][64];
  pid_t parents[2], philosophers[2][2];
  for (int i = 0; i < 2; i++)
  {
    new_session(names[i]);
    parents[i] = start_coordinator(names[i], 2, 0, 3000);
  }
  for (int i = 0; i < 2; i++)
  {
    philosophers[i][0] = start_participant(names[i], "1");
    philosophers[i][1] = start_participant(names[i], "2");
  }
  for (int i = 0; i < 2; i++)
  {
    SOFT_ASSERT_ERR(finish(philosophers[i][0]) == 0);
    SOFT_ASSERT_ERR(finish(philosophers[i][1]) == 0);
    SOFT_ASSERT_ERR(finish(parents[i]) == 0);
    check_events(names[i], 2);
    check_cleanup(names[i]);
  }
}

static void test_foreign_memory(void)
{
  char name[64], path[100];
  new_session(name);
  snprintf(path, sizeof(path), "/philosophers-session-%s", name);
  int fd = shm_open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  SOFT_ASSERT_ERR(fd >= 0);
  if (fd < 0)
  {
    return;
  }
  SOFT_ASSERT_ERR(ftruncate(fd, 1) == 0);
  SOFT_ASSERT_ERR(finish(start_participant(name, "1")) == 1);
  char *arguments[] = {(char *)coordinator, name, NULL};
  SOFT_ASSERT_ERR(finish(launch(arguments, 0)) == 1);
  struct stat info;
  SOFT_ASSERT_ERR(fstat(fd, &info) == 0 && info.st_size == 1);
  int reopened = shm_open(path, O_RDWR, 0);
  SOFT_ASSERT_ERR(reopened >= 0);
  if (reopened >= 0)
  {
    close(reopened);
  }
  close(fd);
  shm_unlink(path);
  check_cleanup(name);
}

static void test_invalid_input(void)
{
  const char *bad[] = {"", "0", "-1", "+1", " 1", "1 ", "1x", "257",
      "9999999999999999999999999999999999999"};
  char name[64];
  new_session(name);
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
  {
    SOFT_ASSERT_ERR(finish(start_participant(name, bad[i])) == 1);
  }
  const char *bad_names[] = {"", "a/b", ".", "x y", "abcdefghijklmnopqrstuvwxyz1234567"};
  for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++)
  {
    char *arguments[] = {(char *)coordinator, (char *)bad_names[i], NULL};
    SOFT_ASSERT_ERR(finish(launch(arguments, 0)) == 1);
    SOFT_ASSERT_ERR(finish(start_participant(bad_names[i], "1")) == 1);
  }
  SOFT_ASSERT_ERR(finish(start_participant(name, "1")) == 1);
  char *arguments[] = {(char *)coordinator, name, "-2", NULL};
  SOFT_ASSERT_ERR(finish(launch(arguments, 0)) == 1);
  check_cleanup(name);
}

int main(int argc, char **argv)
{
  if (argc != 3)
  {
    return 1;
  }
  coordinator = argv[1];
  participant = argv[2];
  char directory[] = "/tmp/philosophers-session-tests-XXXXXX";
  if (!mkdtemp(directory) || chdir(directory) == -1)
  {
    return 1;
  }
  test_invalid_input();
  test_normal(2);
  test_normal(5);
  test_duplicates();
  test_stop(0, SIGINT);
  test_stop(0, SIGTERM);
  test_stop(1, SIGINT);
  test_stop(1, SIGKILL);
  test_stop(1, SIGSTOP);
  test_missing(0);
  test_missing(1);
  test_foreign_semaphore();
  test_limits(0);
  test_limits(1);
  test_parallel();
  test_foreign_memory();
  printf("%s: independent session tests; logs: %s\n", fl_err ? "FAILED" : "OK", directory);
  return fl_err ? 1 : 0;
}
