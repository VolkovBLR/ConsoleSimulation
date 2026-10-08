#define _POSIX_C_SOURCE 200809L

#include "race.h"
#include "io_util.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct Candidate {
    struct Cell path[MAX_PATH];
    int n;
    int cost;
    int laps;
    int lane;
    int sec;
};

static struct Candidate g_found[256];
static int g_nfound;
static int g_path_lane[MAX_PATH];
static int g_path_sec[MAX_PATH];
static int g_path_n;
static char g_vis[MAX_LANES][MAX_SECTORS];

static void emit(struct Race *r, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io_write(r->out_fd, buf);
    if (r->log_fd >= 0 && r->log_fd != r->out_fd) {
        io_write(r->log_fd, buf);
    }
}

static int dist_of(const struct Car *c, int sectors) {
    return c->laps * sectors + c->sec;
}

static int is_ahead(const struct Car *a, const struct Car *b, int sectors) {
    int da = dist_of(a, sectors);
    int db = dist_of(b, sectors);
    if (da != db) {
        return da > db;
    }
    return a->lane < b->lane;
}

void config_defaults(struct Config *c) {
    memset(c, 0, sizeof(*c));
    c->lanes = 4;
    c->sectors = 24;
    c->laps_total = 2;
    c->n_teams = 3;
    c->cars_per_team = 2;
    c->n_cars = 6;
    c->default_strategy = STR_GREEDY;
    c->use_bonus = 1;
    c->use_lapping = 1;
    c->max_rounds = 200;
    c->seed = 1;
    c->delay_ms = 0;
    c->log_path = "race.log";
}

int config_n_cars(const struct Config *c) {
    return c->n_teams * c->cars_per_team;
}

static void occ_clear(struct Race *r) {
    int i, j;
    for (i = 0; i < MAX_LANES; i++) {
        for (j = 0; j < MAX_SECTORS; j++) {
            r->occ[i][j] = -1;
        }
    }
}

static void occ_rebuild(struct Race *r) {
    int i;
    occ_clear(r);
    for (i = 0; i < r->cfg.n_cars; i++) {
        if (r->cars[i].status == CAR_RETIRED) {
            continue;
        }
        if (r->cars[i].status == CAR_FINISHED) {
            continue; /* после раунда финишёры уже сняты */
        }
        r->occ[r->cars[i].lane][r->cars[i].sec] = i;
    }
}

static int cell_ok(const struct Race *r, int lane, int sec, int self,
                   int self_laps, int is_leader) {
    int v;
    if (g_vis[lane][sec]) {
        return 0;
    }
    v = r->occ[lane][sec];
    if (v < 0 || v == self) {
        return 1;
    }
    /* лидер может «проехать» отстающего на внутренней — того снимут */
    if (is_leader && r->cfg.use_lapping && lane == 0 &&
        r->cars[v].status == CAR_RACING && r->cars[v].laps < self_laps) {
        return 1;
    }
    return 0;
}

static void save_candidate(int cost, int laps) {
    struct Candidate *c;
    int i;
    if (cost <= 0 || g_nfound >= 256) {
        return;
    }
    c = &g_found[g_nfound++];
    c->n = g_path_n;
    c->cost = cost;
    c->laps = laps;
    c->lane = g_path_lane[g_path_n - 1];
    c->sec = g_path_sec[g_path_n - 1];
    for (i = 0; i < g_path_n; i++) {
        c->path[i].lane = g_path_lane[i];
        c->path[i].sec = g_path_sec[i];
    }
}

static void dfs(struct Race *r, int self, int is_leader, int lane, int sec,
                int laps, int cost, int maxc) {
    int nl, ns, nlap, fin;

    if (cost > 0 && cost <= maxc) {
        save_candidate(cost, laps);
    }
    if (cost >= maxc || g_path_n >= MAX_PATH - 1) {
        return;
    }

    ns = sec + 1;
    nlap = laps;
    if (ns >= r->cfg.sectors) {
        ns = 0;
        nlap++;
    }
    fin = (nlap >= r->cfg.laps_total);
    if (cell_ok(r, lane, ns, self, laps, is_leader)) {
        g_vis[lane][ns] = 1;
        g_path_lane[g_path_n] = lane;
        g_path_sec[g_path_n] = ns;
        g_path_n++;
        if (fin) {
            save_candidate(cost + 1, nlap);
        } else {
            dfs(r, self, is_leader, lane, ns, nlap, cost + 1, maxc);
        }
        g_path_n--;
        g_vis[lane][ns] = 0;
    }

    nl = lane - 1;
    if (nl >= 0 && cell_ok(r, nl, sec, self, laps, is_leader)) {
        g_vis[nl][sec] = 1;
        g_path_lane[g_path_n] = nl;
        g_path_sec[g_path_n] = sec;
        g_path_n++;
        dfs(r, self, is_leader, nl, sec, laps, cost + 1, maxc);
        g_path_n--;
        g_vis[nl][sec] = 0;
    }

    nl = lane + 1;
    if (nl < r->cfg.lanes && cost + 3 <= maxc &&
        cell_ok(r, nl, sec, self, laps, is_leader)) {
        g_vis[nl][sec] = 1;
        g_path_lane[g_path_n] = nl;
        g_path_sec[g_path_n] = sec;
        g_path_n++;
        dfs(r, self, is_leader, nl, sec, laps, cost + 3, maxc);
        g_path_n--;
        g_vis[nl][sec] = 0;
    }
}

static int score_cand(const struct Candidate *c, int laps_total) {
    if (c->laps >= laps_total) {
        return 1000000 - c->cost;
    }
    return c->laps * 10000 + c->sec * 10 - c->lane;
}

static const struct Candidate *pick(struct Race *r, struct Car *car) {
    int i, best = -1, bi = 0, s;
    if (g_nfound == 0) {
        return NULL;
    }
    if (car->strategy == STR_RANDOM) {
        return &g_found[rand() % g_nfound];
    }
    for (i = 0; i < g_nfound; i++) {
        s = score_cand(&g_found[i], r->cfg.laps_total);
        if (s > best) {
            best = s;
            bi = i;
        }
    }
    return &g_found[bi];
}

static void sort_order(struct Race *r) {
    int i, j, t, n = r->cfg.n_cars;
    for (i = 0; i < n; i++) {
        r->order[i] = i;
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            struct Car *a = &r->cars[r->order[i]];
            struct Car *b = &r->cars[r->order[j]];
            int swap = 0;
            if (a->status != CAR_RACING && b->status == CAR_RACING) {
                swap = 1;
            } else if (a->status == CAR_RACING && b->status == CAR_RACING) {
                if (is_ahead(b, a, r->cfg.sectors)) {
                    swap = 1;
                }
            }
            if (swap) {
                t = r->order[i];
                r->order[i] = r->order[j];
                r->order[j] = t;
            }
        }
    }
}

static int count_racing(struct Race *r) {
    int i, n = 0;
    for (i = 0; i < r->cfg.n_cars; i++) {
        if (r->cars[i].status == CAR_RACING) {
            n++;
        }
    }
    return n;
}

static const char *st_name(enum CarStatus s) {
    if (s == CAR_FINISHED) {
        return "финиш";
    }
    if (s == CAR_RETIRED) {
        return "сход";
    }
    return "гонка";
}

static void print_board(struct Race *r) {
    int lane, sec, id;
    emit(r, "  карта (L0 внутри, | финиш):\n");
    for (lane = 0; lane < r->cfg.lanes; lane++) {
        emit(r, "  L%d ", lane);
        for (sec = 0; sec < r->cfg.sectors; sec++) {
            if (sec == 0) {
                emit(r, "|");
            }
            id = r->occ[lane][sec];
            if (id < 0) {
                emit(r, ".");
            } else {
                emit(r, "%c", 'A' + id);
            }
        }
        emit(r, "\n");
    }
}

static void print_positions(struct Race *r) {
    int i;
    emit(r, "  положение:\n");
    for (i = 0; i < r->cfg.n_cars; i++) {
        struct Car *c = &r->cars[i];
        emit(r, "    %c команда %d  круг %d/%d  сектор %d  дорожка %d  [%s]\n",
             'A' + c->id, c->team, c->laps, r->cfg.laps_total,
             c->sec, c->lane, st_name(c->status));
    }
}

static int rank_key(struct Race *r, int id, int *pri, int *sec) {
    struct Car *c = &r->cars[id];
    if (c->status == CAR_FINISHED) {
        *pri = 0;
        *sec = c->dist_at_stop; /* меньше — раньше финиш */
        return 0;
    }
    *pri = 1;
    *sec = -((c->status == CAR_RETIRED) ? c->dist_at_stop : dist_of(c, r->cfg.sectors));
    *sec = *sec * 10 + c->lane;
    return 0;
}

void race_print_standings(struct Race *r, const char *title) {
    int idx[MAX_CARS], i, j, t;
    int team_pts[MAX_TEAMS];
    int pts[] = {9, 6, 4, 3, 2, 1};
    int pa, pb, sa, sb;

    emit(r, "\n=== %s ===\n", title);
    for (i = 0; i < r->cfg.n_cars; i++) {
        idx[i] = i;
    }
    for (i = 0; i < r->cfg.n_cars; i++) {
        for (j = i + 1; j < r->cfg.n_cars; j++) {
            rank_key(r, idx[i], &pa, &sa);
            rank_key(r, idx[j], &pb, &sb);
            if (pb < pa || (pb == pa && sb < sa)) {
                t = idx[i];
                idx[i] = idx[j];
                idx[j] = t;
            }
        }
    }
    memset(team_pts, 0, sizeof(team_pts));
    for (i = 0; i < r->cfg.n_cars; i++) {
        struct Car *c = &r->cars[idx[i]];
        int d = (c->status == CAR_FINISHED) ? dist_of(c, r->cfg.sectors)
                                            : ((c->status == CAR_RETIRED) ? c->dist_at_stop
                                                                          : dist_of(c, r->cfg.sectors));
        emit(r, "  %d) машина %c  команда %d  %s  dist=%d\n",
             i + 1, 'A' + c->id, c->team, st_name(c->status), d);
        if (i < 6) {
            team_pts[c->team] += pts[i];
        }
    }
    emit(r, "командный зачёт:\n");
    for (i = 0; i < r->cfg.n_teams; i++) {
        emit(r, "  команда %d: %d очков\n", i, team_pts[i]);
    }
}

static void retire(struct Race *r, int id, const char *why) {
    struct Car *c = &r->cars[id];
    if (c->status != CAR_RACING) {
        return;
    }
    c->status = CAR_RETIRED;
    c->dist_at_stop = dist_of(c, r->cfg.sectors);
    r->occ[c->lane][c->sec] = -1;
    emit(r, "  !! сход машины %c (%s), дистанция %d\n", 'A' + id, why, c->dist_at_stop);
}

static void bump_blockers(struct Race *r, int leader, const struct Candidate *mv) {
    int k, oid;
    struct Car *L = &r->cars[leader];
    if (!r->cfg.use_lapping) {
        return;
    }
    for (k = 0; k < mv->n; k++) {
        oid = r->occ[mv->path[k].lane][mv->path[k].sec];
        if (oid < 0 || oid == leader) {
            continue;
        }
        if (mv->path[k].lane == 0 && r->cars[oid].status == CAR_RACING &&
            r->cars[oid].laps < L->laps) {
            emit(r, "  блокирование: лидер %c через клетку машины %c\n",
                 'A' + leader, 'A' + oid);
            retire(r, oid, "на пути лидера");
        }
    }
}

void race_init(struct Race *r, const struct Config *c) {
    int place, id, outer;

    memset(r, 0, sizeof(*r));
    r->cfg = *c;
    r->out_fd = STDOUT_FILENO;
    r->log_fd = -1;
    r->finish_seq = 0;
    if (c->log_path && c->log_path[0]) {
        r->log_fd = open(c->log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    srand(c->seed);
    occ_clear(r);
    outer = c->lanes - 1;

    for (place = 0; place < c->n_cars; place++) {
        id = c->start_order[place];
        r->cars[id].id = id;
        r->cars[id].team = id / c->cars_per_team;
        r->cars[id].lane = outer;
        r->cars[id].sec = c->n_cars - 1 - place;
        r->cars[id].laps = 0;
        r->cars[id].bonus = 0;
        r->cars[id].status = CAR_RACING;
        r->cars[id].strategy = c->default_strategy;
        r->order[place] = id;
    }
    occ_rebuild(r);
    r->n_racing = c->n_cars;
}

static int base_limit(int place, int prev, int lead) {
    if (place == 0) {
        return 4;
    }
    if (place == 1) {
        return (lead < 1) ? 1 : lead + 2;
    }
    return (prev < 1) ? 1 : prev + 1;
}

static int do_move(struct Race *r, int id, int place, int prev, int lead_cost) {
    struct Car *car = &r->cars[id];
    const struct Candidate *mv;
    int maxc, i, bonus_use, base;
    int old_lane, old_sec, old_laps;
    int is_leader = (place == 0);

    if (car->status != CAR_RACING) {
        return prev;
    }

    base = base_limit(place, prev, lead_cost);
    maxc = base;
    if (r->cfg.use_bonus) {
        maxc += car->bonus;
    }

    emit(r, "ход %c (команда %d, %s): лимит %d",
         'A' + id, car->team,
         car->strategy == STR_RANDOM ? "random" : "greedy", maxc);
    if (car->bonus > 0 && r->cfg.use_bonus) {
        emit(r, " премия=%d", car->bonus);
    }
    emit(r, "\n");

    r->occ[car->lane][car->sec] = -1;
    memset(g_vis, 0, sizeof(g_vis));
    g_vis[car->lane][car->sec] = 1;
    g_nfound = 0;
    g_path_n = 0;
    dfs(r, id, is_leader, car->lane, car->sec, car->laps, 0, maxc);
    mv = pick(r, car);

    if (!mv) {
        emit(r, "  нулевой ход: нет допустимого маршрута\n");
        car->zero_move = 1;
        r->occ[car->lane][car->sec] = id;
        return 0;
    }

    car->zero_move = 0;
    old_lane = car->lane;
    old_sec = car->sec;
    old_laps = car->laps;

    bump_blockers(r, id, mv);

    bonus_use = 0;
    if (r->cfg.use_bonus && mv->cost > base) {
        bonus_use = mv->cost - base;
        if (bonus_use > car->bonus) {
            bonus_use = car->bonus;
        }
        car->bonus -= bonus_use;
    }

    emit(r, "  маршрут cost=%d: (%d,%d)", mv->cost, old_lane, old_sec);
    for (i = 0; i < mv->n; i++) {
        emit(r, "->(%d,%d)", mv->path[i].lane, mv->path[i].sec);
        if (mv->path[i].lane != (i ? mv->path[i - 1].lane : old_lane)) {
            emit(r, "[дорожка]");
        }
    }
    emit(r, "\n");

    car->lane = mv->lane;
    car->sec = mv->sec;
    car->laps = mv->laps;
    r->occ[car->lane][car->sec] = id;

    if (car->laps > old_laps && car->laps < r->cfg.laps_total) {
        emit(r, "  прохождение круга %d машиной %c\n", car->laps, 'A' + id);
        if (r->cfg.use_bonus) {
            car->bonus = 3;
            emit(r, "  начисление премии +3\n");
        }
    }
    if (car->laps >= r->cfg.laps_total) {
        r->finish_seq++;
        car->status = CAR_FINISHED;
        car->dist_at_stop = r->finish_seq;
        emit(r, "  ФИНИШ машины %c (порядок %d)\n", 'A' + id, r->finish_seq);
    }
    if (bonus_use) {
        emit(r, "  списана премия %d\n", bonus_use);
    }
    return mv->cost;
}

static void after_round(struct Race *r) {
    int i, leader = -1, ld;

    for (i = 0; i < r->cfg.n_cars; i++) {
        if (r->cars[i].status == CAR_RACING && r->cars[i].zero_move) {
            retire(r, i, "нулевой ход");
        }
        r->cars[i].zero_move = 0;
    }

    occ_rebuild(r);

    if (r->cfg.use_lapping) {
        for (i = 0; i < r->cfg.n_cars; i++) {
            if (r->cars[i].status == CAR_RACING) {
                if (leader < 0 || is_ahead(&r->cars[i], &r->cars[leader], r->cfg.sectors)) {
                    leader = i;
                }
            }
        }
        if (leader >= 0) {
            ld = dist_of(&r->cars[leader], r->cfg.sectors);
            for (i = 0; i < r->cfg.n_cars; i++) {
                if (i != leader && r->cars[i].status == CAR_RACING &&
                    ld - dist_of(&r->cars[i], r->cfg.sectors) >= r->cfg.sectors) {
                    retire(r, i, "отставание на круг");
                }
            }
        }
    }

    occ_rebuild(r);
    sort_order(r);
    r->n_racing = count_racing(r);
}

void race_run(struct Race *r) {
    int i, k, id, prev, lead_cost, done;
    const char *reason = "лимит раундов";

    emit(r, "судья: старт, seed=%u, трасса %dx%d, кругов %d\n",
         r->cfg.seed, r->cfg.lanes, r->cfg.sectors, r->cfg.laps_total);
    emit(r, "команд %d x %d машин, премии %s, отставшие %s\n",
         r->cfg.n_teams, r->cfg.cars_per_team,
         r->cfg.use_bonus ? "вкл" : "выкл",
         r->cfg.use_lapping ? "вкл" : "выкл");
    emit(r, "старт (внешняя): ");
    for (i = 0; i < r->cfg.n_cars; i++) {
        emit(r, "%c ", 'A' + r->cfg.start_order[i]);
    }
    emit(r, "\n");
    print_board(r);
    print_positions(r);

    while (!r->stop_flag) {
        r->round++;
        if (r->round > r->cfg.max_rounds) {
            reason = "лимит раундов";
            break;
        }
        if (count_racing(r) == 0) {
            reason = "все машины финишировали или сошли";
            break;
        }

        emit(r, "\n----- раунд %d -----\nпорядок: ", r->round);
        for (i = 0; i < r->cfg.n_cars; i++) {
            if (r->cars[r->order[i]].status == CAR_RACING) {
                emit(r, "%c ", 'A' + r->order[i]);
            }
        }
        emit(r, "\n");

        prev = 0;
        lead_cost = 4;
        k = 0;
        for (i = 0; i < r->cfg.n_cars && !r->stop_flag; i++) {
            id = r->order[i];
            if (r->cars[id].status != CAR_RACING) {
                continue;
            }
            prev = do_move(r, id, k, prev, lead_cost);
            if (k == 0) {
                lead_cost = (prev > 0) ? prev : 1;
            }
            k++;
            print_board(r);
            io_sleep_ms(r->cfg.delay_ms);
        }

        after_round(r);
        print_positions(r);

        done = 1;
        for (i = 0; i < r->cfg.n_cars; i++) {
            if (r->cars[i].status == CAR_RACING) {
                done = 0;
            }
        }
        if (done) {
            reason = "финиш или сход всех машин";
            break;
        }
    }

    if (r->stop_flag) {
        reason = "прерывание сигнала";
    }
    emit(r, "\nсудья: гонка остановлена (%s), раундов %d\n", reason, r->round);
    race_print_standings(r, "итоговая классификация");
}
