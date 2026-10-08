#ifndef RACE_H
#define RACE_H

#define MAX_LANES   8
#define MAX_SECTORS 64
#define MAX_CARS    16
#define MAX_TEAMS   8
#define MAX_PATH    24

enum CarStatus {
    CAR_RACING = 0,
    CAR_FINISHED,
    CAR_RETIRED
};

enum Strategy {
    STR_GREEDY = 0, /* максимально вперёд и к внутренней бровке */
    STR_RANDOM      /* случайный допустимый маршрут */
};

struct Cell {
    int lane;
    int sec;
};

struct Car {
    int id;           /* номер машины 0..n-1 */
    int team;         /* номер команды */
    int lane;         /* 0 — внутренняя дорожка */
    int sec;          /* сектор 0..sectors-1, вперёд = +1 */
    int laps;         /* сколько кругов уже закрыто */
    int bonus;        /* оставшиеся клетки круговой премии */
    int zero_move;    /* 1, если в этом раунде ход «ноль» */
    int dist_at_stop; /* дистанция на момент финиша/схода */
    enum CarStatus status;
    enum Strategy strategy;
};

struct Config {
    int lanes;
    int sectors;
    int laps_total;
    int n_teams;
    int cars_per_team;
    int n_cars;
    int start_order[MAX_CARS];
    enum Strategy default_strategy;
    int use_bonus;
    int use_lapping;
    int max_rounds;
    unsigned seed;
    int delay_ms;
    const char *log_path;
};

struct Race {
    struct Config cfg;
    struct Car cars[MAX_CARS];
    int occ[MAX_LANES][MAX_SECTORS]; /* id машины или -1 */
    int order[MAX_CARS];             /* порядок хода в раунде */
    int n_racing;
    int round;
    int finish_seq;                  /* кто раньше финишировал */
    int stop_flag;                   /* SIGINT/SIGTERM */
    int log_fd;
    int out_fd;
};

void config_defaults(struct Config *c);
int  config_n_cars(const struct Config *c);
void race_init(struct Race *r, const struct Config *c);
void race_run(struct Race *r);
void race_print_standings(struct Race *r, const char *title);

#endif
