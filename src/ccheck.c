// ccheck.c — main-process implementation for the Chinese Checkers assignment with deep tracing
// Each line includes a short, purposeful comment as requested.

#define _POSIX_C_SOURCE 200809L              // Request POSIX 2008 behavior.
#include <errno.h>                           // errno / strerror for error logs.
#include <fcntl.h>                           // fcntl for nonblocking toggles.
#include <signal.h>                          // Signals used for child control.
#include <stdarg.h>                          // Variadic formatting for logs.
#include <stdbool.h>                         // bool type and literals.
#include <stdint.h>                          // Fixed-width integer typedefs.
#include <stdio.h>                           // Stdio for FILE*, printf, etc.
#include <stdlib.h>                          // exit, atoi, getenv.
#include <string.h>                          // memset, strlen, strcmp.
#include <sys/types.h>                       // pid_t.
#include <sys/wait.h>                        // waitpid for reaping children.
#include <time.h>                            // clock_gettime for timestamps.
#include <unistd.h>                          // fork/pipe/dup2/close/usleep.
#include "ccheck.h"                         // Assignment-provided API.

/*=========================
  Configuration structure
=========================*/

typedef struct Config {                       // Launch-time switches.
    bool play_white_engine;                   // If true, engine controls white.
    bool play_black_engine;                   // If true, engine controls black.
    bool randomized_play;                     // Engine randomization flag.
    bool verbose_stats;                       // Engine verbose flag.
    bool no_display;                          // Disable xdisp entirely.
    bool tournament_mode;                     // Text protocol mode for engines.
    int  avg_time;                            // Seconds/turn budget for engine.
    const char *init_file;                    // Optional history to preload.
    const char *transcript;                   // Optional transcript output.
} Config;

/*=========================
  Global child state
=========================*/

static pid_t g_disp_pid = -1;                 // PID of xdisp child or -1.
static pid_t g_eng_pid  = -1;                 // PID of engine child or -1.

static FILE *g_disp_in  = NULL;               // To xdisp stdin.
static FILE *g_disp_out = NULL;               // From xdisp stdout.
static FILE *g_eng_in   = NULL;               // To engine stdin.
static FILE *g_eng_out  = NULL;               // From engine stdout.
static FILE *g_tx       = NULL;               // Transcript stream (optional).

/*=========================
  Signal flags (async-safe)
=========================*/

static volatile sig_atomic_t g_got_sigint  = 0; // Ctrl-C flag.
static volatile sig_atomic_t g_got_sigterm = 0; // Termination request.
static volatile sig_atomic_t g_got_sigpipe = 0; // Broken pipe indicator.
static volatile sig_atomic_t g_got_sigchld = 0; // Child state change flag.

/*=========================
  Engine globals provided
=========================*/

extern int randomized;                        // Engine randomization control.
extern int verbose;                           // Engine prints extra stats.
extern int avgtime;                           // Avg seconds per move.
extern int depth;                             // Current/target depth.
extern int times[];                           // Timing model array.
extern Move principal_var[];                  // Principal variation buffer.

/*=========================
  Tracing helpers
=========================*/

static int g_trace = 0;                       // Global trace enable toggle.

static const char *ts_now(void) {             // Return static timestamp string.
    static char buf[64];                      // Static buffer for formatting.
    struct timespec t;                        // Wall clock time holder.
    clock_gettime(CLOCK_REALTIME, &t);        // Read current time.
    long ms = (long)(t.tv_nsec / 1000000L);   // Convert nanoseconds to ms.
    struct tm tm;                             // Broken-down local time.
    localtime_r(&t.tv_sec, &tm);              // Convert to local time safely.
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return buf;                               // Return pointer to static buf.
}

static void tlog(const char *fmt, ...) __attribute__((format(printf,1,2))); // printf-like attr.
static void tlog(const char *fmt, ...) {     // Trace logger guarded by g_trace.
    if (!g_trace) return;                     // Skip if tracing disabled.
    va_list ap;                               // Vararg list for formatting.
    va_start(ap, fmt);                        // Start varargs processing.
    fprintf(stderr, "[trace %s] ", ts_now()); // Prefix with timestamp.
    vfprintf(stderr, fmt, ap);                // Print caller-provided format.
    fprintf(stderr, "\n");                   // Terminate with newline.
    va_end(ap);                               // End varargs processing.
}

static void rawlog(const char *fmt, ...) __attribute__((format(printf,1,2))); // Unconditional.
static void rawlog(const char *fmt, ...) {  // Always-on prefix "[ccheck]".
    va_list ap;                               // Vararg holder.
    va_start(ap, fmt);                         // Start varargs.
    fprintf(stderr, "[ccheck] ");             // Component tag.
    vfprintf(stderr, fmt, ap);                 // Body text.
    fprintf(stderr, "\n");                   // Newline.
    va_end(ap);                                // End varargs.
}

static void die(const char *fmt, ...) __attribute__((format(printf,1,2)));   // Exit-on-error.
static void die(const char *fmt, ...) {
    va_list ap;                               // Varargs for message.
    va_start(ap, fmt);                         // Start varargs.
    fprintf(stderr, "ccheck: ");              // Program tag.
    vfprintf(stderr, fmt, ap);                 // Error description.
    fprintf(stderr, "\n");                   // Newline.
    va_end(ap);                                // End varargs.

    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM); // Ask xdisp to exit.
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM); // Ask engine to exit.

    if (g_disp_in)  fclose(g_disp_in);         // Close xdisp stdin.
    if (g_disp_out) fclose(g_disp_out);        // Close xdisp stdout.
    if (g_eng_in)   fclose(g_eng_in);          // Close engine stdin.
    if (g_eng_out)  fclose(g_eng_out);         // Close engine stdout.
    if (g_tx)       fclose(g_tx);              // Close transcript file.

    if (g_disp_pid > 0) { usleep(100000); kill(g_disp_pid, SIGKILL); } // Hard-kill if needed.
    if (g_eng_pid  > 0) { usleep(100000); kill(g_eng_pid,  SIGKILL); } // Hard-kill if needed.

    while (waitpid(-1, NULL, WNOHANG) > 0) {}  // Reap any remaining children.
    exit(EXIT_FAILURE);                        // Exit with failure status.
}

static void info(const char *fmt, ...) __attribute__((format(printf,1,2))); // Soft log helper.
static void info(const char *fmt, ...) {
    va_list ap;                               // Varargs holder.
    va_start(ap, fmt);                         // Start varargs.
    fprintf(stderr, "[ccheck] ");             // Tag.
    vfprintf(stderr, fmt, ap);                 // Message content.
    fprintf(stderr, "\n");                   // Newline.
    va_end(ap);                                // End varargs.
}

static void log_child_status(pid_t pid, int status, const char *who) { // Child exit report.
    if (WIFEXITED(status)) {                  // Check normal exit.
        info("%s (pid %d) exited with code %d", who, (int)pid, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {         // Check signal-induced exit.
        info("%s (pid %d) killed by signal %d", who, (int)pid, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {          // Check if child stopped.
        info("%s (pid %d) stopped by signal %d", who, (int)pid, WSTOPSIG(status));
    } else {                                  // Fallback for other states.
        info("%s (pid %d) changed state (status=0x%x)", who, (int)pid, status);
    }
}

/*=========================
  Signal plumbing
=========================*/

static void on_sigint (int s) { (void)s; g_got_sigint  = 1; tlog("SIGINT flagged"); }
static void on_sigterm(int s) { (void)s; g_got_sigterm = 1; tlog("SIGTERM flagged"); }
static void on_sigpipe(int s) { (void)s; g_got_sigpipe = 1; tlog("SIGPIPE flagged"); }
static void on_sigchld(int s) { (void)s; g_got_sigchld = 1; tlog("SIGCHLD flagged"); }

static void install_handlers(void) {           // Install synchronous handlers.
    struct sigaction sa = { .sa_flags = SA_RESTART }; // Restart syscalls on signal.
    sigemptyset(&sa.sa_mask);                  // Clear mask.

    sa.sa_handler = on_sigint;  if (sigaction(SIGINT, &sa, NULL) < 0)  die("sigaction SIGINT: %s", strerror(errno));
    sa.sa_handler = on_sigterm; if (sigaction(SIGTERM, &sa, NULL) < 0) die("sigaction SIGTERM: %s", strerror(errno));
    sa.sa_handler = on_sigpipe; if (sigaction(SIGPIPE, &sa, NULL) < 0) die("sigaction SIGPIPE: %s", strerror(errno));
    sa.sa_handler = on_sigchld; if (sigaction(SIGCHLD, &sa, NULL) < 0) die("sigaction SIGCHLD: %s", strerror(errno));
}

static void reap_children_nonblock(void) {     // Non-blocking waitpid loop.
    int saved = errno;                         // Preserve errno across loop.
    while (1) {                                // Reap until no child states.
        int status;                            // Status receive buffer.
        pid_t pid = waitpid(-1, &status, WNOHANG); // Poll for any child.
        if (pid <= 0) break;                   // Exit when none pending.
        if (pid == g_disp_pid) { log_child_status(pid, status, "xdisp"); g_disp_pid = -1; }
        else if (pid == g_eng_pid) { log_child_status(pid, status, "engine"); g_eng_pid = -1; }
        else { log_child_status(pid, status, "child"); }
    }
    errno = saved;                             // Restore errno for callers.
}

/*=========================
  Small I/O helpers
=========================*/

static FILE *fdopen_checked(int fd, const char *mode) { // Wrap fd into FILE*.
    FILE *f = fdopen(fd, mode);              // Convert descriptor to stream.
    if (!f) die("fdopen: %s", strerror(errno)); // Bail on failure.
    setvbuf(f, NULL, _IOLBF, 0);             // Line-buffered for responsiveness.
    return f;                                 // Return configured FILE*.
}

static void close_fd_if_open(int *fd) {       // Close descriptor if valid.
    if (*fd >= 0) { close(*fd); *fd = -1; }   // Close and mark as invalid.
}

static bool read_line(FILE *in, char *buf, size_t bufsz) { // Safe fgets wrapper.
    return fgets(buf, (int)bufsz, in) != NULL; // Return true unless EOF/error.
}

/*=========================
  External engine entry
=========================*/

extern void my_engine(Board *);               // Engine entry point provided.

/*=========================
  Spawn children
=========================*/

static void spawn_display_if_needed(const Config *cfg) { // Start xdisp if allowed.
    if (cfg->no_display) { tlog("display disabled by -d"); return; } // Respect -d flag.

    int to_disp[2], from_disp[2];             // Pipes to/from xdisp.
    if (pipe(to_disp) < 0 || pipe(from_disp) < 0) // Create two pipes.
        die("pipe: %s", strerror(errno));     // Abort on failure.

    pid_t pid = fork();                        // Fork child process.
    if (pid < 0) die("fork xdisp: %s", strerror(errno)); // Abort on failure.

    if (pid == 0) {                            // Child branch: exec xdisp.
        dup2(to_disp[0], STDIN_FILENO);        // Wire pipe into stdin.
        dup2(from_disp[1], STDOUT_FILENO);     // Wire pipe out of stdout.
        close_fd_if_open(&to_disp[0]);         // Close extra fds in child.
        close_fd_if_open(&to_disp[1]);         // Close write end in child.
        close_fd_if_open(&from_disp[0]);       // Close read end in child.
        close_fd_if_open(&from_disp[1]);       // Close extra write end.
        execlp("util/xdisp", "xdisp", NULL); // Replace process image.
        _exit(127);                             // Exec failed -> exit child.
    }

    g_disp_pid = pid;                          // Stash child pid.
    close_fd_if_open(&to_disp[0]);             // Close read end in parent.
    close_fd_if_open(&from_disp[1]);           // Close write end in parent.
    g_disp_in  = fdopen_checked(to_disp[1], "w"); // Create FILE* for input.
    g_disp_out = fdopen_checked(from_disp[0], "r"); // Create FILE* for output.

    info("xdisp spawned pid=%d", (int)g_disp_pid); // Report child pid.

    char ready[256];                           // Buffer for banner read.
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 150000000 }; // 150ms wait.
    fcntl(fileno(g_disp_out), F_SETFL, O_NONBLOCK); // Temporarily non-blocking.
    if (fgets(ready, sizeof(ready), g_disp_out))    // Try to read banner line.
        info("xdisp banner: %s", ready);      // Log banner if present.
    else
        nanosleep(&ts, NULL);                  // Small delay to stabilize.

    int fl = fcntl(fileno(g_disp_out), F_GETFL);    // Get current flags.
    fcntl(fileno(g_disp_out), F_SETFL, fl & ~O_NONBLOCK); // Back to blocking.
}

static void spawn_engine_if_needed(const Config *cfg, Board *bp) {
    info("BEGIN spawn_engine_if_needed");

    if (!cfg->play_white_engine && !cfg->play_black_engine) {
        tlog("no engine requested (-w/-b not set)");
        return;
    }

    int to_eng[2], from_eng[2];
    if (pipe(to_eng) < 0 || pipe(from_eng) < 0)
        die("pipe: %s", strerror(errno));

    // Log the raw file descriptors right after pipe creation
    info("to_eng[0]=%d (read end), to_eng[1]=%d (write end)", to_eng[0], to_eng[1]);
    info("from_eng[0]=%d (read end), from_eng[1]=%d (write end)", from_eng[0], from_eng[1]);

    pid_t pid = fork();
    if (pid < 0)
        die("fork engine: %s", strerror(errno));

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

    // Log fd numbers and /proc paths for debugging
    int fd_in  = fileno(g_eng_in);
    int fd_out = fileno(g_eng_out);
    char path_in[64], path_out[64];
    snprintf(path_in, sizeof(path_in), "/proc/self/fd/%d", fd_in);
    snprintf(path_out, sizeof(path_out), "/proc/self/fd/%d", fd_out);

    info("engine input: fd=%d -> %s", fd_in, path_in);
    info("engine output: fd=%d -> %s", fd_out, path_out);
    info("engine spawned pid=%d roles: white=%d black=%d",
         (int)g_eng_pid, (int)cfg->play_white_engine, (int)cfg->play_black_engine);
}

/*=========================
  CLI parsing
=========================*/

static void parse_args(Config *cfg, int argc, char **argv) { // Parse CLI.
    memset(cfg, 0, sizeof(*cfg));             // Initialize config to zeros.
    cfg->avg_time = 0;                        // Default time budget disabled.

    int opt;                                  // Option character holder.
    while ((opt = getopt(argc, argv, ":wbrvdta:i:o:")) != -1) { // Parse options.
        switch (opt) {                        // Handle each flag.
            case 'w': cfg->play_white_engine = true; break;   // Engine is white.
            case 'b': cfg->play_black_engine = true; break;   // Engine is black.
            case 'r': cfg->randomized_play   = true; break;   // Enable randomness.
            case 'v': cfg->verbose_stats     = true; break;   // Engine verbose.
            case 'd': cfg->no_display        = true; break;   // Disable GUI.
            case 't': cfg->tournament_mode   = true; break;   // Text protocol.
            case 'a': cfg->avg_time          = atoi(optarg); break; // Seconds/turn.
            case 'i': cfg->init_file         = optarg; break; // History file path.
            case 'o': cfg->transcript        = optarg; break; // Transcript file.
            case ':': die("missing argument for -%c", optopt); // Missing arg.
            default:  die("unknown option -%c", optopt);       // Unknown flag.
        }
    }

    randomized = cfg->randomized_play ? 1 : 0; // Bind global engine flags.
    verbose    = cfg->verbose_stats   ? 1 : 0; // Enable verbose stats if -v.
    avgtime    = cfg->avg_time;                // Pass time budget to engine.

    const char *env = getenv("CCHECK_TRACE"); // Optional env toggle for trace.
    g_trace = (env && *env && strcmp(env, "0") != 0); // Any non-empty/"0" enables.
    if (g_trace) info("trace enabled via CCHECK_TRACE"); // Note trace activation.
}

/*=========================
  Protocol helpers
=========================*/

static void send_display_move(Board *bp, Player p, Move m) { // Tell GUI a move.
    if (g_disp_pid <= 0) return;              // Skip if no GUI child present.

    const char *side = (p == X) ? "white" : "black"; // Map player to label.

    tlog("GUI <= >%s:(move follows)", side);  // Trace before serialization.

    fprintf(g_disp_in, ">%s:", side);        // Prefix expected by xdisp.
    print_move(bp, m, g_disp_in);              // Serialize move using helper.
    fputc('\n', g_disp_in);                   // Terminate with newline.
    fflush(g_disp_in);                         // Flush promptly to child.
}

static void write_transcript_move(Board *bp, Player p, Move m) { // Append transcript.
    if (!g_tx) return;                         // Skip if transcript disabled.
    int ply = move_number(bp);                 // Get half-move number.
    int turn = (ply / 2) + 1;                  // Convert to full turn number.
    const char *side = (p == X) ? "white" : "black"; // Label for player.
    char mv[128];                               // Temporary move buffer.
    FILE *mem = fmemopen(mv, sizeof(mv), "w"); // Memory stream for printing.
    print_move(bp, m, mem); fflush(mem); fclose(mem); // Serialize then close.
    if (p == X) fprintf(g_tx, "%d. %s:%s", turn, side, mv); // White line format.
    else        fprintf(g_tx, "%d. ... %s:%s", turn, side, mv); // Black line format.
    fflush(g_tx);                               // Flush to disk.
}

/*=========================
  Initialization from -i
=========================*/

static void load_history_if_any(Board *bp, const Config *cfg) { // Preload moves.
    if (!cfg->init_file) return;              // Skip if no history provided.
    FILE *f = fopen(cfg->init_file, "r");    // Open history file.
    if (!f) die("open -i %s: %s", cfg->init_file, strerror(errno)); // Abort on error.

    info("replaying history from %s", cfg->init_file); // Log replay start.
    while (1) {                                // Loop through all moves.
        Move m = read_move_from_pipe(f, bp);   // Read next move from file.
        if (m == 0) break;                     // Stop on EOF / no move.
        Player p = player_to_move(bp);         // Determine side to move.
        send_display_move(bp, p, m);           // Show on GUI using pre-state.
        apply(bp, m);                           // Apply to referee board.
    }
    fclose(f);                                  // Close history file.
}

/*=========================
  Engine sync helpers
=========================*/

static void notify_engine_of_opponent_move(Board *bp, Player mover, const Config *cfg, Move m) {
    if (g_eng_pid <= 0) return;

    bool engine_is_white = cfg->play_white_engine;
    bool engine_is_black = cfg->play_black_engine;
    bool mover_is_opponent =
        (mover == X && engine_is_black) ||
        (mover == O && engine_is_white);

    if (!mover_is_opponent) return;

    tlog("engine <= opponent move follows");

    // ✅ FIX: send prefix '>' but do NOT repeat the color
    fprintf(g_eng_in, ">");
    print_move(bp, m, g_eng_in);   // already prints "white:D1-D2" or "black:D1-D2"
    fputc('\n', g_eng_in);
    fflush(g_eng_in);

    if (kill(g_eng_pid, SIGHUP) < 0) {
        info("failed to signal engine for opponent move: %s", strerror(errno));
        return;
    }

    char ack[128];
    if (read_line(g_eng_out, ack, sizeof(ack))) {
        size_t n = strlen(ack);
        while (n && (ack[n-1] == '\n' || ack[n-1] == '\r')) ack[--n] = 0;
        info("engine ack: %s", ack);
    } else {
        info("no ack received from engine after opponent move");
    }
}

static Move request_move_from_engine(Board *bp) { // Ask child engine for a move.
    if (g_eng_pid <= 0)
        die("engine requested but no engine child"); // Enforce presence.

    info("requesting move from engine...");   // Step 1: preparing engine query

    // Send '<' marker to engine input
    if (fputs("<\n", g_eng_in) == EOF || fflush(g_eng_in) == EOF)
        die("failed to request engine move"); // Abort if write fails.

    // Wake engine via SIGHUP
    if (kill(g_eng_pid, SIGHUP) < 0)
        die("failed to signal engine (SIGHUP): %s", strerror(errno));

    info("requesting move from engine1... (SIGHUP sent, waiting for output)");

    char line[256];                            // Buffer for engine output
    Move m = 0;                                // Parsed move holder

    // Main reading loop — read one line at a time from engine stdout
    while (read_line(g_eng_out, line, sizeof(line))) {
        info("requesting move from engine2... (raw read started)");

        // Log the raw content before trimming
        fprintf(stderr, "[ccheck-debug] raw engine line: '%s'\n", line);
        fflush(stderr);

        size_t n = strlen(line);               // Compute string length
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;                     // Trim newline chars

        // Log after trimming
        fprintf(stderr, "[ccheck-debug] trimmed engine line: '%s'\n", line);
        fflush(stderr);

        if (n == 0) {
            info("engine output blank line, skipping...");
            continue; // Skip blanks
        }

        if (line[0] == '[') {
            info("engine log/debug line detected -> '%s' (skipping)", line);
            continue; // Skip debug lines
        }

        info("requesting move from engine21... (attempting to parse line)");

        char *payload = line; // Pointer to parse start
        if (!strncmp(payload, "white:", 6)) {
            info("detected 'white:' prefix in engine output");
            payload += 6;
        } else if (!strncmp(payload, "black:", 6)) {
            info("detected 'black:' prefix in engine output");
            payload += 6;
        }

        while (*payload == ' ')
            payload++; // Trim extra spaces

        fprintf(stderr, "[ccheck-debug] payload for parse: '%s'\n", payload);
        fflush(stderr);

        info("requesting move from engine22... (opening memory stream)");
        FILE *mem = fmemopen(payload, strlen(payload), "r");
        if (!mem) {
            info("fmemopen failed for payload (skipping line)");
            continue;
        }

        // Attempt to parse move using referee parser
        m = read_move_from_pipe(mem, bp);
        fclose(mem);

        // Log parse result
        if (m == 0) {
            info("read_move_from_pipe() returned 0 (invalid move string)");
        } else {
            info("read_move_from_pipe() returned nonzero (potentially valid move)");
        }

        info("requesting move from engine23... (post-parse validation)");
        if (m != 0) {
            tlog("engine raw: '%s'", line);
            tlog("engine parsed -> (move object)");

            info("requesting move from engine24... (checking legality)");
            if (!legal_move(m, bp)) {
                print_bd(bp, stderr);
                die("engine move illegal for referee board");
            }

            info("engine move parsed and validated successfully");
            return m;
        }
    }

    // If we reached here, no valid move came through
    info("requesting move from engine3... (no valid move lines left)");

    reap_children_nonblock(); // Reap in case engine died
    die("engine produced no valid move text");
    return 0; // Unreached, placates compiler
}


static Move request_move_from_display(Board *bp) { // Ask GUI for human move.
    if (g_disp_pid <= 0) die("display requested but no display child"); // Require GUI.
    fputs("<\n", g_disp_in);                   // Send request marker.
    fflush(g_disp_in);                          // Flush immediately.
    if (kill(g_disp_pid, SIGHUP) < 0) die("display SIGHUP: %s", strerror(errno)); // Wake GUI.
    Move m = read_move_from_pipe(g_disp_out, bp); // Parse move from GUI.
    if (m == 0) die("display produced EOF instead of a move"); // Abort on EOF.
    return m;                                   // Return GUI-supplied move.
}

/*=========================
  Main referee loop
=========================*/

static void game_loop(Board *bp, const Config *cfg) { // Core play loop.
    for (;;) {                                 // Loop until win or signal.
        if (g_got_sigint || g_got_sigterm) { info("received termination signal"); break; } // Respect exit.
        if (g_got_sigchld) { reap_children_nonblock(); g_got_sigchld = 0; } // Handle child changes.
        if (g_got_sigpipe) { info("SIGPIPE encountered"); g_got_sigpipe = 0; } // Log pipes.

        int ended = game_over(bp);             // Query win state.
        if (ended) {                           // If someone won, announce.
            fprintf(stdout, ended == 1 ? "X (white) wins!" : "O (black) wins!");
            fflush(stdout);                    // Flush message.
            info("game over announced");       // Log end of game.
            break;                             // Leave loop.
        }

        Player p = player_to_move(bp);         // Determine side to play.
        bool p_is_engine = (p == X) ? cfg->play_white_engine : cfg->play_black_engine; // Engine turn?
        tlog("turn begin: ply=%d side=%s engine=%d", move_number(bp), (p==X)?"white":"black", (int)p_is_engine); // Trace turn header.

        Move m = 0;                            // Placeholder for chosen move.
        bool from_display = false;             // Track input origin.

        info("0...");   // Log action.
        if (p_is_engine) {                     // Engine will choose move.
            rawlog("[turn] engine (%s) thinking...", (p==X) ? "white" : "black"); // Announce action.
            m = request_move_from_engine(bp);  // Query child engine.
        } else if (!cfg->no_display && !cfg->tournament_mode) { // Human via GUI.
            rawlog("[turn] your move as %s (click on the board)", (p==X) ? "white" : "black"); // Prompt.
            m = request_move_from_display(bp); // Read GUI move.
            from_display = true;               // Record that GUI supplied it.
        } else {                               // Human via stdin fallback.
            rawlog("[turn] your move as %s (type coordinates)", (p==X) ? "white" : "black"); // Prompt.
            m = read_move_interactive(bp);     // Parse textual move.
        }
        
        info("1...");   // Log action.

        if (m == 0) { info("source returned EOF/0 move -> exiting loop"); break; } // Graceful exit.

        if (!legal_move(m, bp)) {              // Defend against illegal moves.
            fprintf(stderr, "[ccheck] ILLEGAL move detected before apply: ");
            print_move(bp, m, stderr); fprintf(stderr, "\n"); // Print offender.
            die("Illegal move before apply()"); // Abort hard for debugging.
        }

        info("2...");   // Log action.
        if (!cfg->no_display && !from_display) // If engine or stdin produced it,
            send_display_move(bp, p, m);       // show move on GUI first.

        apply(bp, m);                           // Update authoritative board.
        tlog("applied move to board");         // Trace application.
        
        info("3...");   // Log action.
        
        fprintf(stderr, "[ccheck] Applied move: "); // Confirm to stderr.
        print_move(bp, m, stderr); fprintf(stderr, "\n"); // Emit readable form.

        setclock(p);                            // Update timing model.
        write_transcript_move(bp, p, m);        // Append to transcript if any.

        if (cfg->tournament_mode && p_is_engine) { // Echo move for tournament.
            const char *side = (p == X) ? "white" : "black"; // Label for side.
            char mv[128]; FILE *mem = fmemopen(mv, sizeof(mv), "w"); // Buffer.
            print_move(bp, m, mem); fflush(mem); fclose(mem); // Serialize move.
            fprintf(stdout, "@@@%s:%s", side, mv); fflush(stdout); // Emit marker.
        }

        notify_engine_of_opponent_move(bp, p, cfg, m); // Keep engine in sync.
        tlog("turn end: ply now %d", move_number(bp)); // Trace turn completion.
    }
}

/*=========================
  Entrypoint wrapper
=========================*/

int ccheck(int argc, char *argv[]) {          // Main entry called by wrapper.
    Config cfg;                                // Local config instance.
    parse_args(&cfg, argc, argv);              // Parse command line.
    install_handlers();                         // Hook up signal handlers.

    if (cfg.transcript) {                      // If transcript requested...
        g_tx = fopen(cfg.transcript, "w");    // Open output file.
        if (!g_tx) die("open -o %s: %s", cfg.transcript, strerror(errno)); // Abort on error.
    }

    Board *bp = newbd();                       // Create new referee board.
    if (!bp) die("newbd failed");             // Abort if allocation failed.

    spawn_display_if_needed(&cfg);             // Possibly start xdisp.
    load_history_if_any(bp, &cfg);             // Replay history if provided.
    spawn_engine_if_needed(&cfg, bp);          // Possibly start engine child.

    usleep(200000);                            // Give engine 200ms to arm handlers.
    info("entering game loop");               // Announce loop entry.
    game_loop(bp, &cfg);                       // Run until termination.

    if (g_disp_pid > 0) kill(g_disp_pid, SIGTERM); // Ask GUI to exit.
    if (g_eng_pid  > 0) kill(g_eng_pid,  SIGTERM); // Ask engine to exit.

    if (g_disp_in)  fclose(g_disp_in);         // Close GUI stdin stream.
    if (g_disp_out) fclose(g_disp_out);        // Close GUI stdout stream.
    if (g_eng_in)   fclose(g_eng_in);          // Close engine stdin stream.
    if (g_eng_out)  fclose(g_eng_out);         // Close engine stdout stream.
    if (g_tx)       fclose(g_tx);              // Close transcript file.

    g_got_sigchld = 1;                          // Force a final reap cycle.
    if (g_got_sigchld) reap_children_nonblock(); // Reap any remaining children.

    info("clean shutdown");                   // Log successful shutdown.
    return EXIT_SUCCESS;                        // Indicate success to caller.
}
