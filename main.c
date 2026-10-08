#define _POSIX_C_SOURCE 200809L

#include "io_util.h"
#include "race.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct Race *g_race;

static void on_signal(int sig) {
    (void)sig;
    if (g_race) {
        g_race->stop_flag = 1;
    }
}

static void usage(int fd) {
    io_write(fd,
             "autogonki — имитация кольцевой гонки (правила В. Пудова)\n"
             "\n"
             "запуск:\n"
             "  ./autogonki [опции]\n"
             "\n"
             "опции:\n"
             "  --lanes N         число дорожек (2..8), по умолчанию 4\n"
             "  --sectors N       длина круга в клетках (8..64), по умолчанию 24\n"
             "  --laps N          кругов до финиша, по умолчанию 2\n"
             "  --teams N         команд (1..8), по умолчанию 3\n"
             "  --cars N          машин в команде (1..2), по умолчанию 2\n"
             "  --strategy S      greedy | random\n"
             "  --bonus 0|1       круговые премии\n"
             "  --lapping 0|1     снятие отставших на круг\n"
             "  --max-rounds N    лимит раундов\n"
             "  --seed N          зерно ГПСЧ\n"
             "  --delay MS        пауза между ходами\n"
             "  --log ФАЙЛ        журнал (также пишется на stdout)\n"
             "  --start i,j,...   порядок старта (id машин)\n"
             "  --interactive     спросить основные параметры\n"
             "  --help\n");
}

static int parse_start(char *s, struct Config *c) {
    int n = 0;
    char *p = s;
    while (*p && n < MAX_CARS) {
        c->start_order[n++] = atoi(p);
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
    return n;
}

static void ask_int(const char *msg, int *dst) {
    char buf[64];
    io_printf(STDOUT_FILENO, "%s [%d]: ", msg, *dst);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        *dst = atoi(buf);
    }
}

int main(int argc, char **argv) {
    struct Config cfg;
    struct Race race;
    int i;
    int interactive = 0;
    int start_given = 0;

    config_defaults(&cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(STDOUT_FILENO);
            return 0;
        } else if (strcmp(argv[i], "--lanes") == 0 && i + 1 < argc) {
            cfg.lanes = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--sectors") == 0 && i + 1 < argc) {
            cfg.sectors = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--laps") == 0 && i + 1 < argc) {
            cfg.laps_total = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--teams") == 0 && i + 1 < argc) {
            cfg.n_teams = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--cars") == 0 && i + 1 < argc) {
            cfg.cars_per_team = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--strategy") == 0 && i + 1 < argc) {
            i++;
            cfg.default_strategy = (strcmp(argv[i], "random") == 0) ? STR_RANDOM : STR_GREEDY;
        } else if (strcmp(argv[i], "--bonus") == 0 && i + 1 < argc) {
            cfg.use_bonus = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lapping") == 0 && i + 1 < argc) {
            cfg.use_lapping = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--max-rounds") == 0 && i + 1 < argc) {
            cfg.max_rounds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg.seed = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            cfg.delay_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            cfg.log_path = argv[++i];
        } else if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
            start_given = parse_start(argv[++i], &cfg);
        } else if (strcmp(argv[i], "--interactive") == 0) {
            interactive = 1;
        } else {
            io_printf(STDERR_FILENO, "неизвестная опция: %s\n", argv[i]);
            usage(STDERR_FILENO);
            return 1;
        }
    }

    if (interactive) {
        ask_int("дорожек", &cfg.lanes);
        ask_int("клеток в круге", &cfg.sectors);
        ask_int("кругов", &cfg.laps_total);
        ask_int("команд", &cfg.n_teams);
        ask_int("машин в команде", &cfg.cars_per_team);
        ask_int("лимит раундов", &cfg.max_rounds);
        ask_int("задержка мс", &cfg.delay_ms);
    }

    if (cfg.lanes < 2 || cfg.lanes > MAX_LANES ||
        cfg.sectors < 8 || cfg.sectors > MAX_SECTORS ||
        cfg.laps_total < 1 || cfg.n_teams < 1 || cfg.n_teams > MAX_TEAMS ||
        cfg.cars_per_team < 1 || cfg.cars_per_team > 2) {
        io_write(STDERR_FILENO, "некорректные параметры трассы/состава\n");
        return 1;
    }

    cfg.n_cars = cfg.n_teams * cfg.cars_per_team;
    if (cfg.n_cars > MAX_CARS || cfg.n_cars > cfg.sectors) {
        io_write(STDERR_FILENO, "слишком много машин для длины круга\n");
        return 1;
    }

    if (!start_given) {
        for (i = 0; i < cfg.n_cars; i++) {
            cfg.start_order[i] = i;
        }
    } else if (start_given != cfg.n_cars) {
        io_write(STDERR_FILENO, "в --start должно быть столько id, сколько машин\n");
        return 1;
    }

    g_race = &race;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    race_init(&race, &cfg);
    race_run(&race);

    if (race.log_fd >= 0 && race.log_fd != STDOUT_FILENO) {
        close(race.log_fd);
    }
    return 0;
}
