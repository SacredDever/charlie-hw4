#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ccheck.h"

typedef struct Config {
    bool play_white_engine;
    bool play_black_engine;
    bool randomized_play;
    bool verbose_stats;
    bool no_display;
    bool tournament_mode;
    int  avg_time;
    const char *init_file;
    const char *transcript;
} Config;

static pid_t g_disp_pid = -1;
static pid_t g_eng_pid  = -1;

static FILE *g_disp_in  = NULL;
static FILE *g_disp_out = NULL;
static FILE *g_eng_in   = NULL;
static FILE *g_eng_out  = NULL;
static FILE *g_tx       = NULL;

static volatile sig_atomic_t g_got_sigint  = 0;
static volatile sig_atomic_t g_got_sigterm = 0;
static volatile sig_atomic_t g_got_sigpipe = 0;
static volatile sig_atomic_t g_got_sigchld = 0;

extern int randomized;
extern int verbose;
extern int avgtime;
extern int depth;
extern int times[];
extern Move principal_var[];

static void die(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "ccheck: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM);
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM);

    if (g_disp_in)  fclose(g_disp_in);
    if (g_disp_out) fclose(g_disp_out);
    if (g_eng_in)   fclose(g_eng_in);
    if (g_eng_out)  fclose(g_eng_out);
    if (g_tx)       fclose(g_tx);

    if (g_disp_pid > 0) { usleep(100000); kill(g_disp_pid, SIGKILL); }
    if (g_eng_pid  > 0) { usleep(100000); kill(g_eng_pid,  SIGKILL); }

    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    exit(EXIT_FAILURE);
}

static void info(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[ccheck] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void log_child_status(pid_t pid, int status, const char *who) {
    if (WIFEXITED(status)) {
        info("%s (pid %d) exited with code %d", who, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        info("%s (pid %d) killed by signal %d", who, (int)pid, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        info("%s (pid %d) stopped by signal %d", who, (int)pid, WSTOPSIG(status));
    } else {
        info("%s (pid %d) changed state (status=0x%x)", who, (int)pid, status);
    }
}

static void on_sigint (int s) { (void)s; g_got_sigint  = 1; }
static void on_sigterm(int s) { (void)s; g_got_sigterm = 1; }
static void on_sigpipe(int s) { (void)s; g_got_sigpipe = 1; }
static void on_sigchld(int s) { (void)s; g_got_sigchld = 1; }

static void install_handlers(void) {
    struct sigaction sa = { .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);

    sa.sa_handler = on_sigint;  if (sigaction(SIGINT, &sa, NULL) < 0)  die("sigaction SIGINT: %s", strerror(errno));
    sa.sa_handler = on_sigterm; if (sigaction(SIGTERM, &sa, NULL) < 0) die("sigaction SIGTERM: %s", strerror(errno));
    sa.sa_handler = on_sigpipe; if (sigaction(SIGPIPE, &sa, NULL) < 0) die("sigaction SIGPIPE: %s", strerror(errno));
    sa.sa_handler = on_sigchld; if (sigaction(SIGCHLD, &sa, NULL) < 0) die("sigaction SIGCHLD: %s", strerror(errno));
}

static void reap_children_nonblock(void) {
    int saved = errno;
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        if (pid == g_disp_pid) { log_child_status(pid, status, "xdisp"); g_disp_pid = -1; }
        else if (pid == g_eng_pid) { log_child_status(pid, status, "engine"); g_eng_pid = -1; }
        else { log_child_status(pid, status, "child"); }
    }
    errno = saved;
}

static FILE *fdopen_checked(int fd, const char *mode) {
    FILE *f = fdopen(fd, mode);
    if (!f) die("fdopen: %s", strerror(errno));
    setvbuf(f, NULL, _IOLBF, 0);
    return f;
}

static void close_fd_if_open(int *fd) {
    if (*fd >= 0) { close(*fd); *fd = -1; }
}

static bool read_line(FILE *in, char *buf, size_t bufsz) {
    return fgets(buf, (int)bufsz, in) != NULL;
}

extern void my_engine(Board *);

static void spawn_display_if_needed(const Config *cfg) {
    if (cfg->no_display) return;

    int to_disp[2], from_disp[2];
    if (pipe(to_disp) < 0 || pipe(from_disp) < 0)
        die("pipe: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork xdisp: %s", strerror(errno));

    if (pid == 0) {
        dup2(to_disp[0], STDIN_FILENO);
        dup2(from_disp[1], STDOUT_FILENO);
        close_fd_if_open(&to_disp[0]);
        close_fd_if_open(&to_disp[1]);
        close_fd_if_open(&from_disp[0]);
        close_fd_if_open(&from_disp[1]);
        execlp("util/xdisp", "xdisp", NULL);
        _exit(127);
    }

    g_disp_pid = pid;
    close_fd_if_open(&to_disp[0]);
    close_fd_if_open(&from_disp[1]);
    g_disp_in  = fdopen_checked(to_disp[1], "w");
    g_disp_out = fdopen_checked(from_disp[0], "r");

    char ready[256];
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 150000000 };
    fcntl(fileno(g_disp_out), F_SETFL, O_NONBLOCK);
    if (fgets(ready, sizeof(ready), g_disp_out))
        info("xdisp banner: %s", ready);
    else
        nanosleep(&ts, NULL);

    int fl = fcntl(fileno(g_disp_out), F_GETFL);
    fcntl(fileno(g_disp_out), F_SETFL, fl & ~O_NONBLOCK);
}

static void spawn_engine_if_needed(const Config *cfg, Board *bp) {
    if (!cfg->play_white_engine && !cfg->play_black_engine) return;

    int to_eng[2], from_eng[2];
    if (pipe(to_eng) < 0 || pipe(from_eng) < 0)
        die("pipe: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork engine: %s", strerror(errno));

    if (pid == 0) {
        dup2(to_eng[0], STDIN_FILENO);
        dup2(from_eng[1], STDOUT_FILENO);
        close_fd_if_open(&to_eng[0]);
        close_fd_if_open(&to_eng[1]);
        close_fd_if_open(&from_eng[0]);
        close_fd_if_open(&from_eng[1]);
        my_engine(bp);
        _exit(0);
    }

    g_eng_pid = pid;
    close_fd_if_open(&to_eng[0]);
    close_fd_if_open(&from_eng[1]);
    g_eng_in  = fdopen_checked(to_eng[1], "w");
    g_eng_out = fdopen_checked(from_eng[0], "r");
}

static void parse_args(Config *cfg, int argc, char **argv) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->avg_time = 0;

    int opt;
    while ((opt = getopt(argc, argv, ":wbrvdta:i:o:")) != -1) {
        switch (opt) {
            case 'w': cfg->play_white_engine = true; break;
            case 'b': cfg->play_black_engine = true; break;
            case 'r': cfg->randomized_play   = true; break;
            case 'v': cfg->verbose_stats     = true; break;
            case 'd': cfg->no_display        = true; break;
            case 't': cfg->tournament_mode   = true; break;
            case 'a': cfg->avg_time          = atoi(optarg); break;
            case 'i': cfg->init_file         = optarg; break;
            case 'o': cfg->transcript        = optarg; break;
            case ':': die("missing argument for -%c", optopt);
            default:  die("unknown option -%c", optopt);
        }
    }

    randomized = cfg->randomized_play ? 1 : 0;
    verbose    = cfg->verbose_stats   ? 1 : 0;
    avgtime    = cfg->avg_time;
}

static void send_display_move(Board *bp, Player p, Move m) {
    if (g_disp_pid <= 0) return;

    char mvbuf[128];
    FILE *mem = fmemopen(mvbuf, sizeof(mvbuf), "w");
    print_move(bp, m, mem);
    fflush(mem); fclose(mem);

    const char *side = (p == X) ? "white" : "black";
    fprintf(g_disp_in, ">%s:%s\n", side, mvbuf);  // Add newline
    fflush(g_disp_in);
}

static void load_history_if_any(Board *bp, const Config *cfg) {
    if (!cfg->init_file) return;
    FILE *f = fopen(cfg->init_file, "r");
    if (!f) die("open -i %s: %s", cfg->init_file, strerror(errno));

    while (1) {
        Move m = read_move_from_pipe(f, bp);
        if (m == 0) break;
        Player p = player_to_move(bp);
        send_display_move(bp, p, m);
        apply(bp, m);
        if (g_tx) {
            int ply = move_number(bp) - 1;
            int turn = (ply / 2) + 1;
            const char *side = (p == X) ? "white" : "black";
            char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
            print_move(bp, m, mem); fflush(mem); fclose(mem);
            if (p == X) fprintf(g_tx, "%d. %s:%s", turn, side, mv);
            else        fprintf(g_tx, "%d. ... %s:%s", turn, side, mv);
            fflush(g_tx);
        }
    }
    fclose(f);
}

static void notify_engine_of_opponent_move(Board *bp, Player mover, const Config *cfg, Move m) {
    if (g_eng_pid <= 0) return;
    bool engine_is_white = cfg->play_white_engine;
    bool engine_is_black = cfg->play_black_engine;
    bool mover_is_opponent = (mover == X && engine_is_black) || (mover == O && engine_is_white);
    if (!mover_is_opponent) return;

    char mvbuf[128]; FILE *mem = fmemopen(mvbuf, sizeof(mvbuf), "w");
    print_move(bp, m, mem); fflush(mem); fclose(mem);

    const char *side = (mover == X) ? "white" : "black";
    char line[192]; snprintf(line, sizeof(line), ">%s:%s\n", side, mvbuf);

    char ack[192];
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (fputs(line, g_eng_in) == EOF || fflush(g_eng_in) == EOF) continue;
        if (kill(g_eng_pid, SIGHUP) < 0) continue;
        if (read_line(g_eng_out, ack, sizeof(ack))) return;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 80000000 };
        nanosleep(&ts, NULL);
    }
    info("warning: engine did not ack opponent move; continuing");
}

static Move request_move_from_engine(Board *bp) {
    if (g_eng_pid <= 0) die("engine requested but no engine child");

    if (fputs("<\n", g_eng_in) == EOF || fflush(g_eng_in) == EOF)
        die("failed to request engine move");

    if (kill(g_eng_pid, SIGHUP) < 0)
        die("failed to signal engine (SIGHUP): %s", strerror(errno));

    Move m = read_move_from_pipe(g_eng_out, bp);
    if (m == 0) {
        // grab latest child status before dying so you see exact cause
        reap_children_nonblock();
        die("engine produced EOF instead of a move");
    }
    return m;
}

static Move request_move_from_display(Board *bp) {
    if (g_disp_pid <= 0) die("display requested but no display child");
    fputs("<\n", g_disp_in);
    fflush(g_disp_in);
    if (kill(g_disp_pid, SIGHUP) < 0) die("display SIGHUP: %s", strerror(errno));
    Move m = read_move_from_pipe(g_disp_out, bp);
    if (m == 0) die("display produced EOF instead of a move");
    return m;
}

static void write_transcript_move(Board *bp, Player p, Move m) {
    if (!g_tx) return;
    int ply = move_number(bp);
    int turn = (ply / 2) + 1;
    const char *side = (p == X) ? "white" : "black";
    char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
    print_move(bp, m, mem); fflush(mem); fclose(mem);
    if (p == X) fprintf(g_tx, "%d. %s:%s", turn, side, mv);
    else        fprintf(g_tx, "%d. ... %s:%s", turn, side, mv);
    fflush(g_tx);
}

static void game_loop(Board *bp, const Config *cfg) {
    for (;;) {
        if (g_got_sigint || g_got_sigterm) break;
        if (g_got_sigchld) { reap_children_nonblock(); g_got_sigchld = 0; }
        if (g_got_sigpipe) { info("SIGPIPE encountered"); g_got_sigpipe = 0; }

        int ended = game_over(bp);
        if (ended) {
            fprintf(stdout, ended == 1 ? "X (white) wins!" : "O (black) wins!");
            fflush(stdout);
            break;
        }

        Player p = player_to_move(bp);
        bool p_is_engine = (p == X) ? cfg->play_white_engine : cfg->play_black_engine;

        Move m = 0;
        bool from_display = false;

        if (p_is_engine) {
            fprintf(stderr, "[turn] engine (%s) thinking...\n", (p==X) ? "white" : "black");
            m = request_move_from_engine(bp);
        } else if (!cfg->no_display && !cfg->tournament_mode) {
            fprintf(stderr, "[turn] your move as %s (click on the board)\n", (p==X) ? "white" : "black");
            m = request_move_from_display(bp);
            from_display = true;
        } else {
            fprintf(stderr, "[turn] your move as %s (type coordinates)\n", (p==X) ? "white" : "black");
            m = read_move_interactive(bp);
        }

        if (m == 0) break;
        setclock(p);
        write_transcript_move(bp, p, m);

        if (!cfg->no_display && !from_display)
            send_display_move(bp, p, m);

        if (cfg->tournament_mode && p_is_engine) {
            const char *side = (p == X) ? "white" : "black";
            char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w");
            print_move(bp, m, mem); fflush(mem); fclose(mem);
            fprintf(stdout, "@@@%s:%s", side, mv);
            fflush(stdout);
        }

        notify_engine_of_opponent_move(bp, p, cfg, m);
        apply(bp, m);
    }
}

int ccheck(int argc, char *argv[]) {
    Config cfg;
    parse_args(&cfg, argc, argv);
    install_handlers();

    if (cfg.transcript) {
        g_tx = fopen(cfg.transcript, "w");
        if (!g_tx) die("open -o %s: %s", cfg.transcript, strerror(errno));
    }

    Board *bp = newbd();
    if (!bp) die("newbd failed");

    spawn_display_if_needed(&cfg);
    load_history_if_any(bp, &cfg);
    spawn_engine_if_needed(&cfg, bp);
    usleep(200000);  // <-- give engine 0.2s to arm its signal handlers
    game_loop(bp, &cfg);

    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM);
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM);

    if (g_disp_in)  fclose(g_disp_in);
    if (g_disp_out) fclose(g_disp_out);
    if (g_eng_in)   fclose(g_eng_in);
    if (g_eng_out)  fclose(g_eng_out);
    if (g_tx)       fclose(g_tx);

    g_got_sigchld = 1;
    if (g_got_sigchld) reap_children_nonblock();

    return EXIT_SUCCESS;
}
