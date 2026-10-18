#include "soft_assert.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static int fl_err = 0;
static char binary[PATH_MAX];

/* Оба выражения вычисляем только один раз. long long подходит и для времени. */
#define SOFT_ASSERT_EQ_ERR(a, b)                                          \
  do                                                                      \
  {                                                                       \
    long long actual = (a);                                               \
    long long expected = (b);                                             \
    if (actual != expected)                                               \
    {                                                                     \
      fprintf(stderr, "FAIL %s:%d: %s == %s (got %lld, expected %lld)\n", \
              __FILE__, __LINE__, #a, #b, actual, expected);              \
      fl_err++;                                                           \
    }                                                                     \
  } while (0)

enum
{
  NORMAL,
  LOG_DIRECTORY,
  LOG_FULL,
  CLOSED_OUTPUT
};

typedef struct
{
  char *output;
  char *errors;
  int log_exists;
} TestRun;

enum
{
  NOT_STARTED,
  THINKING,
  HUNGRY,
  EATING,
  FINISHED,
  DONE,
  STOPPED
};

typedef struct
{
  int state;
  int cycles_done;
  int meals_started;
  int requested_left;
  int requested_right;
  long long deadline;
  long long hungry_since;
  long long total_wait;
  long long pending_wait;
  long long max_wait;
} CheckedPhilosopher;

/* Читаем весь файл в динамический массив, оставляя место для '\0'. */
static char *read_file(const char *name)
{
  FILE *file = fopen(name, "r");
  if (file == NULL)
  {
    return NULL;
  }
  int result = fseek(file, 0, SEEK_END);
  long size = ftell(file);
  SOFT_ASSERT_ERR(result == 0 && size >= 0);
  if (result != 0 || size < 0)
  {
    fclose(file);
    return NULL;
  }
  rewind(file);
  char *text = calloc((size_t)size + 1, 1);
  SOFT_ASSERT_ERR(text != NULL);
  if (text != NULL)
  {
    size_t length = fread(text, 1, (size_t)size, file);
    SOFT_ASSERT_ERR(length == (size_t)size);
  }
  SOFT_ASSERT_ERR(fclose(file) == 0);
  return text;
}

static void free_run(TestRun run)
{
  free(run.output);
  free(run.errors);
}

/* Каждый запуск получает отдельный каталог, чтобы не затереть журнал пользователя. */
static TestRun run_case(char *const arguments[], int expected_code,
                        int signal_number, int mode)
{
  TestRun run = {0};
  char directory[] = "/tmp/philosophers-tests-XXXXXX";
  char screen_path[PATH_MAX];
  char errors_path[PATH_MAX];
  char log_path[PATH_MAX];
  char *created = mkdtemp(directory);
  SOFT_ASSERT_ERR(created != NULL);
  if (created == NULL)
  {
    return run;
  }
  snprintf(screen_path, sizeof(screen_path), "%s/stdout.txt", directory);
  snprintf(errors_path, sizeof(errors_path), "%s/stderr.txt", directory);
  snprintf(log_path, sizeof(log_path), "%s/philosophers.log", directory);

  if (mode == LOG_DIRECTORY)
  {
    SOFT_ASSERT_ERR(mkdir(log_path, 0700) == 0);
  }
  else if (mode == LOG_FULL)
  {
    SOFT_ASSERT_ERR(symlink("/dev/full", log_path) == 0);
  }

  int count = 0;
  while (arguments[count] != NULL)
  {
    count++;
  }
  char **command = calloc((size_t)count + 2, sizeof(char *));
  SOFT_ASSERT_ERR(command != NULL);
  if (command == NULL)
  {
    if (mode == LOG_DIRECTORY)
    {
      rmdir(log_path);
    }
    else
    {
      unlink(log_path);
    }
    rmdir(directory);
    return run;
  }
  command[0] = binary;
  for (int i = 0; i < count; i++)
  {
    command[i + 1] = arguments[i];
  }

  int channel[2] = {-1, -1};
  if (mode == CLOSED_OUTPUT)
  {
    int result = pipe(channel);
    SOFT_ASSERT_ERR(result == 0);
    if (result != 0)
    {
      free(command);
      rmdir(directory);
      return run;
    }
  }

  pid_t child = fork();
  SOFT_ASSERT_ERR(child >= 0);
  if (child == 0)
  {
    /* Если проверяемая программа зависнет, тест завершит её через 5 секунд. */
    alarm(5);
    if (chdir(directory) != 0)
    {
      _exit(125);
    }
    int screen = open("stdout.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int errors = open("stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (screen < 0 || errors < 0)
    {
      _exit(125);
    }
    int output = mode == CLOSED_OUTPUT ? channel[1] : screen;
    if (dup2(output, STDOUT_FILENO) == -1 ||
        dup2(errors, STDERR_FILENO) == -1)
    {
      _exit(125);
    }
    close(screen);
    close(errors);
    if (mode == CLOSED_OUTPUT)
    {
      close(channel[0]);
      close(channel[1]);
    }
    execv(binary, command);
    _exit(125); /* Сюда попадаем, только если execv не смог запустить программу. */
  }
  free(command);
  if (mode == CLOSED_OUTPUT)
  {
    close(channel[0]);
    close(channel[1]); /* У канала не осталось читателей. */
  }

  if (child > 0)
  {
    if (signal_number != 0)
    {
      /* Отправляем сигнал после начала еды, а не во время запуска процесса. */
      int ready = 0;
      struct timespec pause = {0, 10000000L};
      for (int i = 0; i < 300 && !ready; i++)
      {
        char *log = read_file(log_path);
        ready = log != NULL && strstr(log, "начал есть") != NULL;
        free(log);
        if (!ready)
        {
          nanosleep(&pause, NULL);
        }
      }
      SOFT_ASSERT_ERR(ready);
      SOFT_ASSERT_ERR(kill(child, ready ? signal_number : SIGKILL) == 0);
    }
    int status = 0;
    pid_t waited;
    do
    {
      waited = waitpid(child, &status, 0);
    } while (waited == -1 && errno == EINTR);
    SOFT_ASSERT_EQ_ERR(waited, child);
    SOFT_ASSERT_ERR(WIFEXITED(status));
    if (waited == child && WIFEXITED(status))
    {
      SOFT_ASSERT_EQ_ERR(WEXITSTATUS(status), expected_code);
    }
    run.output = read_file(screen_path);
    run.errors = read_file(errors_path);
    SOFT_ASSERT_ERR(run.output != NULL && run.errors != NULL);
    run.log_exists = access(log_path, F_OK) == 0;
    if (run.log_exists && (mode == NORMAL || mode == CLOSED_OUTPUT))
    {
      char *log = read_file(log_path);
      SOFT_ASSERT_ERR(log != NULL);
      if (log != NULL && run.output != NULL)
      {
        if (mode == CLOSED_OUTPUT)
        {
          free(run.output);
          run.output = log; /* После ошибки stdout проверяем события по журналу. */
        }
        else
        {
          SOFT_ASSERT_ERR(strcmp(run.output, log) == 0);
          free(log);
        }
      }
      else
      {
        free(log);
      }
    }
  }

  unlink(screen_path);
  unlink(errors_path);
  if (mode == LOG_DIRECTORY)
  {
    rmdir(log_path);
  }
  else
  {
    unlink(log_path);
  }
  SOFT_ASSERT_ERR(rmdir(directory) == 0);
  return run;
}

/* Восстанавливаем состояние независимо от программы, только по её сообщениям. */
static int check_trace(const char *output, int count, int cycles, int complete)
{
  SOFT_ASSERT_ERR(output != NULL);
  if (output == NULL)
  {
    return 0;
  }
  int think_min, think_max, eat_min, eat_max, wait_limit;
  long long final_time;
  const char *ranges = strstr(output, "Размышление:");
  const char *limit = strstr(output, "Лимит ожидания:");
  const char *time = strstr(output, "Модельное время:");
  SOFT_ASSERT_ERR(ranges != NULL && limit != NULL && time != NULL);
  if (ranges == NULL || limit == NULL || time == NULL)
  {
    return 0;
  }
  int fields = sscanf(ranges, "Размышление: %d..%d ms, еда: %d..%d ms",
                      &think_min, &think_max, &eat_min, &eat_max);
  int limit_fields = sscanf(limit, "Лимит ожидания: %d ms", &wait_limit);
  int time_fields = sscanf(time, "Модельное время: %lld ms", &final_time);
  SOFT_ASSERT_ERR(fields == 4 && limit_fields == 1 && time_fields == 1);
  if (fields != 4 || limit_fields != 1 || time_fields != 1)
  {
    return 0;
  }

  CheckedPhilosopher *philosophers = calloc((size_t)count, sizeof(*philosophers));
  int *owners = malloc((size_t)count * sizeof(int));
  int *queue = malloc((size_t)count * sizeof(int));
  char *copy = strdup(output);
  SOFT_ASSERT_ERR(philosophers != NULL && owners != NULL && queue != NULL && copy != NULL);
  if (philosophers == NULL || owners == NULL || queue == NULL || copy == NULL)
  {
    free(philosophers);
    free(owners);
    free(queue);
    free(copy);
    return 0;
  }
  for (int i = 0; i < count; i++)
  {
    owners[i] = -1;
  }
  int queue_size = 0;
  int max_eaters = 0;
  int eaters = 0;
  long long last_time = 0;
  char *position;
  char *line = strtok_r(copy, "\n", &position);
  while (line != NULL)
  {
    long long now;
    int number;
    int offset = 0;
    sscanf(line, "[%lld ms] Философ %d: %n", &now, &number, &offset);
    if (offset != 0)
    {
      SOFT_ASSERT_ERR(number >= 1 && number <= count);
      if (number < 1 || number > count)
      {
        line = strtok_r(NULL, "\n", &position);
        continue;
      }
      int id = number - 1;
      int right = (id + 1) % count;
      CheckedPhilosopher *philosopher = &philosophers[id];
      const char *text = line + offset;
      int duration = 0;
      int fork_number = 0;
      long long waited = 0;
      SOFT_ASSERT_ERR(now >= last_time && now <= final_time);
      last_time = now;

      if (sscanf(text, "начал размышление на %d ms", &duration) == 1)
      {
        SOFT_ASSERT_ERR(philosopher->state == NOT_STARTED || philosopher->state == FINISHED);
        for (int i = 0; i < count; i++)
        {
          SOFT_ASSERT_ERR(owners[i] != id);
        }
        SOFT_ASSERT_ERR(duration >= think_min && duration <= think_max);
        philosopher->state = THINKING;
        philosopher->deadline = now + duration;
      }
      else if (strncmp(text, "закончил размышление", strlen("закончил размышление")) == 0)
      {
        SOFT_ASSERT_EQ_ERR(philosopher->state, THINKING);
        SOFT_ASSERT_EQ_ERR(now, philosopher->deadline);
        for (int i = 0; i < queue_size; i++)
        {
          SOFT_ASSERT_ERR(queue[i] != id);
        }
        SOFT_ASSERT_ERR(queue_size < count);
        if (queue_size < count)
        {
          queue[queue_size++] = id;
        }
        philosopher->state = HUNGRY;
        philosopher->hungry_since = now;
        philosopher->requested_left = 0;
        philosopher->requested_right = 0;
      }
      else if (sscanf(text, "запросил левую вилку %d", &fork_number) == 1 ||
               sscanf(text, "запросил правую вилку %d", &fork_number) == 1 ||
               sscanf(text, "запросил вилку %d", &fork_number) == 1)
      {
        int fork_id = fork_number - 1;
        SOFT_ASSERT_EQ_ERR(philosopher->state, HUNGRY);
        SOFT_ASSERT_ERR(fork_id == id || fork_id == right);
        if (fork_id == id)
        {
          philosopher->requested_left = 1;
        }
        else if (fork_id == right)
        {
          philosopher->requested_right = 1;
        }
      }
      else if (sscanf(text, "получил левую вилку %d", &fork_number) == 1 ||
               sscanf(text, "получил правую вилку %d", &fork_number) == 1 ||
               sscanf(text, "получил вилку %d", &fork_number) == 1)
      {
        int fork_id = fork_number - 1;
        SOFT_ASSERT_EQ_ERR(philosopher->state, HUNGRY);
        SOFT_ASSERT_ERR(philosopher->requested_left && philosopher->requested_right);
        SOFT_ASSERT_ERR(queue_size > 0 && queue[0] == id);
        SOFT_ASSERT_ERR(fork_id == id || fork_id == right);
        if (fork_id == id || fork_id == right)
        {
          SOFT_ASSERT_EQ_ERR(owners[fork_id], -1);
          owners[fork_id] = id;
        }
      }
      else if (sscanf(text, "начал есть после ожидания %lld ms на %d ms",
                      &waited, &duration) == 2)
      {
        SOFT_ASSERT_EQ_ERR(philosopher->state, HUNGRY);
        for (int i = 0; i < count; i++)
        {
          SOFT_ASSERT_ERR((owners[i] == id) == (i == id || i == right));
        }
        SOFT_ASSERT_ERR(queue_size > 0 && queue[0] == id);
        if (queue_size > 0)
        {
          queue_size--;
          memmove(queue, queue + 1, (size_t)queue_size * sizeof(int));
        }
        SOFT_ASSERT_EQ_ERR(waited, now - philosopher->hungry_since);
        SOFT_ASSERT_ERR(waited >= 0 && waited <= wait_limit);
        SOFT_ASSERT_ERR(duration >= eat_min && duration <= eat_max);
        philosopher->total_wait += waited;
        if (waited > philosopher->max_wait)
        {
          philosopher->max_wait = waited;
        }
        philosopher->meals_started++;
        philosopher->state = EATING;
        philosopher->deadline = now + duration;
        eaters++;
        if (eaters > max_eaters)
        {
          max_eaters = eaters;
        }
      }
      else if (strcmp(text, "закончил есть") == 0)
      {
        SOFT_ASSERT_EQ_ERR(philosopher->state, EATING);
        SOFT_ASSERT_EQ_ERR(now, philosopher->deadline);
        philosopher->cycles_done++;
        philosopher->state = FINISHED;
        eaters--;
      }
      else if (strncmp(text, "еда прервана", strlen("еда прервана")) == 0)
      {
        SOFT_ASSERT_EQ_ERR(philosopher->state, EATING);
        philosopher->state = STOPPED;
        eaters--;
      }
      else if (strncmp(text, "освободил вилки", strlen("освободил вилки")) == 0)
      {
        int left_number = 0;
        int right_number = 0;
        SOFT_ASSERT_EQ_ERR(sscanf(text, "освободил вилки %d и %d", &left_number, &right_number), 2);
        SOFT_ASSERT_EQ_ERR(left_number, id + 1);
        SOFT_ASSERT_EQ_ERR(right_number, right + 1);
        SOFT_ASSERT_EQ_ERR(owners[id], id);
        SOFT_ASSERT_EQ_ERR(owners[right], id);
        owners[id] = -1;
        owners[right] = -1;
      }
      else if (strncmp(text, "выполнил все", strlen("выполнил все")) == 0)
      {
        SOFT_ASSERT_EQ_ERR(philosopher->cycles_done, cycles);
        SOFT_ASSERT_EQ_ERR(philosopher->state, FINISHED);
        SOFT_ASSERT_EQ_ERR(owners[id], -1);
        SOFT_ASSERT_EQ_ERR(owners[right], -1);
        philosopher->state = DONE;
      }
    }
    line = strtok_r(NULL, "\n", &position);
  }
  free(copy);

  long long total_meals = 0;
  long long started_meals = 0;
  long long total_wait = 0;
  for (int i = 0; i < count; i++)
  {
    CheckedPhilosopher *philosopher = &philosophers[i];
    SOFT_ASSERT_EQ_ERR(owners[i], -1);
    if (philosopher->state == HUNGRY)
    {
      philosopher->pending_wait = final_time - philosopher->hungry_since;
      if (philosopher->pending_wait > philosopher->max_wait)
      {
        philosopher->max_wait = philosopher->pending_wait;
      }
    }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "\nP%d: циклов=", i + 1);
    const char *stats = strstr(output, prefix);
    SOFT_ASSERT_ERR(stats != NULL);
    if (stats != NULL)
    {
      int printed_id = 0, done = 0, target = 0, meals = 0;
      long long wait = 0, pending = 0, maximum = 0;
      int read = sscanf(stats + 1,
                        "P%d: циклов=%d/%d, начато приёмов пищи=%d, "
                        "ожидание перед начатой едой=%lld ms, "
                        "незавершённое ожидание=%lld ms, максимум=%lld ms",
                        &printed_id, &done, &target, &meals, &wait, &pending, &maximum);
      SOFT_ASSERT_EQ_ERR(read, 7);
      SOFT_ASSERT_EQ_ERR(printed_id, i + 1);
      SOFT_ASSERT_EQ_ERR(done, philosopher->cycles_done);
      SOFT_ASSERT_EQ_ERR(target, cycles);
      SOFT_ASSERT_EQ_ERR(meals, philosopher->meals_started);
      SOFT_ASSERT_EQ_ERR(wait, philosopher->total_wait);
      SOFT_ASSERT_EQ_ERR(pending, philosopher->pending_wait);
      SOFT_ASSERT_EQ_ERR(maximum, philosopher->max_wait);
    }
    if (complete)
    {
      SOFT_ASSERT_EQ_ERR(philosopher->cycles_done, cycles);
      SOFT_ASSERT_EQ_ERR(philosopher->state, DONE);
    }
    total_meals += philosopher->cycles_done;
    started_meals += philosopher->meals_started;
    total_wait += philosopher->total_wait;
  }
  const char *total = strstr(output, "Всего завершённых приёмов пищи:");
  SOFT_ASSERT_ERR(total != NULL);
  if (total != NULL)
  {
    long long meals = 0;
    SOFT_ASSERT_EQ_ERR(sscanf(total, "Всего завершённых приёмов пищи: %lld", &meals), 1);
    SOFT_ASSERT_EQ_ERR(meals, total_meals);
  }
  if (started_meals > 0)
  {
    const char *mean = strstr(output, "Среднее ожидание перед начатой едой:");
    SOFT_ASSERT_ERR(mean != NULL);
    if (mean != NULL)
    {
      double printed = 0;
      SOFT_ASSERT_EQ_ERR(sscanf(mean, "Среднее ожидание перед начатой едой: %lf", &printed), 1);
      double difference = printed - (double)total_wait / (double)started_meals;
      SOFT_ASSERT_ERR(difference >= -0.006 && difference <= 0.006);
    }
  }
  if (complete)
  {
    SOFT_ASSERT_EQ_ERR(queue_size, 0);
    SOFT_ASSERT_ERR(strstr(output, "все философы выполнили") != NULL);
  }
  SOFT_ASSERT_EQ_ERR(eaters, 0);
  free(philosophers);
  free(owners);
  free(queue);
  return max_eaters;
}

/* Обычный запуск: проверяем код, совпадение журнала и все события. */
static TestRun run_model(char *const arguments[], int code)
{
  TestRun run = run_case(arguments, code, 0, NORMAL);
  SOFT_ASSERT_ERR(run.log_exists);
  int count = arguments[0] == NULL ? 5 : atoi(arguments[0]);
  int cycles = arguments[0] == NULL || arguments[1] == NULL ? 3 : atoi(arguments[1]);
  check_trace(run.output, count, cycles, code == 0);
  return run;
}

static void check_contains(const char *text, const char *part)
{
  SOFT_ASSERT_ERR(text != NULL && strstr(text, part) != NULL);
}

static void test_defaults(void)
{
  char *arguments[] = {NULL};
  free_run(run_model(arguments, 0));
}

static void test_help(void)
{
  char *options[] = {"--help", "-h"};
  for (int i = 0; i < 2; i++)
  {
    char *arguments[] = {options[i], NULL};
    TestRun run = run_case(arguments, 0, 0, NORMAL);
    check_contains(run.output, "Использование");
    SOFT_ASSERT_ERR(!run.log_exists);
    free_run(run);
  }
}

static void test_exact_durations(void)
{
  char *arguments[] = {"2", "1", "3", "3", "7", "7", "7", "0", "42", NULL};
  TestRun run = run_model(arguments, 0);
  check_contains(run.output, "Модельное время: 17 ms");
  free_run(run);
}

static void test_zero_thinking(void)
{
  char *arguments[] = {"2", "2", "0", "0", "7", "7", "7", "0", "42", NULL};
  TestRun run = run_model(arguments, 0);
  check_contains(run.output, "[     0 ms] Философ 1: начал есть");
  free_run(run);
}

static void test_wait_boundary(void)
{
  char *arguments[] = {"2", "1", "0", "0", "200", "200", "200", "0", "42", NULL};
  free_run(run_model(arguments, 0));
}

static void test_wait_timeout(void)
{
  char *arguments[] = {"2", "1", "0", "0", "200", "200", "150", "0", "42", NULL};
  TestRun run = run_model(arguments, 2);
  check_contains(run.output, "философ 2 ждёт 151 ms");
  SOFT_ASSERT_ERR(run.output != NULL && strstr(run.output, "Философ 2: получил") == NULL);
  free_run(run);
}

static void test_global_timeout(void)
{
  char *arguments[] = {"2", "1", "0", "0", "200", "200", "1000", "0", "42", "fifo", "50", NULL};
  TestRun run = run_model(arguments, 2);
  check_contains(run.output, "достигнут общий лимит");
  check_contains(run.output, "Модельное время: 50 ms");
  free_run(run);
}

static void test_completion_boundary(void)
{
  char *arguments[] = {"2", "1", "0", "0", "7", "7", "7", "0", "42", "fifo", "14", NULL};
  TestRun run = run_model(arguments, 0);
  check_contains(run.output, "Модельное время: 14 ms");
  free_run(run);
}

static void test_many_philosophers(void)
{
  char *arguments[] = {"137", "2", "0", "0", "1", "1", "1000", "0", "42", NULL};
  free_run(run_model(arguments, 0));
}

static void test_long_times(void)
{
  char *arguments[] = {"2", "1", "2147483647", "2147483647", "1", "1", "1", "0", "42", "fifo", "0", NULL};
  TestRun run = run_model(arguments, 0);
  check_contains(run.output, "Модельное время: 2147483649 ms");
  free_run(run);
}

static void test_wide_time_range(void)
{
  char *arguments[] = {"2", "1", "0", "2147483647", "1", "1", "2147483647", "0", "42", "fifo", "0", NULL};
  free_run(run_model(arguments, 0));
}

static void test_large_cycle_count(void)
{
  char *arguments[] = {"2", "2147483647", "0", "0", "1", "1", "1", "0", "42", "fifo", "2", NULL};
  free_run(run_model(arguments, 2));
}

static void test_statistics_overflow(void)
{
  char *arguments[] = {"2", "2147483647", "2147483647", "2147483647", "2147483647", "2147483647", "2147483647", "0", "42", NULL};
  TestRun run = run_case(arguments, 1, 0, NORMAL);
  SOFT_ASSERT_ERR(!run.log_exists);
  SOFT_ASSERT_ERR(run.output != NULL && strstr(run.output, "начал размышление") == NULL);
  free_run(run);
}

static void test_reproducible(void)
{
  char *arguments[] = {"7", "3", "0", "30", "1", "20", "10000", "0", "123", NULL};
  TestRun first = run_model(arguments, 0);
  TestRun second = run_model(arguments, 0);
  SOFT_ASSERT_ERR(first.output != NULL && second.output != NULL &&
                  strcmp(first.output, second.output) == 0);
  free_run(first);
  free_run(second);
}

static void test_non_neighbours(void)
{
  char *arguments[] = {"5", "3", "0", "30", "20", "20", "1000", "0", "42", NULL};
  TestRun run = run_case(arguments, 0, 0, NORMAL);
  SOFT_ASSERT_ERR(check_trace(run.output, 5, 3, 1) >= 2);
  free_run(run);
}

static void test_various_inputs(void)
{
  srand(42);
  for (int i = 0; i < 32; i++)
  {
    char count[16], cycles[16], seed[16];
    snprintf(count, sizeof(count), "%d", 2 + rand() % 8);
    snprintf(cycles, sizeof(cycles), "%d", 1 + rand() % 5);
    snprintf(seed, sizeof(seed), "%d", rand() % 1000);
    char *arguments[] = {count, cycles, "0", "30", "1", "30", "10000", "0", seed, "fifo", "0", NULL};
    free_run(run_model(arguments, 0));
  }
}

static void test_invalid_inputs(void)
{
  char *bad_numbers[] = {"abc", "", "2x", "999999999999999999999999999"};
  for (int i = 0; i < 4; i++)
  {
    char *arguments[] = {bad_numbers[i], NULL};
    free_run(run_case(arguments, 1, 0, NORMAL));
  }
  int indexes[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 4, 4, 6, 7, 7, 8, 8};
  char *values[] = {"0", "1", "2147483648", "4294967298", "0", "2147483648",
                    "-1", "1", "4294967295", "0", "2", "0", "-1", "2147483648", "-1", "2147483648"};
  for (int i = 0; i < 16; i++)
  {
    char *arguments[] = {"2", "1", "0", "0", "1", "1", "1000", "0", "42", NULL};
    arguments[indexes[i]] = values[i];
    free_run(run_case(arguments, 1, 0, NORMAL));
  }
  char *bad_strategy[] = {"2", "1", "0", "0", "1", "1", "1000", "0", "42", "unknown", NULL};
  char *negative_limit[] = {"2", "1", "0", "0", "1", "1", "1000", "0", "42", "fifo", "-1", NULL};
  char *large_limit[] = {"2", "1", "0", "0", "1", "1", "1000", "0", "42", "fifo", "2147483648", NULL};
  char *extra_argument[] = {"2", "1", "0", "0", "1", "1", "1000", "0", "42", "fifo", "10", "1", NULL};
  free_run(run_case(bad_strategy, 1, 0, NORMAL));
  free_run(run_case(negative_limit, 1, 0, NORMAL));
  free_run(run_case(large_limit, 1, 0, NORMAL));
  free_run(run_case(extra_argument, 1, 0, NORMAL));
}

static void test_signals(void)
{
  int signals[] = {SIGINT, SIGTERM};
  for (int i = 0; i < 2; i++)
  {
    char *arguments[] = {"2", "100", "0", "0", "200", "200", "1000", "10000", "42", "fifo", "0", NULL};
    TestRun run = run_case(arguments, 128 + signals[i], signals[i], NORMAL);
    check_trace(run.output, 2, 100, 0);
    check_contains(run.output, "прерывание пользователем");
    check_contains(run.output, "Модельное время: 0 ms");
    free_run(run);
  }
}

static void test_soft_assert_diagnostics(void)
{
  char *arguments[] = {"1", "0", "2", "1", "0", "1", "0", "0", "42", NULL};
  TestRun run = run_case(arguments, 1, 0, NORMAL);
  int failures = 0;
  if (run.errors != NULL)
  {
    const char *line = run.errors;
    while (line != NULL)
    {
      if (strncmp(line, "FAIL ", 5) == 0)
      {
        failures++;
      }
      line = strchr(line, '\n');
      if (line != NULL)
      {
        line++;
      }
    }
  }
  SOFT_ASSERT_EQ_ERR(failures, 5);
  SOFT_ASSERT_ERR(run.output != NULL && run.output[0] == '\0');
  SOFT_ASSERT_ERR(!run.log_exists);
  free_run(run);
}

static void test_log_open_error(void)
{
  char *arguments[] = {NULL};
  TestRun run = run_case(arguments, 1, 0, LOG_DIRECTORY);
  check_contains(run.errors, "Ошибка открытия лога");
  free_run(run);
}

static void test_log_write_error(void)
{
  char *arguments[] = {NULL};
  TestRun run = run_case(arguments, 1, 0, LOG_FULL);
  check_contains(run.errors, "Ошибка записи");
  free_run(run);
}

static void test_closed_output(void)
{
  char *arguments[] = {"100", "100", "0", "0", "1", "1", "1000", "0", NULL};
  TestRun run = run_case(arguments, 1, 0, CLOSED_OUTPUT);
  check_contains(run.errors, "Ошибка записи");
  check_trace(run.output, 100, 100, 0);
  free_run(run);
}

int main(int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "Использование: %s путь_к_программе\n", argv[0]);
    return 1;
  }
  int length;
  if (argv[1][0] == '/')
  {
    length = snprintf(binary, sizeof(binary), "%s", argv[1]);
  }
  else
  {
    char directory[PATH_MAX];
    SOFT_ASSERT_ERR(getcwd(directory, sizeof(directory)) != NULL);
    if (fl_err != 0)
    {
      return 1;
    }
    length = snprintf(binary, sizeof(binary), "%s/%s", directory, argv[1]);
  }
  SOFT_ASSERT_ERR(length >= 0 && (size_t)length < sizeof(binary));
  SOFT_ASSERT_ERR(access(binary, X_OK) == 0);
  if (fl_err != 0)
  {
    return 1;
  }

  test_defaults();
  test_help();
  test_exact_durations();
  test_zero_thinking();
  test_wait_boundary();
  test_wait_timeout();
  test_global_timeout();
  test_completion_boundary();
  test_many_philosophers();
  test_long_times();
  test_wide_time_range();
  test_large_cycle_count();
  test_statistics_overflow();
  test_reproducible();
  test_non_neighbours();
  test_various_inputs();
  test_invalid_inputs();
  test_signals();
  test_soft_assert_diagnostics();
  test_log_open_error();
  test_log_write_error();
  test_closed_output();

  if (fl_err == 0)
  {
    puts("OK: all 22 tests passed");
    return 0;
  }
  fprintf(stderr, "FAILED: %d checks\n", fl_err);
  return 1;
}
