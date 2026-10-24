#include "console.h"
#include "philosophers.h"
#include "soft_assert.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  int id;
  Simulation *simulation;
  pthread_t thread;
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
  const Parameters *parameters;
  pthread_mutex_t mutex; /* Защищает очередь, вилки, состояния и весь вывод */
  struct timespec started_at;
  int sem_count;
  int stopped;
  int result;
  const char *reason;
  Philosopher *philosophers;
  int *fork_owner; /* -1 - свободна, иначе индекс владельца */
  int *queue;      /* Ссылка на массив, содержащий индексы голодных */
  int queue_head;
  int queue_tail;
  int queue_size;
  int finished;
  long long now_ms;
};

static int init_simulation(Simulation *simulation, int count)
{
  int fl_err = 0;
  size_t size = (size_t)count;

  /* проверяем размер до начала всего */
  SOFT_ASSERT_ERR(size <= SIZE_MAX / sizeof(Philosopher));
  SOFT_ASSERT_ERR(size <= SIZE_MAX / sizeof(int));
  if (fl_err != 0)
  {
    print_log("Ошибка: размер массивов не помещается в size_t.\n");
    return -1;
  }

  simulation->philosophers = calloc(size, sizeof(Philosopher));
  simulation->fork_owner = calloc(size, sizeof(int));
  simulation->queue = calloc(size, sizeof(int));
  SOFT_ASSERT_ERR(simulation->philosophers != NULL);
  SOFT_ASSERT_ERR(simulation->fork_owner != NULL);
  SOFT_ASSERT_ERR(simulation->queue != NULL);
  if (fl_err != 0)
  {
    print_log("Ошибка: не удалось выделить память для %d философов.\n", count);
    return -1;
  }
  for (int i = 0; i < count; i++)
  {
    Philosopher *philosopher = &simulation->philosophers[i];
    philosopher->id = i;
    philosopher->simulation = simulation;
    philosopher->state = WAITING;
    simulation->fork_owner[i] = -1;

    /* Инициализируем семафор для конкретного философа*/
    if (sem_init(&philosopher->wake, 0, 0) == -1)
    {
      print_log("Ошибка sem_init: %s\n", strerror(errno));
      return -1;
    }
    simulation->sem_count++;
  }
  return 0;
}

static void free_simulation(Simulation *simulation)
{
  /* Удаляем ненужные семафоры */
  for (int i = 0; i < simulation->sem_count; i++)
  {
    sem_destroy(&simulation->philosophers[i].wake);
  }
  free(simulation->philosophers);
  free(simulation->fork_owner);
  free(simulation->queue);
}

/* Общий генератор вызывается только под мьютексом симуляции */
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

/* Вызывается потоком философа под общим мьютексом. */
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
    
    /* Ставит семофор и говорит потоку философа, что выдали вилки */
    if (sem_post(&philosopher->wake) == -1)
    {
      stop_simulation(simulation, 1, "ошибка sem_post при выдаче вилок");
      return;
    }
  }
}

/* Все функции работы с общим состоянием вызываются под mutex. */
static void update_time(Simulation *simulation)
{
  /* Так как теперь у нас потоков много, то работать с виртуальным временем не выйдет,
      пришлось вот так выкручиваться*/
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
  simulation->reason = reason;

  /* Будим и ожидающих вилок, и спящих */
  for (int i = 0; i < simulation->parameters->count; i++)
  {
    if (sem_post(&simulation->philosophers[i].wake) == -1)
    {
      simulation->result = 1;
      simulation->reason = "ошибка sem_post при остановке";
    }
  }
}

static int check_stop(Simulation *simulation)
{
  const Parameters *parameters = simulation->parameters;

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

/* Ожидание на время раздумий или еды, во время которого мы отпускаем мьютекс */
static int wait_until(Philosopher *philosopher)
{
  Simulation *simulation = philosopher->simulation;

  while (!check_stop(simulation) &&
         simulation->now_ms < philosopher->next_event_ms)
  {
    long long remaining = philosopher->next_event_ms - simulation->now_ms;

    /* Просыпаемся каждые 10 милисекунд, чтобы не фулл тайм занимать проц*/
    if (remaining > 10)
    {
      remaining = 10;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) == -1)
    {
      stop_simulation(simulation, 1, "ошибка clock_gettime");
      break;
    }

    deadline.tv_nsec += (long)remaining * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }

    /* Вот тут как раз освободили мьютекс, чтобы остальные могли им пользоваться */
    pthread_mutex_unlock(&simulation->mutex);
    int result;
    do
    {
      /* Блокируем поток до дд */
      result = sem_timedwait(&philosopher->wake, &deadline);
    } while (result == -1 && errno == EINTR);

    int error = errno;
    pthread_mutex_lock(&simulation->mutex);

    if (result == -1 && error != ETIMEDOUT)
    {
      stop_simulation(simulation, 1, "ошибка sem_timedwait");
    }
  }
  return !simulation->stopped;
}

/* Тут ожидание семафора */
static int wait_wakeup(Philosopher *philosopher)
{
  int result;
  do
  {
    result = sem_wait(&philosopher->wake);
  } while (result == -1 && errno == EINTR);
  return result;
}

static void *philosopher_main(void *argument)
{
  Philosopher *philosopher = argument;
  Simulation *simulation = philosopher->simulation;
  const Parameters *parameters = simulation->parameters;
  int id = philosopher->id;

  int result = wait_wakeup(philosopher); /* Ждем ожидания для общего старта */
  pthread_mutex_lock(&simulation->mutex);

  if (result == -1)
  {
    stop_simulation(simulation, 1, "ошибка sem_wait при старте");
  }
  while (!check_stop(simulation))
  {

    start_thinking(simulation, parameters, id);
    if (!wait_until(philosopher))
    {
      break;
    }
    finish_thinking(simulation, parameters, id);

    /* Раздаем вилки и разлочиваем мбютекс, чтобы их могли взять*/
    give_forks(simulation, parameters);
    pthread_mutex_unlock(&simulation->mutex);
    result = wait_wakeup(philosopher);

    pthread_mutex_lock(&simulation->mutex);
    if (result == -1)
    {
      stop_simulation(simulation, 1, "ошибка sem_wait при ожидании вилок");
    }
    if (check_stop(simulation) || !wait_until(philosopher))
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
  pthread_mutex_unlock(&simulation->mutex);
  return NULL;
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

int run_simulation(const Parameters *parameters)
{
  Simulation simulation = {.parameters = parameters};
  int error = pthread_mutex_init(&simulation.mutex, NULL);

  if (error != 0)
  {
    print_log("Ошибка pthread_mutex_init: %s\n", strerror(error));
    return 1;
  }
  if (init_simulation(&simulation, parameters->count) == -1)
  {
    free_simulation(&simulation);
    pthread_mutex_destroy(&simulation.mutex);
    return 1;
  }

  print_log("=== Обедающие философы: многопоточная модель pthread ===\n"
            "Философов/вилок: %d, циклов: %d, стратегия: fifo.\n"
            "Размышление: %d..%d ms, еда: %d..%d ms.\n"
            "Лимит ожидания: %d ms, общий лимит: %d ms (0 — отключён).\n"
            "Период показа: %d ms, seed: %d. Лог: philosophers.log\n\n",
            parameters->count, parameters->cycles, parameters->think_min,
            parameters->think_max, parameters->eat_min, parameters->eat_max,
            parameters->max_wait, parameters->time_limit,
            parameters->display_delay, parameters->seed);

  srand((unsigned int)parameters->seed);

  int created = 0;
  for (int i = 0; i < parameters->count; i++)
  {
    /* Создаем поток для конкретного философа */
    error = pthread_create(&simulation.philosophers[i].thread, NULL,
                           philosopher_main, &simulation.philosophers[i]);

    if (error != 0)
    {
      print_log("Ошибка pthread_create для философа %d: %s\n", i + 1,
                strerror(error));
      break;
    }
    created++;
  }

  pthread_mutex_lock(&simulation.mutex);

  /* Тут мы записываем время старта, проверяем, всех ли записали и тд*/
  if (clock_gettime(CLOCK_MONOTONIC, &simulation.started_at) == -1)
  {
    stop_simulation(&simulation, 1, "ошибка clock_gettime при старте");
  }

  else if (created != parameters->count)
  {
    stop_simulation(&simulation, 1, "не удалось создать все потоки");
  }

  else if (!check_stop(&simulation))
  {
    /* тут всем философам разрешаем начало работы */
    for (int i = 0; i < created; i++)
    {
      if (sem_post(&simulation.philosophers[i].wake) == -1)
      {
        stop_simulation(&simulation, 1, "ошибка sem_post при старте");
        break;
      }
    }
  }

  long long next_snapshot = 0;
  while (!check_stop(&simulation))
  {
    if (parameters->display_delay > 0 && simulation.now_ms >= next_snapshot)
    {
      print_snapshot(&simulation, parameters->count);
      next_snapshot = simulation.now_ms + parameters->display_delay;
    }

    pthread_mutex_unlock(&simulation.mutex);
    pause_display(5);
    pthread_mutex_lock(&simulation.mutex);
  }

  pthread_mutex_unlock(&simulation.mutex);
  for (int i = 0; i < created; i++)
  {
    /* Дожидаемся окончания работы каждого потока */
    error = pthread_join(simulation.philosophers[i].thread, NULL);

    /* Ресурсы нельзя освобождать, если поток ещё может их читать */
    if (error != 0)
    {
      print_log("Ошибка pthread_join: %s\n", strerror(error));
      return 1;
    }
  }

  print_log("\n=== Итоги моделирования ===\nПричина завершения: %s.\n",
            simulation.reason);
  stop_philosophers(&simulation, parameters->count);
  print_snapshot(&simulation, parameters->count);
  print_statistics(&simulation, parameters);
  if (!output_failed())
  {
    print_log("Лог: philosophers.log\n");
  }
  
  int result = output_failed() ? 1 : simulation.result;
  free_simulation(&simulation);
  pthread_mutex_destroy(&simulation.mutex);
  return result;
}
