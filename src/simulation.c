#include "console.h"
#include "philosophers.h"
#include "soft_assert.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef enum
{
  WAITING,
  THINKING,
  HUNGRY,
  EATING,
  DONE,
  STOPPED
} State;

typedef struct Simulation Simulation;
static void stop_simulation(Simulation *simulation, int result,
                            const char *reason);
static int check_stop(Simulation *simulation);

typedef struct
{
  State state;
  sem_t wake; /* Выдача вилок, старт и остановка будят владельца семафора */
  int cycles_done;
  int meals_started;
  long long next_event_ms; /* Когда закончится размышление или еда */
  long long hungry_since_ms;
  long long
      total_wait_ms; /* Сумма ожиданий, после которых философ получил вилки */
  long long max_wait_ms;
  long long pending_wait_ms; /* Ожидание, прерванное до получения вилок */
} Philosopher;

/* По факту это свой queue из плюсов, но в си его в стандартных либах нету,
    поэтому пришлось выкручиваться так*/
struct Simulation
{
  Parameters parameters;
  pid_t parent_pid;
  sem_t mutex; /* Защищает очередь, вилки, состояния и весь вывод */
  struct timespec started_at;
  int sem_count;
  int mutex_ready;
  int stopped;
  int result;
  char reason[128];

  /* В общей памяти храним сами данные, а не адреса из памяти родителя */
  Philosopher philosophers[MAX_PHILOSOPHERS];
  int fork_owner[MAX_PHILOSOPHERS]; /* -1 - свободна, иначе индекс владельца */
  int queue[MAX_PHILOSOPHERS];

  int queue_head;
  int queue_tail;
  int queue_size;
  int finished;
  long long now_ms;
};

static int free_simulation(Simulation *simulation)
{
  int fl_err = 0;
  for (int i = 0; i < simulation->sem_count; i++)
  {
    SOFT_ASSERT_ERR(sem_destroy(&simulation->philosophers[i].wake) == 0);
  }
  if (simulation->mutex_ready)
  {
    SOFT_ASSERT_ERR(sem_destroy(&simulation->mutex) == 0);
  }
  SOFT_ASSERT_ERR(munmap(simulation, sizeof(*simulation)) == 0);
  return fl_err == 0 ? 0 : -1;
}

static Simulation *init_simulation(const Parameters *parameters)
{
  char name[64];
  snprintf(name, sizeof(name), "/philosophers-%ld", (long)getpid());
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd == -1)
  {
    print_log("Ошибка shm_open: %s\n", strerror(errno));
    return NULL;
  }

  /* Имя больше не нужно: дети получат отображение памяти через fork */
  if (shm_unlink(name) == -1)
  {
    print_log("Ошибка shm_unlink: %s\n", strerror(errno));
    close(fd);
    return NULL;
  }

  /* Резервируем место, чтобы нехватка /dev/shm не дала SIGBUS при записи */
  int error = posix_fallocate(fd, 0, sizeof(Simulation));
  if (error != 0)
  {
    print_log("Ошибка posix_fallocate: %s\n", strerror(error));
    close(fd);
    return NULL;
  }
  Simulation *simulation = mmap(NULL, sizeof(Simulation), PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, 0);
  int map_error = errno;
  int close_result = close(fd);
  if (simulation == MAP_FAILED)
  {
    print_log("Ошибка mmap: %s\n", strerror(map_error));
    return NULL;
  }
  if (close_result == -1)
  {
    print_log("Ошибка close shared memory: %s\n", strerror(errno));
    munmap(simulation, sizeof(*simulation));
    return NULL;
  }

  simulation->parameters = *parameters;
  simulation->parent_pid = getpid();
  if (sem_init(&simulation->mutex, 1, 1) == -1)
  {
    print_log("Ошибка sem_init общего семафора: %s\n", strerror(errno));
    free_simulation(simulation);
    return NULL;
  }
  simulation->mutex_ready = 1;
  for (int i = 0; i < parameters->count; i++)
  {
    simulation->fork_owner[i] = -1;
    if (sem_init(&simulation->philosophers[i].wake, 1, 0) == -1)
    {
      print_log("Ошибка sem_init: %s\n", strerror(errno));
      free_simulation(simulation);
      return NULL;
    }
    simulation->sem_count++;
  }
  return simulation;
}

/* 0 - получили семафор, 1 - тайм-аут или сигнал, -1 - ошибка */
static int wait_semaphore(sem_t *semaphore, int milliseconds)
{
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) == -1)
  {
    return -1;
  }
  deadline.tv_nsec += (long)milliseconds * 1000000L;
  if (deadline.tv_nsec >= 1000000000L)
  {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  if (sem_timedwait(semaphore, &deadline) == 0)
  {
    return 0;
  }
  return errno == EINTR || errno == ETIMEDOUT ? 1 : -1;
}

/* Родитель проверяет детей отдельно, поэтому здесь ждут только философы */
static int lock_simulation(Simulation *simulation)
{
  int result;
  do
  {
    if (getppid() != simulation->parent_pid)
    {
      return -1;
    }
    result = wait_semaphore(&simulation->mutex, 10);
  } while (result == 1 && !received_signal());
  return result == 0 ? 0 : -1;
}

/* У каждого процесса своя копия генератора rand */
static int random_time(int minimum, int maximum)
{
  long long range = (long long)maximum - minimum + 1;
  return minimum + (int)(rand() % range);
}

static const char *state_name(State state)
{
  switch (state)
  {
  case WAITING:
    return "ожидает старта";
  case THINKING:
    return "думает";
  case HUNGRY:
    return "голоден";
  case EATING:
    return "ест";
  case DONE:
    return "завершил";
  case STOPPED:
    return "остановлен";
  }
  return "неизвестно";
}

static void print_snapshot(const Simulation *simulation, int count)
{
  print_log("[%6lld ms] Состояние:", simulation->now_ms);
  for (int i = 0; i < count; i++)
  {
    const Philosopher *philosopher = &simulation->philosophers[i];
    print_log(" P%d=%s", i + 1, state_name(philosopher->state));
    if (philosopher->state == HUNGRY)
    {
      print_log("(ждёт %lld ms)",
                simulation->now_ms - philosopher->hungry_since_ms);
    }
  }
  print_log("\nВилки:");
  for (int i = 0; i < count; i++)
  {
    if (simulation->fork_owner[i] == -1)
    {
      print_log(" F%d=свободна", i + 1);
    }
    else
    {
      print_log(" F%d=P%d", i + 1, simulation->fork_owner[i] + 1);
    }
  }
  print_log("\n\n");
}

static void start_thinking(Simulation *simulation, const Parameters *parameters,
                           int id)
{
  Philosopher *philosopher = &simulation->philosophers[id];
  int duration = random_time(parameters->think_min, parameters->think_max);
  philosopher->state = THINKING;
  philosopher->next_event_ms = simulation->now_ms + duration;
  print_log("[%6lld ms] Философ %d: начал размышление на %d ms\n",
            simulation->now_ms, id + 1, duration);
}

static void release_forks(Simulation *simulation, int id, int count)
{
  /* Смотрим на вилку слева, на вилку справа и освобождаем */
  int left = id;
  int right = (id + 1) % count;
  simulation->fork_owner[left] = -1;
  simulation->fork_owner[right] = -1;
  print_log("[%6lld ms] Философ %d: освободил вилки %d и %d\n",
            simulation->now_ms, id + 1, left + 1, right + 1);
}

/* Вызывается процессом философа под общим семафором. */
static void finish_thinking(Simulation *simulation, const Parameters *parameters,
                            int id)
{

  Philosopher *philosopher = &simulation->philosophers[id];
  philosopher->state = HUNGRY;
  philosopher->hungry_since_ms = simulation->now_ms;

  simulation->queue[simulation->queue_tail] = id;
  simulation->queue_tail = (simulation->queue_tail + 1) % parameters->count;
  simulation->queue_size++;

  print_log("[%6lld ms] Философ %d: закончил размышление и проголодался\n",
            simulation->now_ms, id + 1);
  print_log("[%6lld ms] Философ %d: запросил левую вилку %d\n",
            simulation->now_ms, id + 1, id + 1);
  print_log("[%6lld ms] Философ %d: запросил правую вилку %d\n",
            simulation->now_ms, id + 1, (id + 1) % parameters->count + 1);
}

static int wait_limit_reached(const Simulation *simulation,
                              const Parameters *parameters)
{
  for (int i = 0; i < parameters->count; i++)
  {
    const Philosopher *philosopher = &simulation->philosophers[i];
    long long waited = simulation->now_ms - philosopher->hungry_since_ms;
    if (philosopher->state == HUNGRY && waited > parameters->max_wait)
    {
      print_log("Диагностика: философ %d ждёт %lld ms, лимит %d ms.\n", i + 1,
                waited, parameters->max_wait);
      return 1;
    }
  }
  return 0;
}

static void give_forks(Simulation *simulation, const Parameters *parameters)
{
  while (simulation->queue_size > 0)
  {
    if (check_stop(simulation))
    {
      return;
    }

    /* Берем буквально самого голодного философа :(*/
    int id = simulation->queue[simulation->queue_head];
    int left = id;
    int right = (id + 1) % parameters->count;

    /* Если первый ждёт, остальные не обходят его */
    if (simulation->fork_owner[left] != -1 ||
        simulation->fork_owner[right] != -1)
    {
      break;
    }
    /* Двигаем начало очереди по кругу, не сдвигая весь массив */
    simulation->queue_head = (simulation->queue_head + 1) % parameters->count;
    simulation->queue_size--;

    /* Выдаем пару вилок */
    simulation->fork_owner[left] = id;
    simulation->fork_owner[right] = id;
    Philosopher *philosopher = &simulation->philosophers[id];

    long long waited = simulation->now_ms - philosopher->hungry_since_ms;
    philosopher->total_wait_ms += waited;
    if (waited > philosopher->max_wait_ms)
    {
      philosopher->max_wait_ms = waited;
    }

    int duration = random_time(parameters->eat_min, parameters->eat_max);
    philosopher->state = EATING;
    philosopher->meals_started++;
    philosopher->next_event_ms = simulation->now_ms + duration;

    print_log("[%6lld ms] Философ %d: получил левую вилку %d\n",
              simulation->now_ms, id + 1, left + 1);
    print_log("[%6lld ms] Философ %d: получил правую вилку %d\n",
              simulation->now_ms, id + 1, right + 1);
    print_log("[%6lld ms] Философ %d: начал есть после ожидания %lld ms "
              "на %d ms\n",
              simulation->now_ms, id + 1, waited, duration);
    
    /* Будим процесс философа, которому выдали вилки */
    if (sem_post(&philosopher->wake) == -1)
    {
      stop_simulation(simulation, 1, "ошибка sem_post при выдаче вилок");
      return;
    }
  }
}

/* Все функции работы с общим состоянием вызываются под общим семафором */
static void update_time(Simulation *simulation)
{
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
  {
    stop_simulation(simulation, 1, "ошибка clock_gettime");
    return;
  }

  if (!simulation->stopped)
  {
    long long seconds = now.tv_sec - simulation->started_at.tv_sec;
    long nanoseconds = now.tv_nsec - simulation->started_at.tv_nsec;
    if (nanoseconds < 0)
    {
      seconds--;
      nanoseconds += 1000000000L;
    }

    /* Перевод в миллисек */
    simulation->now_ms = seconds * 1000 + nanoseconds / 1000000;
  }
}

static void stop_simulation(Simulation *simulation, int result,
                            const char *reason)
{
  if (simulation->stopped)
  {
    return;
  }

  simulation->stopped = 1;
  simulation->result = result;
  snprintf(simulation->reason, sizeof(simulation->reason), "%s", reason);

  /* Будим и ожидающих вилок, и спящих */
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    if (sem_post(&simulation->philosophers[i].wake) == -1)
    {
      simulation->result = 1;
      snprintf(simulation->reason, sizeof(simulation->reason),
                "ошибка sem_post при остановке");
    }
  }
}

static int check_stop(Simulation *simulation)
{
  const Parameters *parameters = &simulation->parameters;

  update_time(simulation);
  if (simulation->stopped)
  {
    return 1;
  }

  if (received_signal())
  {
    stop_simulation(simulation, 128 + received_signal(),
                    "прерывание пользователем (SIGINT/SIGTERM)");
  }
  else if (output_failed())
  {
    stop_simulation(simulation, 1, "ошибка вывода");
  }
  else if (simulation->finished == parameters->count)
  {
    stop_simulation(simulation, 0, "все философы выполнили заданное число циклов");
  }
  else if (wait_limit_reached(simulation, parameters))
  {
    stop_simulation(simulation, 2, "превышено максимально допустимое ожидание");
  }
  else if (parameters->time_limit > 0 &&
           simulation->now_ms >= parameters->time_limit)
  {
    print_log("Диагностика: достигнут общий лимит %d ms.\n",
              parameters->time_limit);
    stop_simulation(simulation, 2, "достигнут общий лимит времени");
  }

  return simulation->stopped;
}

/* 1 - время прошло, 0 - остановка
   в обоих случаях семафор занят нами
   При -1 общий семафор уже не удерживается, процесс должен завершиться */
static int wait_until(Simulation *simulation, int id)
{
  Philosopher *philosopher = &simulation->philosophers[id];

  while (!check_stop(simulation) &&
         simulation->now_ms < philosopher->next_event_ms)
  {
    long long remaining = philosopher->next_event_ms - simulation->now_ms;

    /* Проверяем остановку каждые 10 ms, не удерживая общий семафор */
    if (remaining > 10)
    {
      remaining = 10;
    }
    if (sem_post(&simulation->mutex) == -1)
    {
      return -1;
    }
    int result = wait_semaphore(&philosopher->wake, (int)remaining);
    if (lock_simulation(simulation) == -1)
    {
      return -1;
    }
    if (result == -1)
    {
      stop_simulation(simulation, 1, "ошибка sem_timedwait");
    }
  }
  return !simulation->stopped;
}

/* Тут ожидание семафора */
static int wait_wakeup(Simulation *simulation, int id)
{
  int result;
  do
  {
    if (getppid() != simulation->parent_pid)
    {
      return -1;
    }
    result = wait_semaphore(&simulation->philosophers[id].wake, 10);
  } while (result == 1 && !received_signal());
  return result;
}

static int philosopher_main(Simulation *simulation, int id)
{
  Philosopher *philosopher = &simulation->philosophers[id];
  const Parameters *parameters = &simulation->parameters;
  srand((unsigned int)parameters->seed + (unsigned int)id);

  int result = wait_wakeup(simulation, id);
  if (lock_simulation(simulation) == -1)
  {
    return 1;
  }

  if (result == -1)
  {
    stop_simulation(simulation, 1, "ошибка ожидания при старте");
  }
  while (!check_stop(simulation))
  {

    start_thinking(simulation, parameters, id);
    result = wait_until(simulation, id);
    if (result == -1)
    {
      return 1;
    }
    if (result == 0)
    {
      break;
    }
    finish_thinking(simulation, parameters, id);

    /* Ожидание вилок не должно мешать другим процессам */
    give_forks(simulation, parameters);
    if (sem_post(&simulation->mutex) == -1)
    {
      return 1;
    }
    result = wait_wakeup(simulation, id);
    if (lock_simulation(simulation) == -1)
    {
      return 1;
    }
    if (result == -1)
    {
      stop_simulation(simulation, 1, "ошибка ожидания вилок");
    }
    result = wait_until(simulation, id);
    if (result == -1)
    {
      return 1;
    }
    if (result == 0)
    {
      break;
    }

    print_log("[%6lld ms] Философ %d: закончил есть\n",
              simulation->now_ms, id + 1);
    release_forks(simulation, id, parameters->count);

    philosopher->cycles_done++;

    /* Временное состояние, чтобы его опять не закинули в EATING */
    philosopher->state = STOPPED;

    if (philosopher->cycles_done == parameters->cycles)
    {
      philosopher->state = DONE;
      simulation->finished++;
      print_log("[%6lld ms] Философ %d: выполнил все %d циклов\n",
                simulation->now_ms, id + 1, parameters->cycles);
    }

    if (!check_stop(simulation))
    {
      give_forks(simulation, parameters);
    }
    if (philosopher->state == DONE)
    {
      break;
    }
  }
  return sem_post(&simulation->mutex) == 0 ? 0 : 1;
}

static void stop_philosophers(Simulation *simulation, int count)
{
  for (int i = 0; i < count; i++)
  {
    Philosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state == DONE)
    {
      continue;
    }
    if (philosopher->state == EATING)
    {
      print_log("[%6lld ms] Философ %d: еда прервана, цикл не засчитан\n",
                simulation->now_ms, i + 1);
      release_forks(simulation, i, count);
    }
    else if (philosopher->state == HUNGRY)
    {
      philosopher->pending_wait_ms =
          simulation->now_ms - philosopher->hungry_since_ms;

      if (philosopher->pending_wait_ms > philosopher->max_wait_ms)
      {
        philosopher->max_wait_ms = philosopher->pending_wait_ms;
      }
    }
    philosopher->state = STOPPED;
  }
  /* При гибели процесса он мог не закончить изменение владельцев вилок */
  for (int i = 0; i < count; i++)
  {
    simulation->fork_owner[i] = -1;
  }
  simulation->queue_size = 0;
}

static void print_statistics(const Simulation *simulation,
                             const Parameters *parameters)
{
  long long total_meals = 0;
  long long started_meals = 0;
  long long total_wait = 0;

  for (int i = 0; i < parameters->count; i++)
  {
    const Philosopher *philosopher = &simulation->philosophers[i];
    print_log("P%d: циклов=%d/%d, начато приёмов пищи=%d, "
              "ожидание перед начатой едой=%lld ms, "
              "незавершённое ожидание=%lld ms, максимум=%lld ms\n",
              i + 1, philosopher->cycles_done, parameters->cycles,
              philosopher->meals_started, philosopher->total_wait_ms,
              philosopher->pending_wait_ms, philosopher->max_wait_ms);
    total_meals += philosopher->cycles_done;
    started_meals += philosopher->meals_started;
    total_wait += philosopher->total_wait_ms;
  }
  print_log("Всего завершённых приёмов пищи: %lld\n", total_meals);
  if (started_meals > 0)
  {
    print_log("Среднее ожидание перед начатой едой: %.2f ms\n",
              (double)total_wait / (double)started_meals);
  }
  print_log("Время работы: %lld ms\n", simulation->now_ms);
}

/* PID обнуляем только после waitpid */
static int reap_children(pid_t children[], int count)
{
  int failed = 0;
  for (int i = 0; i < count; i++)
  {
    if (children[i] == 0)
    {
      continue;
    }
    int status = 0;
    pid_t waited = waitpid(children[i], &status, WNOHANG | WUNTRACED);
    if (waited == -1 && errno != EINTR)
    {
      int error = errno;
      dprintf(STDERR_FILENO, "Ошибка waitpid: %s\n", strerror(error));
      if (error == ECHILD)
      {
        children[i] = 0;
      }
      failed = 1;
    }
    else if (waited > 0)
    {
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      {
        dprintf(STDERR_FILENO, "Ошибка: процесс философа %d (PID %ld) "
                               "аварийно завершился или был остановлен.\n",
                i + 1, (long)children[i]);
        failed = 1;
      }
      if (!WIFSTOPPED(status))
      {
        children[i] = 0;
      }
    }
  }
  return failed;
}

static int signal_children(const pid_t children[], int count, int number)
{
  int fl_err = 0;
  for (int i = 0; i < count; i++)
  {
    if (children[i] > 0)
    {
      int result = kill(children[i], number);
      SOFT_ASSERT_ERR(result == 0 || errno == ESRCH);
    }
  }
  return fl_err == 0 ? 0 : -1;
}

static int wait_children(pid_t children[], int count)
{
  int fl_err = 0;
  for (int attempt = 0; attempt < 200; attempt++)
  {
    SOFT_ASSERT_ERR(reap_children(children, count) == 0);
    int remaining = 0;
    for (int i = 0; i < count; i++)
    {
      remaining += children[i] > 0;
    }
    if (remaining == 0)
    {
      return fl_err == 0 ? 0 : -1;
    }
    struct timespec pause = {0, 5000000L};
    nanosleep(&pause, NULL);
  }

  /* Зависший или остановленный процесс не должен удерживать приложение */
  dprintf(STDERR_FILENO, "Ошибка: дети не завершились, отправляем SIGKILL.\n");
  signal_children(children, count, SIGKILL);
  for (int i = 0; i < count; i++)
  {
    if (children[i] == 0)
    {
      continue;
    }
    pid_t waited;
    do
    {
      waited = waitpid(children[i], NULL, 0);
    } while (waited == -1 && errno == EINTR);
    SOFT_ASSERT_ERR(waited == children[i]);
    children[i] = 0;
  }
  return -1;
}

int run_simulation(const Parameters *parameters)
{
  Simulation *simulation = init_simulation(parameters);
  if (simulation == NULL)
  {
    return received_signal() ? 128 + received_signal() : 1;
  }
  if (wait_semaphore(&simulation->mutex, 10) != 0)
  {
    print_log("Ошибка ожидания общего семафора при запуске.\n");
    free_simulation(simulation);
    return received_signal() ? 128 + received_signal() : 1;
  }

  print_log("=== Обедающие философы: процессы и неименованные POSIX семафоры ===\n"
            "Философов/вилок: %d, циклов: %d, стратегия: fifo.\n"
            "Размышление: %d..%d ms, еда: %d..%d ms.\n"
            "Лимит ожидания: %d ms, общий лимит: %d ms (0 — отключён).\n"
            "Период показа: %d ms, seed: %d. Лог: philosophers.log\n\n",
            parameters->count, parameters->cycles, parameters->think_min,
            parameters->think_max, parameters->eat_min, parameters->eat_max,
            parameters->max_wait, parameters->time_limit,
            parameters->display_delay, parameters->seed);

  srand((unsigned int)parameters->seed);
  pid_t children[MAX_PHILOSOPHERS] = {0};
  int created = 0;
  for (int i = 0; i < parameters->count; i++)
  {
    if (received_signal() || output_failed())
    {
      break;
    }
    pid_t child = fork();
    if (child == -1)
    {
      print_log("Ошибка fork для философа %d: %s\n", i + 1, strerror(errno));
      break;
    }
    if (child == 0)
    {
      _exit(philosopher_main(simulation, i));
    }
    children[i] = child;
    created++;
    print_log("Процесс философа %d: PID %ld.\n", i + 1, (long)child);
  }

  if (clock_gettime(CLOCK_MONOTONIC, &simulation->started_at) == -1)
  {
    stop_simulation(simulation, 1, "ошибка clock_gettime при старте");
  }
  else if (received_signal())
  {
    stop_simulation(simulation, 128 + received_signal(),
                    "прерывание пользователем (SIGINT/SIGTERM)");
  }
  else if (created != parameters->count)
  {
    stop_simulation(simulation, 1, "не удалось создать все процессы");
  }
  else if (!check_stop(simulation))
  {
    for (int i = 0; i < created; i++)
    {
      if (sem_post(&simulation->philosophers[i].wake) == -1)
      {
        stop_simulation(simulation, 1, "ошибка sem_post при старте");
        break;
      }
    }
  }

  int emergency = sem_post(&simulation->mutex) == -1;
  long long next_snapshot = 0;
  while (!emergency)
  {
    if (reap_children(children, created))
    {
      emergency = 1;
      break;
    }
    int locked = wait_semaphore(&simulation->mutex, 10);
    if (locked == -1 || (locked == 1 && received_signal()))
    {
      emergency = 1;
      break;
    }
    if (locked == 1)
    {
      continue;
    }
    int stopped = check_stop(simulation);
    if (!stopped && parameters->display_delay > 0 &&
        simulation->now_ms >= next_snapshot)
    {
      print_snapshot(simulation, parameters->count);
      next_snapshot = simulation->now_ms + parameters->display_delay;
    }
    emergency = sem_post(&simulation->mutex) == -1;
    if (stopped)
    {
      break;
    }
    pause_display(5);
  }

  if (emergency)
  {
    /* Общий семафор мог остаться у погибшего процесса, его больше не ждём */
    signal_children(children, created, SIGTERM);
  }
  int wait_error = wait_children(children, created);
  if (emergency || wait_error == -1)
  {
    /* Теперь детей нет, поэтому состояние можно исправить без семафора */
    update_time(simulation);
    simulation->stopped = 1;
    simulation->result = received_signal() ? 128 + received_signal() : 1;
    snprintf(simulation->reason, sizeof(simulation->reason), "%s",
              received_signal() ? "прерывание пользователем (SIGINT/SIGTERM)"
                                : "аварийное завершение дочернего процесса или ошибка синхронизации");
  }

  print_log("\n=== Итоги моделирования ===\nПричина завершения: %s.\n",
            simulation->reason);
  stop_philosophers(simulation, parameters->count);
  print_snapshot(simulation, parameters->count);
  print_statistics(simulation, parameters);
  if (!output_failed())
  {
    print_log("Лог: philosophers.log\n");
  }
  
  int result = output_failed() ? 1 : simulation->result;
  if (free_simulation(simulation) == -1)
  {
    result = 1;
  }
  return result;
}
