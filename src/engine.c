// engine.c — resilient engine that never prints an illegal/zero move
// Every line is commented per your preference.

#define _POSIX_C_SOURCE 200809L                // Use POSIX for timers/handlers.
#include <stdio.h>                             // stdin/stdout, fprintf.
#include <stdlib.h>                            // exit helpers.
#include <unistd.h>                            // getpid.
#include <signal.h>                            // sigaction, sig* APIs.
#include <setjmp.h>                            // sigsetjmp/siglongjmp.
#include <string.h>                            // memset, strlen, strchr.
#include <stdarg.h>                            // variadic logging.
#include <sys/time.h>                          // setitimer.
#include "ccheck.h"                            // assignment-provided API.
#include "debug.h"                             // print_stats etc.

extern int verbose, avgtime, depth, times[];   // Provided globals.
extern Move principal_var[];                   // Provided principal variation.

static volatile sig_atomic_t g_hup = 0;        // SIGHUP flag from parent.
static volatile sig_atomic_t g_alarm = 0;      // SIGALRM budget expiration.
static volatile sig_atomic_t g_interrupted = 0;// We requested early stop.
static volatile sig_atomic_t g_in_search = 0;  // We are inside bestmove().
static sigjmp_buf g_jmp;                       // Jump target for interrupts.

/* ---------- Logging ---------- */
static void elog(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void elog(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[engine] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(ap);
}

/* ---------- Signals ---------- */
static void on_sighup(int s){
    (void)s;
    g_hup = 1;
    elog("SIGHUP received (in_search=%d)", g_in_search);
    if (g_in_search) {                         // If searching, break out fast.
        g_interrupted = 1;
        elog("SIGHUP interrupting search, jumping...");
        siglongjmp(g_jmp, 1);
    }
}

static void on_sigalrm(int s){
    (void)s;
    g_alarm = 1;
    elog("SIGALRM received (in_search=%d)", g_in_search);
    if (g_in_search) {                         // If searching, break out fast.
        g_interrupted = 1;
        elog("SIGALRM interrupting search, jumping...");
        siglongjmp(g_jmp, 1);
    }
}

/* ---------- Small utils ---------- */
static void arm_timer_seconds(int secs){
    struct itimerval it = {0};
    if (secs > 0) it.it_value.tv_sec = secs;   // One-shot wall timer.
    setitimer(ITIMER_REAL, &it, NULL);
    elog("Timer armed for %d sec(s)", secs);
}

static int read_line(char *buf, size_t cap){
    return fgets(buf, cap, stdin) != NULL;     // 1 on success, 0 on EOF.
}

static void chomp(char *s){
    size_t n = strlen(s);
    if (n && s[n-1] == '\n') s[n-1] = 0;       // Drop trailing newline.
}

static Move parse_move_text(Board *bp, const char *text){
    elog("Parsing move text: '%s'", text);
    FILE *mem = fmemopen((void*)text, strlen(text), "r");  // No copy, read-only.
    if (!mem) return 0;
    Move m = read_move_from_pipe(mem, bp);     // Will abort if text is ill-formed
    fclose(mem);
    return m;
}

static void send_ack(void){
    fputs("ok\n", stdout);                     // Acknowledge opponent move.
    fflush(stdout);
    elog("Sent ack -> parent");
}

/* ---------- Core search helpers ---------- */
static void think_until_interrupted(Board *bp){
    elog("think_until_interrupted() starting");
    g_interrupted = 0;

    if (sigsetjmp(g_jmp, 1) != 0) {            // Resume point after interrupts.
        elog("think_until_interrupted(): jumped (interrupted)");
        g_in_search = 0;
        return;
    }

    for (depth = 1; depth <= MAXPLY; depth++) {
        if (g_hup || g_alarm || g_interrupted) // Respect stop signals quickly.
            break;

        g_in_search = 1;                       // Mark "in search" for handlers.
        elog("Depth %d: calling bestmove()", depth);
        bestmove(bp, player_to_move(bp), 0, principal_var, -MAXEVAL, MAXEVAL);
        g_in_search = 0;

        timings(depth);                        // Update timing model.
        elog("Depth %d done", depth);

        if (verbose) {                         // Optional debug printing.
            print_stats();
            print_pvar(bp, 0);
            fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    elog("think_until_interrupted() complete");
}

/* Ensure principal_var[0] is **non-zero and legal** for bp. */
static int ensure_legal_best_move(Board *bp){
    // If PV already looks good, validate legality.
    if (principal_var[0] && legal_move(principal_var[0], bp))
        return 1;

    elog("PV empty/illegal, computing fallback PV @ depth=1");
    int saved = depth;                         // Save global depth.
    depth = 1;                                 // Minimal search to get *some* move.
    g_interrupted = 0;

    if (sigsetjmp(g_jmp, 1) == 0) {            // Fresh, short search.
        g_in_search = 1;
        bestmove(bp, player_to_move(bp), 0, principal_var, -MAXEVAL, MAXEVAL);
        g_in_search = 0;
        timings(depth);
    } else {
        g_in_search = 0;                       // Interrupted during fallback.
    }

    depth = saved;                             // Restore depth limit.

    // Final check: must be non-zero and legal.
    if (principal_var[0] && legal_move(principal_var[0], bp)) {
        elog("Fallback produced a legal move");
        return 1;
    }

    // Give up cleanly (don’t abort inside print_move/read_move).
    elog("No legal move available for current position -> resigning");
    return 0;
}

/* Print the chosen move **atomically** wrt signals so we don't jump mid-print. */
static int send_best_move_safely(Board *bp){
    if (!ensure_legal_best_move(bp))           // Make sure we've got a valid move.
        return 0;

    sigset_t block, prev;                      // Block HUP/ALRM during print.
    sigemptyset(&block);
    sigaddset(&block, SIGHUP);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, &prev);

    elog("Best move about to print: %u", principal_var[0]);
    print_move(bp, principal_var[0], stdout);  // Must not be called with illegal m.
    fputc('\n', stdout);                       // Trailing newline is required.
    fflush(stdout);
    elog("Best move sent");

    sigprocmask(SIG_SETMASK, &prev, NULL);     // Restore signal mask.
    return 1;
}

/* Apply a '>side:move' line to our internal board and ack. */
static int handle_opponent_line(Board *bp, const char *line){
    const char *colon = strchr(line, ':');     // Split at first ':'.
    if (!colon) return 0;
    Move m = parse_move_text(bp, colon + 1);   // Parse (aborts on malformed).
    if (!m || !legal_move(m, bp)) return 0;    // Safety: must be legal for bp.
    apply(bp, m);                              // Update our board state.
    send_ack();                                // Tell parent we've applied it.
    elog("Applied opponent move successfully");
    return 1;
}

/* ---------- Main engine loops ---------- */
static void engine_main(Board *bp){
    elog("Engine starting up (pid=%d)", getpid());

    // Install handlers (SA_RESTART keeps stdio happy across signals).
    struct sigaction sa = { .sa_flags = SA_RESTART, .sa_handler = on_sighup };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);

    struct sigaction sb = { .sa_flags = SA_RESTART, .sa_handler = on_sigalrm };
    sigemptyset(&sb.sa_mask);
    sigaction(SIGALRM, &sb, NULL);

    // Make stdout line-buffered so the parent always sees lines promptly.
    setvbuf(stdout, NULL, _IOLBF, 0);

    // Clear PV so we never read uninitialized garbage.
    for (int i = 0; i < MAXPLY; i++) principal_var[i] = 0;

    // Disarm timer initially.
    arm_timer_seconds(0);

    // Command buffer from parent.
    char line[512];

    elog("Entering main loop");
    while (1) {
        // Reset per-iteration flags (avoid sticky state).
        g_alarm = 0;
        g_interrupted = 0;

        // Idle-search if no command yet; this pre-warms PV for the next '<'.
        if (!g_hup) {
            think_until_interrupted(bp);
            continue;
        }

        // We have a command pending; clear the flag and read the line.
        g_hup = 0;
        if (!read_line(line, sizeof(line))) {
            elog("stdin EOF -> exiting");
            _exit(0);
        }
        chomp(line);
        elog("Received line: '%s'", line);

        if (line[0] == '<') {                  // Parent requests our move.
            int budget = (avgtime > 0) ? avgtime : 1;
            arm_timer_seconds(budget);         // Budget thinking time.
            elog("Thinking for up to %d sec(s)", budget);
            think_until_interrupted(bp);       // Deepen PV as time allows.
            arm_timer_seconds(0);              // Stop timer before printing.

            if (!send_best_move_safely(bp))    // Print move; never illegal/zero.
                _exit(4);                      // Clean failure: no legal move.

            if (principal_var[0]) {            // Keep our internal board in sync.
                apply(bp, principal_var[0]);
                elog("Applied our own move");
            }

        } else if (line[0] == '>') {           // Parent forwards opponent move.
            if (!handle_opponent_line(bp, line)) {
                elog("Invalid opponent line -> exit(2)");
                _exit(2);
            }
            // After applying opponent move, keep thinking opportunistically.
            g_interrupted = 0;
            think_until_interrupted(bp);

        } else {                               // Unknown protocol line.
            elog("Unknown command '%s' -> exit(3)", line);
            _exit(3);
        }
    }
}

void my_engine(Board *bp) { engine_main(bp); }