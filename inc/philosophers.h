#ifndef PHILOSOPHERS_H
#define PHILOSOPHERS_H

typedef struct {
    int count;
    int cycles;
    int think_min;
    int think_max;
    int eat_min;
    int eat_max;
    int max_wait;
    int display_delay;
    int seed;
    int time_limit;  /* 0 отключает общий лимит времени */
} Parameters;

/* 1 - параметры прочитаны, 0 - справка, -1 - ошибка */
int read_parameters(int argc, char **argv, Parameters *parameters);

/* 0 - успех, 1 - ошибка, 2 - лимит, 130/143 - SIGINT/SIGTERM */
int run_simulation(const Parameters *parameters);

#endif
