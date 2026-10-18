#include "console.h"
#include "philosophers.h"
#include "soft_assert.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum { THINKING, HUNGRY, EATING, DONE, STOPPED } State;

typedef struct {
  State state;
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
typedef struct {
  Philosopher *philosophers;
  int *fork_owner; /* -1 - свободна, иначе индекс владельца */
  int *queue; /* Ссылка на массив, содержащий индексы голодных */
  int queue_head;
  int queue_tail;
  int queue_size;
  int finished;
  long long now_ms;
} Simulation;

static int init_simulation(Simulation *simulation, int count) {
  int fl_err = 0;
  size_t size = (size_t)count;

  /* проверяем размер до начала всего */
  SOFT_ASSERT_ERR(size <= SIZE_MAX / sizeof(Philosopher));
  SOFT_ASSERT_ERR(size <= SIZE_MAX / sizeof(int));
  if (fl_err != 0) {
    print_log("Ошибка: размер массивов не помещается в size_t.\n");
    return -1;
  }

  simulation->philosophers = calloc(size, sizeof(Philosopher));
  simulation->fork_owner = calloc(size, sizeof(int));
  simulation->queue = calloc(size, sizeof(int));
  SOFT_ASSERT_ERR(simulation->philosophers != NULL);
  SOFT_ASSERT_ERR(simulation->fork_owner != NULL);
  SOFT_ASSERT_ERR(simulation->queue != NULL);
  if (fl_err != 0) {
    print_log("Ошибка: не удалось выделить память для %d философов.\n", count);
    return -1;
  }
  return 0;
}

static void free_simulation(Simulation *simulation) {
  free(simulation->philosophers);
  free(simulation->fork_owner);
  free(simulation->queue);
}

static int random_time(int minimum, int maximum) {
  long long range = (long long)maximum - minimum + 1;
  return minimum + (int)(rand() % range);
}

static const char *state_name(State state) {
  switch (state) {
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

static void print_snapshot(const Simulation *simulation, int count) {
  print_log("[%6lld ms] Состояние:", simulation->now_ms);
  for (int i = 0; i < count; i++) {
    const Philosopher *philosopher = &simulation->philosophers[i];
    print_log(" P%d=%s", i + 1, state_name(philosopher->state));
    if (philosopher->state == HUNGRY) {
      print_log("(ждёт %lld ms)",
                simulation->now_ms - philosopher->hungry_since_ms);
    }
  }
  print_log("\nВилки:");
  for (int i = 0; i < count; i++) {
    if (simulation->fork_owner[i] == -1) {
      print_log(" F%d=свободна", i + 1);
    } else {
      print_log(" F%d=P%d", i + 1, simulation->fork_owner[i] + 1);
    }
  }
  print_log("\n\n");
}

static void start_thinking(Simulation *simulation, const Parameters *parameters,
                           int id) {
  Philosopher *philosopher = &simulation->philosophers[id];
  int duration = random_time(parameters->think_min, parameters->think_max);
  philosopher->state = THINKING;
  philosopher->next_event_ms = simulation->now_ms + duration;
  print_log("[%6lld ms] Философ %d: начал размышление на %d ms\n",
            simulation->now_ms, id + 1, duration);
}

static void release_forks(Simulation *simulation, int id, int count) {
  /* Смотрим на вилку слева, на вилку справа и освобождаем */
  int left = id;
  int right = (id + 1) % count;
  simulation->fork_owner[left] = -1;
  simulation->fork_owner[right] = -1;
  print_log("[%6lld ms] Философ %d: освободил вилки %d и %d\n",
            simulation->now_ms, id + 1, left + 1, right + 1);
}

static void finish_meals(Simulation *simulation, const Parameters *parameters) {
  for (int i = 0; i < parameters->count; i++) {
    Philosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state !=
            EATING ||                               
        philosopher->next_event_ms !=               /*                                        */
            simulation->now_ms) {                   /*   пропускаем тех, кто не ест, либо     */
                                                    /*   должен закончить в другой момент     */
      continue; 
    }
    print_log("[%6lld ms] Философ %d: закончил есть\n", simulation->now_ms,
              i + 1);
    release_forks(simulation, i, parameters->count);
    philosopher->cycles_done++;

    if (philosopher->cycles_done ==
        parameters->cycles) {                                   /*                                        */
      philosopher->state = DONE;                                /*      Проверяем, на то                  */
      simulation->finished++;                                   /*        закончились ли циклы еды        */
      print_log(                                                /*          у философов                    */
          "[%6lld ms] Философ %d: выполнил все %d циклов\n",    
          simulation->now_ms, i + 1,
          parameters->cycles); 
    } else {
      start_thinking(simulation, parameters, i);
    }
  }
}

/* Постановка в очередь конкретного философа */
static void finish_thinking(Simulation *simulation,
                            const Parameters *parameters) {
  for (int i = 0; i < parameters->count; i++) {
    Philosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state !=
            THINKING || 
        philosopher->next_event_ms !=
            simulation                          /*                                              */
                ->now_ms) {                     /*      пропускаем тех, кто не **думает**, либо */
      continue;                                 /*      должен закончить в другой момент        */
    }
    philosopher->state = HUNGRY;
    philosopher->hungry_since_ms = simulation->now_ms;
    simulation->queue[simulation->queue_tail] = i;
    simulation->queue_tail = (simulation->queue_tail + 1) %
                             parameters->count;                     /*        так как стол круглый,                    */
                                                                    /*   то мы условно с замыканием перемещаем хвост   */
    simulation->queue_size++;

    print_log("[%6lld ms] Философ %d: закончил размышление и проголодался\n",
              simulation->now_ms, i + 1);
    print_log("[%6lld ms] Философ %d: запросил левую вилку %d\n",
              simulation->now_ms, i + 1, i + 1);
    print_log("[%6lld ms] Философ %d: запросил правую вилку %d\n",
              simulation->now_ms, i + 1, (i + 1) % parameters->count + 1);
  }
}

static int wait_limit_reached(const Simulation *simulation,
                              const Parameters *parameters) {
  for (int i = 0; i < parameters->count; i++) {
    const Philosopher *philosopher = &simulation->philosophers[i];
    long long waited = simulation->now_ms - philosopher->hungry_since_ms;
    if (philosopher->state == HUNGRY && waited > parameters->max_wait) {
      print_log("Диагностика: философ %d ждёт %lld ms, лимит %d ms.\n", i + 1,
                waited, parameters->max_wait);
      return 1;
    }
  }
  return 0;
}

static void give_forks(Simulation *simulation, const Parameters *parameters) {
  while (simulation->queue_size > 0) {
    /* Берем буквально самого голодного философа :(*/
    int id = simulation->queue[simulation->queue_head];
    int left = id;
    int right = (id + 1) % parameters->count;

    /* Если первый ждёт, остальные не обходят его */
    if (simulation->fork_owner[left] != -1 ||
        simulation->fork_owner[right] != -1) {
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
    if (waited > philosopher->max_wait_ms) {
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
  }
}

static long long next_event_time(const Simulation *simulation,
                                 const Parameters *parameters) {
  long long next = LLONG_MAX;
  for (int i = 0; i < parameters->count; i++) {
    const Philosopher *philosopher = &simulation->philosophers[i];
    long long candidate;

    if (philosopher->state == THINKING || philosopher->state == EATING) {
      candidate = philosopher->next_event_ms;
    } else if (philosopher->state == HUNGRY) {
      /* Ровно max_wait ещё допустимо, нарушение наступает через 1 ms */
      candidate = philosopher->hungry_since_ms + parameters->max_wait + 1;
    } else {
      continue;
    }
    if (candidate < next) {
      next = candidate;
    }
  }
  if (parameters->time_limit > 0 && parameters->time_limit < next) {
    next = parameters->time_limit;
  }
  return next;
}

static void stop_philosophers(Simulation *simulation, int count) {
  for (int i = 0; i < count; i++) {
    Philosopher *philosopher = &simulation->philosophers[i];
    if (philosopher->state == DONE) {
      continue;
    }
    if (philosopher->state == EATING) {
      print_log("[%6lld ms] Философ %d: еда прервана, цикл не засчитан\n",
                simulation->now_ms, i + 1);
      release_forks(simulation, i, count);
    } else if (philosopher->state == HUNGRY) {
      philosopher->pending_wait_ms =
          simulation->now_ms - philosopher->hungry_since_ms;

      if (philosopher->pending_wait_ms > philosopher->max_wait_ms) {
        philosopher->max_wait_ms = philosopher->pending_wait_ms;
      }

    }
    philosopher->state = STOPPED;
  }
  simulation->queue_size = 0;
}

static void print_statistics(const Simulation *simulation,
                             const Parameters *parameters) {
  long long total_meals = 0;
  long long started_meals = 0;
  long long total_wait = 0;

  for (int i = 0; i < parameters->count; i++) {
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
  if (started_meals > 0) {
    print_log("Среднее ожидание перед начатой едой: %.2f ms\n",
              (double)total_wait / (double)started_meals);
  }
  print_log("Модельное время: %lld ms\n", simulation->now_ms);
}

/*=============================================*/
/*                                             */
/*               Основной цикл                 */
/*                                             */
/*=============================================*/
int run_simulation(const Parameters *parameters) {
  Simulation simulation = {0};
  if (init_simulation(&simulation, parameters->count) == -1) {
    free_simulation(&simulation);
    return 1;
  }
  int result = 0;
  const char *reason = "все философы выполнили заданное число циклов";

  srand((unsigned int)parameters->seed);
  print_log("=== Обедающие философы: последовательная модель ===\n"
            "Философов/вилок: %d, циклов: %d, стратегия: fifo.\n"
            "Размышление: %d..%d ms, еда: %d..%d ms.\n"
            "Лимит ожидания: %d ms, общий лимит: %d ms (0 — отключён).\n"
            "Задержка показа: %d ms, seed: %d. Лог: philosophers.log\n\n",
            parameters->count, parameters->cycles, parameters->think_min,
            parameters->think_max, parameters->eat_min, parameters->eat_max,
            parameters->max_wait, parameters->time_limit,
            parameters->display_delay, parameters->seed);

  for (int i = 0; i < parameters->count; i++) {
    simulation.fork_owner[i] = -1;
    start_thinking(&simulation, parameters, i);
  }

  while (simulation.finished < parameters->count) {
    if (received_signal() != 0) {
      reason = "прерывание пользователем (SIGINT/SIGTERM)";
      result = 128 + received_signal();
      break;
    }
    if (output_failed()) {
      reason = "ошибка вывода";
      result = 1;
      break;
    }

    finish_meals(&simulation, parameters);
    finish_thinking(&simulation, parameters);
    if (simulation.finished == parameters->count) {
      break;
    }
    /* Проверяем лимиты ДО выдачи вилок */
    if (wait_limit_reached(&simulation, parameters)) {
      reason = "превышено максимально допустимое ожидание";
      result = 2;
      break;
    }
    if (parameters->time_limit > 0 &&
        simulation.now_ms >= parameters->time_limit) {
      reason = "достигнут общий лимит времени";
      print_log("Диагностика: достигнут общий лимит %d ms.\n",
                parameters->time_limit);
      result = 2;
      break;
    }

    give_forks(&simulation, parameters);
    print_snapshot(&simulation, parameters->count);
    pause_display(parameters->display_delay);

    /* При прерывании паузы время больше не продвигаем */
    if (!received_signal() && !output_failed()) {
      simulation.now_ms = next_event_time(&simulation, parameters);
    }
  }

  print_log("\n=== Итоги моделирования ===\nПричина завершения: %s.\n", reason);
  stop_philosophers(&simulation, parameters->count);
  print_snapshot(&simulation, parameters->count);
  print_statistics(&simulation, parameters);
  if (!output_failed()) {
    print_log("Лог: philosophers.log\n");
  }
  if (output_failed()) {
    result = 1;
  }
  free_simulation(&simulation);
  return result;
}
