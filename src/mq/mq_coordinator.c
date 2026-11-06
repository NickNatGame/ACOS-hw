#include "mq_philosophers.h"
#include "console.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum
{
  WAITING,
  READY,
  THINKING,
  HUNGRY,
  EATING,
  DONE,
  STOPPED
} MqState;

typedef struct
{
  pid_t pid;
  MqState state;
  int cycles_done;
  int meals_started;
  int departed;
  long long deadline;
  long long hungry_since;
  long long total_wait;
  long long max_wait;
  long long pending_wait;
} MqPhilosopher;

/* Только координатор читает и изменяет эту структуру, mmap здесь не нужен */
typedef struct
{
  Parameters parameters;
  MqResources resources;
  MqPhilosopher philosophers[MAX_PHILOSOPHERS];
  int forks[MAX_PHILOSOPHERS];
  int queue[MAX_PHILOSOPHERS];
  int head;
  int size;
  int connected;
  int finished;
  int started;
  int stopped;
  int result;
  long long start_ms;
  long long now_ms;
  char reason[128];
} MqSimulation;

static void stop_model(MqSimulation *simulation, int result, const char *reason)
{
  if (!simulation->stopped)
  {
    simulation->stopped = 1;
    simulation->result = result;
    snprintf(simulation->reason, sizeof(simulation->reason), "%s", reason);
  }
}

static int reply(MqSimulation *simulation, int id, int type, int value)
{
  MqMessage message = {.version = MQ_PROTOCOL_VERSION, .type = type,
      .id = id, .pid = getpid(), .value = value};

  if (type == MQ_CONFIG)
  {
    message.parameters = simulation->parameters;
  }

  if (mq_send_message(simulation->resources.replies[id], &message, 100) != 0 ||
      sem_post(simulation->resources.wake[id]) == -1)
  {
    dprintf(STDERR_FILENO, "Ошибка отправки ответа философу %d: %s\n", id + 1, strerror(errno));
    stop_model(simulation, 1, "ошибка ответа или заполненная очередь философа");
    return -1;
  }
  return 0;
}

static int random_time(int minimum, int maximum)
{
  long long range = (long long)maximum - minimum + 1;
  return minimum + (int)(rand() % range);
}

static void release_forks(MqSimulation *simulation, int id)
{
  int right = (id + 1) % simulation->parameters.count;
  simulation->forks[id] = simulation->forks[right] = -1;
  print_log("[%6lld ms] Философ %d: освободил вилки %d и %d\n",
            simulation->now_ms, id + 1, id + 1, right + 1);
}

static void check_limits(MqSimulation *simulation)
{
  if (received_signal())
  {
    stop_model(simulation, 128 + received_signal(), "прерывание координатора (SIGINT/SIGTERM)");
  }
  else if (output_failed())
  {
    stop_model(simulation, 1, "ошибка вывода");
  }

  if (!simulation->started || simulation->stopped)
  {
    return;
  }

  for (int i = 0; i < simulation->parameters.count; i++)
  {
    MqPhilosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state == HUNGRY &&
        simulation->now_ms - philosopher->hungry_since > simulation->parameters.max_wait)
    {
      stop_model(simulation, 2, "превышено максимально допустимое ожидание");
    }
  }
  if (simulation->parameters.time_limit > 0 &&
      simulation->now_ms >= simulation->parameters.time_limit)
  {
    stop_model(simulation, 2, "достигнут общий лимит времени");
  }
}

static void give_forks(MqSimulation *simulation)
{
  while (simulation->size > 0 && !simulation->stopped)
  {
    check_limits(simulation);
    if (simulation->stopped)
    {
      break;
    }

    int id = simulation->queue[simulation->head];
    int right = (id + 1) % simulation->parameters.count;

    if (simulation->forks[id] != -1 || simulation->forks[right] != -1)
    {
      break;
    }

    simulation->head = (simulation->head + 1) % simulation->parameters.count;
    simulation->size--;

    simulation->forks[id] = simulation->forks[right] = id;
    MqPhilosopher *philosopher = &simulation->philosophers[id];

    long long waited = simulation->now_ms - philosopher->hungry_since;
    philosopher->total_wait += waited;
    if (waited > philosopher->max_wait)
    {
      philosopher->max_wait = waited;
    }

    philosopher->meals_started++;
    philosopher->state = EATING;

    int duration = random_time(simulation->parameters.eat_min, simulation->parameters.eat_max);
    philosopher->deadline = simulation->now_ms + duration;
    print_log("[%6lld ms] Философ %d: получил левую вилку %d\n",
              simulation->now_ms, id + 1, id + 1);
    print_log("[%6lld ms] Философ %d: получил правую вилку %d\n",
              simulation->now_ms, id + 1, right + 1);
    print_log("[%6lld ms] Философ %d: начал есть после ожидания %lld ms на %d ms\n",
              simulation->now_ms, id + 1, waited, duration);
    reply(simulation, id, MQ_EAT_ALLOWED, duration);
  }
}

static void handle_message(MqSimulation *simulation, const MqMessage *message)
{
  int id = message->id;
  if (id < 0 || id >= simulation->parameters.count || message->pid <= 1 ||
      message->pid == getpid())
  {
    stop_model(simulation, 1, "неверный номер или PID в сообщении");
    return;
  }

  MqPhilosopher *philosopher = &simulation->philosophers[id];
  if (message->type == MQ_JOIN)
  {
    if (philosopher->pid || simulation->started || !mq_peer_alive(message->pid))
    {
      stop_model(simulation, 1, "повторная или неверная регистрация философа");
      return;
    }

    philosopher->pid = message->pid;
    philosopher->state = READY;
    simulation->connected++;
    print_log("Подключён независимый философ %d: PID %ld.\n", id + 1, (long)message->pid);

    reply(simulation, id, MQ_CONFIG, 0);

    if (!simulation->stopped && simulation->connected == simulation->parameters.count)
    {
      simulation->start_ms = mq_now_ms();
      if (simulation->start_ms < 0)
      {
        stop_model(simulation, 1, "ошибка часов при старте");
        return;
      }

      simulation->started = 1;
      print_log("Все %d независимых философов подключены. Старт моделирования.\n",
                simulation->parameters.count);
      for (int i = 0; i < simulation->parameters.count && !simulation->stopped; i++)
      {
        reply(simulation, i, MQ_START, 0);
      }

    }
    return;
  }

  if (philosopher->pid != message->pid)
  {
    stop_model(simulation, 1, "сообщение от незарегистрированного процесса");
    return;
  }

  if (message->type == MQ_ABORT)
  {
    int result = message->value == 130 || message->value == 143 ? message->value : 1;
    stop_model(simulation, result, "прерывание или ошибка независимого философа");
    return;
  }

  if (!simulation->started)
  {
    stop_model(simulation, 1, "действие до старта сессии");
    return;
  }

  if (message->type == MQ_THINK && philosopher->state == READY)
  {
    int duration = random_time(simulation->parameters.think_min, simulation->parameters.think_max);
    philosopher->state = THINKING;
    philosopher->deadline = simulation->now_ms + duration;

    print_log("[%6lld ms] Философ %d: начал размышление на %d ms\n",
              simulation->now_ms, id + 1, duration);
    reply(simulation, id, MQ_THINK_ALLOWED, duration);
  }
  else if (message->type == MQ_HUNGRY && philosopher->state == THINKING &&
           simulation->now_ms >= philosopher->deadline && simulation->size < simulation->parameters.count)
  {
    philosopher->state = HUNGRY;
    philosopher->hungry_since = simulation->now_ms;
    simulation->queue[(simulation->head + simulation->size) % simulation->parameters.count] = id;
    simulation->size++;

    print_log("[%6lld ms] Философ %d: закончил размышление и проголодался\n",
              simulation->now_ms, id + 1);
    print_log("[%6lld ms] Философ %d: запросил левую вилку %d\n",
              simulation->now_ms, id + 1, id + 1);
    print_log("[%6lld ms] Философ %d: запросил правую вилку %d\n",
              simulation->now_ms, id + 1, (id + 1) % simulation->parameters.count + 1);
    give_forks(simulation);
  }
  else if (message->type == MQ_FINISH && philosopher->state == EATING &&
           simulation->now_ms >= philosopher->deadline)
  {
    print_log("[%6lld ms] Философ %d: закончил есть\n", simulation->now_ms, id + 1);
    release_forks(simulation, id);
    philosopher->cycles_done++;
    philosopher->state = READY;

    if (philosopher->cycles_done == simulation->parameters.cycles)
    {
      philosopher->state = DONE;
      simulation->finished++;
      print_log("[%6lld ms] Философ %d: выполнил все %d циклов\n",
                simulation->now_ms, id + 1, simulation->parameters.cycles);
    }
    else
    {
      reply(simulation, id, MQ_CONTINUE, 0);
    }
    if (simulation->finished == simulation->parameters.count)
    {
      stop_model(simulation, 0, "все философы выполнили заданное число циклов");
    }
    give_forks(simulation);
  }
  else
  {
    stop_model(simulation, 1, "неверный тип сообщения или порядок действий философа");
  }
}

static void print_snapshot(const MqSimulation *simulation)
{
  const char *names[] = {"ожидает подключения", "ожидает действия", "думает",
      "голоден", "ест", "завершил", "остановлен"};

  print_log("[%6lld ms] Состояние:", simulation->now_ms);
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    print_log(" P%d=%s", i + 1, names[simulation->philosophers[i].state]);
  }

  print_log("\nВилки:");
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    if (simulation->forks[i] == -1)
    {
      print_log(" F%d=свободна", i + 1);
    }
    else
    {
      print_log(" F%d=P%d", i + 1, simulation->forks[i] + 1);
    }
  }
  print_log("\n\n");
}

static void print_results(MqSimulation *simulation)
{
  print_log("\n=== Итоги моделирования ===\nПричина завершения: %s.\n", simulation->reason);
  long long total_meals = 0, started_meals = 0, total_wait = 0;
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    MqPhilosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state == EATING)
    {
      print_log("[%6lld ms] Философ %d: еда прервана, цикл не засчитан\n",
                simulation->now_ms, i + 1);
      release_forks(simulation, i);
    }

    if (philosopher->state == HUNGRY)
    {
      philosopher->pending_wait = simulation->now_ms - philosopher->hungry_since;
      if (philosopher->pending_wait > philosopher->max_wait)
      {
        philosopher->max_wait = philosopher->pending_wait;
      }
    }

    if (philosopher->state != DONE)
    {
      philosopher->state = STOPPED;
    }

    total_meals += philosopher->cycles_done;
    started_meals += philosopher->meals_started;
    total_wait += philosopher->total_wait;
  }

  print_snapshot(simulation);
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    const MqPhilosopher *philosopher = &simulation->philosophers[i];
    print_log("P%d: циклов=%d/%d, начато приёмов пищи=%d, ожидание перед начатой едой=%lld ms, "
              "незавершённое ожидание=%lld ms, максимум=%lld ms\n", i + 1,
              philosopher->cycles_done, simulation->parameters.cycles, philosopher->meals_started,
              philosopher->total_wait, philosopher->pending_wait, philosopher->max_wait);
  }
  print_log("Всего завершённых приёмов пищи: %lld\n", total_meals);
  if (started_meals)
  {
    print_log("Среднее ожидание перед начатой едой: %.2f ms\n", (double)total_wait / started_meals);
  }
  print_log("Время работы: %lld ms\n", simulation->now_ms);
}

static void shutdown_participants(MqSimulation *simulation)
{
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    if (simulation->philosophers[i].pid)
    {
      if (reply(simulation, i, MQ_STOP, simulation->result) == -1)
      {
        simulation->result = 1;
      }
    }
  }

  long long deadline = mq_now_ms();
  if (deadline >= 0)
  {
    deadline += 1000;
  }

  while (deadline >= 0)
  {
    int remaining = 0;
    for (int i = 0; i < simulation->parameters.count; i++)
    {
      MqPhilosopher *philosopher = &simulation->philosophers[i];
      if (philosopher->pid && !philosopher->departed)
      {
        if (mq_peer_alive(philosopher->pid))
        {
          remaining++;
        }
        else
        {
          kill(philosopher->pid, SIGKILL);
          philosopher->departed = 1;
        }
      }
    }

    long long now = mq_now_ms();
    if (!remaining || now < 0 || now >= deadline)
    {
      break;
    }

    MqMessage message;
    int received = mq_receive_message(simulation->resources.requests, &message, 10);
    if (received == 0 && message.type == MQ_EXIT && message.id >= 0 &&
        message.id < simulation->parameters.count &&
        simulation->philosophers[message.id].pid == message.pid)
    {
      simulation->philosophers[message.id].departed = 1;
    }
    else if (received == -1)
    {
      if (simulation->result == 0)
      {
        simulation->result = 1;
      }
      break;
    }
  }

  /* Остановленный через SIGSTOP участник не сможет принять MQ_STOP */
  for (int i = 0; i < simulation->parameters.count; i++)
  {
    MqPhilosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->pid && !philosopher->departed && kill(philosopher->pid, 0) == 0)
    {
      kill(philosopher->pid, SIGKILL);
      simulation->result = 1;
    }
  }
}

int run_mq_coordinator(const char *session, const Parameters *parameters)
{
  MqSimulation simulation = {0};
  simulation.parameters = *parameters;
  if (mq_create_resources(&simulation.resources, session, parameters->count) == -1)
  {
    dprintf(STDERR_FILENO, "Ошибка создания POSIX очередей/семафоров: %s\n", strerror(errno));
    mq_close_resources(&simulation.resources);
    return 1;
  }

  char log_path[96];
  snprintf(log_path, sizeof(log_path), "philosophers-mq-%s.log", session);
  if (attach_console_log(log_path, 1) == -1)
  {
    dprintf(STDERR_FILENO, "Ошибка открытия журнала MQ: %s\n", strerror(errno));
    mq_close_resources(&simulation.resources);
    return 1;
  }

  for (int i = 0; i < parameters->count; i++)
  {
    simulation.forks[i] = -1;
  }

  srand((unsigned int)parameters->seed);
  print_log("ГОТОВО: MQ-сессия %s, координатор PID %ld, ожидаем %d философов.\n"
            "POSIX очереди сообщений и именованные семафоры; стратегия FIFO.\n"
            "Циклов: %d, размышление: %d..%d ms, еда: %d..%d ms.\n",
            session, (long)getpid(), parameters->count, parameters->cycles,
            parameters->think_min, parameters->think_max, parameters->eat_min, parameters->eat_max);
  long long waiting_since = mq_now_ms();
  long long next_health_check = 0;
  long long next_snapshot = 0;

  while (!simulation.stopped)
  {
    long long now = mq_now_ms();
    if (now < 0 || waiting_since < 0)
    {
      stop_model(&simulation, 1, "ошибка часов");
      break;
    }

    simulation.now_ms = simulation.started ? now - simulation.start_ms : 0;
    check_limits(&simulation);
    if (!simulation.started && now - waiting_since >= MQ_CONNECT_TIMEOUT)
    {
      stop_model(&simulation, 1, "не все философы подключились за 10 секунд");
    }

    if (now >= next_health_check)
    {
      for (int i = 0; i < parameters->count; i++)
      {
        pid_t pid = simulation.philosophers[i].pid;
        if (pid && !mq_peer_alive(pid))
        {
          stop_model(&simulation, 1, "аварийное завершение или остановка независимого философа");
        }
      }
      next_health_check = now + 100;
    }

    if (simulation.stopped)
    {
      break;
    }
    if (simulation.started && parameters->display_delay > 0 && simulation.now_ms >= next_snapshot)
    {
      print_snapshot(&simulation);
      next_snapshot = simulation.now_ms + parameters->display_delay;
    }

    MqMessage message;
    int received = mq_receive_message(simulation.resources.requests, &message, 10);
    now = mq_now_ms();
    if (now < 0)
    {
      stop_model(&simulation, 1, "ошибка часов");
      break;
    }

    simulation.now_ms = simulation.started ? now - simulation.start_ms : 0;
    check_limits(&simulation);

    if (received == -1)
    {
      stop_model(&simulation, 1, "ошибка очереди или неверный формат сообщения");
    }
    else if (received == 0 && !simulation.stopped)
    {
      handle_message(&simulation, &message);
    }
  }

  shutdown_participants(&simulation);
  print_results(&simulation);
  int result = output_failed() ? 1 : simulation.result;
  if (mq_close_resources(&simulation.resources) == -1)
  {
    result = 1;
  }
  return result;
}
