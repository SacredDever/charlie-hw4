/*
 * Chinese Checkers Engine
 */

/*
 * To implement this module, remove the following #if 0 and the matching #endif.
 * Fill in your implementation of the engine() function below (you may also add
 * other functions).  When you compile the program, your implementation will be
 * incorporated.  If you leave the #if 0 here, then your program will be linked
 * with a demonstration version of the engine.  You can use this feature to work
 * on your implementation of the main program before you attempt to implement
 * the engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <string.h>
#include <errno.h>

#include "ccheck.h"
#include "debug.h"

/* Global variables (declared in ccheck.h, defined elsewhere) */
extern int verbose;
extern int randomized;
extern int depth;
extern int avgtime;
extern Move principal_var[];
extern int times[];
extern int searchtime;
extern int movetime;
extern int xtime;
extern int otime;

/* Signal handling */
static volatile sig_atomic_t sighup_received = 0;
static volatile sig_atomic_t sigalrm_received = 0;
static sigjmp_buf search_jmpbuf;
static int search_jmpbuf_valid = 0;

/* Signal handler */
static void engine_signal_handler(int sig)
{
    if (sig == SIGHUP) {
        sighup_received = 1;
        if (search_jmpbuf_valid) {
            siglongjmp(search_jmpbuf, 1);
        }
    } else if (sig == SIGALRM) {
        sigalrm_received = 1;
        if (search_jmpbuf_valid) {
            siglongjmp(search_jmpbuf, 1);
        }
    }
}

/* Setup signal handlers */
static void setup_engine_signals(void)
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = engine_signal_handler;
    sa.sa_flags = 0;

    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        perror("sigaction SIGHUP");
        abort();
    }
    if (sigaction(SIGALRM, &sa, NULL) < 0) {
        perror("sigaction SIGALRM");
        abort();
    }
}

void engine(Board *bp)
{
    fprintf(stderr, "DEBUG: Engine: engine() function called, bp=%p\n", (void*)bp);
    if (bp == NULL) {
        fprintf(stderr, "DEBUG: Engine: ERROR - board pointer is NULL!\n");
        abort();
    }
    fprintf(stderr, "DEBUG: Engine: initial board state - move_number=%d, player_to_move=%d\n",
            move_number(bp), player_to_move(bp));
    setup_engine_signals();
    fprintf(stderr, "DEBUG: Engine: signal handlers set up\n");

    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    /* Create a working copy of the board for searching */
    Board *search_bp = newbd();
    copybd(bp, search_bp);
    fprintf(stderr, "DEBUG: Engine: created working board copy for searching\n");

    int current_depth = 1;
    int best_depth = 0;
    int searching_on_opponent_time = 0;
    int first_wait = 1;  /* Flag to check stdin on first wait */

    fprintf(stderr, "DEBUG: Engine: entering main loop\n");
    while (1) {
        /* Wait for SIGHUP to read command from main process */
        if (!sighup_received) {
            fprintf(stderr, "DEBUG: Engine: no SIGHUP, searching_on_opponent_time=%d, best_depth=%d\n",
                    searching_on_opponent_time, best_depth);
            /* While waiting, search on opponent's time if we're not at max depth */
            if (searching_on_opponent_time && best_depth < MAXPLY) {
                for (depth = current_depth; depth <= MAXPLY; depth++) {
                    if (sighup_received) {
                        break; /* Interrupted by SIGHUP */
                    }

                    search_jmpbuf_valid = 1;
                    if (sigsetjmp(search_jmpbuf, 1) != 0) {
                        /* Jumped here from signal handler */
                        search_jmpbuf_valid = 0;
                        break;
                    }

                    reset_stats();
                    {
                        time_t t;
                        time(&t);
                        searchtime = (int)t;
                    }

                    if (verbose) {
                        fprintf(stderr, "Searching depth %d...", depth);
                        fflush(stderr);
                    }

                    /* Restore working board to current state before each search */
                    copybd(bp, search_bp);
                    int score = bestmove(search_bp, player_to_move(search_bp), 0, principal_var, -MAXEVAL, MAXEVAL);

                    search_jmpbuf_valid = 0;

                    timings(depth);

                    if (verbose) {
                        print_stats();
                        print_pvar(bp, 0);
                        fprintf(stderr, "\n");
                    }

                    best_depth = depth;

                    /* Don't continue if position is won or lost */
                    if (score == -(MAXEVAL-1) || score == MAXEVAL-1) {
                        break;
                    }

                    if (sighup_received) {
                        break;
                    }
                }
            } else {
                /* On first wait, check if data is already available (handles race condition) */
                if (first_wait) {
                    first_wait = 0;
                    fd_set readfds;
                    struct timeval timeout;
                    FD_ZERO(&readfds);
                    FD_SET(STDIN_FILENO, &readfds);
                    timeout.tv_sec = 0;
                    timeout.tv_usec = 0; /* Don't wait, just check */
                    
                    int ready = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
                    if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
                        if (!feof(stdin)) {
                            fprintf(stderr, "DEBUG: Engine: data available in stdin on first wait, treating as SIGHUP\n");
                            sighup_received = 1;
                            continue; /* Go process the command */
                        }
                    }
                }
                /* Wait for SIGHUP - the main process will send it when it wants to communicate */
                pause();
            }
        }

        if (sighup_received) {
            fprintf(stderr, "DEBUG: Engine: SIGHUP received, reading command\n");
            sighup_received = 0;
            searching_on_opponent_time = 0;

            char cmd[256];
            if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
                if (feof(stdin)) {
                    fprintf(stderr, "DEBUG: Engine: EOF on stdin (pipe closed), exiting\n");
                } else if (ferror(stdin)) {
                    fprintf(stderr, "DEBUG: Engine: Error reading from stdin, exiting\n");
                } else {
                    fprintf(stderr, "DEBUG: Engine: fgets returned NULL, exiting\n");
                }
                break; /* EOF or error */
            }

            fprintf(stderr, "DEBUG: Engine: received command: '%s' (first char: '%c')\n", cmd, cmd[0]);

            if (cmd[0] == '<') {
                fprintf(stderr, "DEBUG: Engine: Main process wants a move - it's our turn\n");
                /* Main process wants a move - it's our turn */
                /* Search with time constraints if needed */
                struct itimerval timer;
                int time_limit = 0;

                /* Calculate available time */
                if (avgtime > 0) {
                    int moves_made = move_number(bp);
                    Player current_player = player_to_move(bp);
                    int total_time_used = (current_player == X) ? xtime : otime;
                    int time_remaining = (avgtime * (moves_made + 1)) - total_time_used;
                    if (time_remaining > 0) {
                        time_limit = time_remaining;
                    }
                }

                /* Set up alarm if we have a time limit */
                if (time_limit > 0) {
                    timer.it_value.tv_sec = time_limit;
                    timer.it_value.tv_usec = 0;
                    timer.it_interval.tv_sec = 0;
                    timer.it_interval.tv_usec = 0;
                    setitimer(ITIMER_REAL, &timer, NULL);
                    sigalrm_received = 0;
                }

                /* Search iteratively deepening with time constraint */
                /* Always search to at least depth 1 */
                if (current_depth > 1 && best_depth == 0) {
                    current_depth = 1;
                }
                
                fprintf(stderr, "DEBUG: Engine: Starting search, current_depth=%d, best_depth=%d\n", current_depth, best_depth);
                
                for (depth = current_depth; depth <= MAXPLY; depth++) {
                    fprintf(stderr, "DEBUG: Engine: Searching at depth %d\n", depth);
                    if (sighup_received) {
                        fprintf(stderr, "DEBUG: Engine: SIGHUP received during search loop, breaking\n");
                        break;
                    }

                    /* Check if we have time for this depth */
                    if (avgtime > 0 && depth > 1 && times[depth] > 0) {
                        int moves_made = move_number(bp);
                        Player current_player = player_to_move(bp);
                        int total_time_used = (current_player == X) ? xtime : otime;
                        int time_remaining = (avgtime * (moves_made + 1)) - total_time_used;
                        if (time_remaining < times[depth]) {
                            fprintf(stderr, "DEBUG: Engine: Not enough time for depth %d, breaking\n", depth);
                            break; /* Not enough time */
                        }
                    }

                    search_jmpbuf_valid = 1;
                    if (sigsetjmp(search_jmpbuf, 1) != 0) {
                        /* Jumped here from signal handler */
                        fprintf(stderr, "DEBUG: Engine: Jumped from signal handler\n");
                        search_jmpbuf_valid = 0;
                        break;
                    }

                    reset_stats();
                    {
                        time_t t;
                        time(&t);
                        searchtime = (int)t;
                    }

                    fprintf(stderr, "DEBUG: Engine: Calling bestmove at depth %d\n", depth);
                    if (verbose) {
                        fprintf(stderr, "Searching depth %d...", depth);
                        fflush(stderr);
                    }

                    /* Restore working board to current state before each search */
                    copybd(bp, search_bp);
                    fprintf(stderr, "DEBUG: Engine: before bestmove - move_number=%d (main), %d (search), player_to_move=%d\n",
                            move_number(bp), move_number(search_bp), player_to_move(bp));
                    int score = bestmove(search_bp, player_to_move(search_bp), 0, principal_var, -MAXEVAL, MAXEVAL);
                    fprintf(stderr, "DEBUG: Engine: after bestmove - move_number=%d (main), %d (search), player_to_move=%d\n",
                            move_number(bp), move_number(search_bp), player_to_move(bp));

                    fprintf(stderr, "DEBUG: Engine: bestmove returned, score=%d\n", score);
                    search_jmpbuf_valid = 0;

                    timings(depth);

                    if (verbose) {
                        print_stats();
                        print_pvar(bp, 0);
                        fprintf(stderr, "\n");
                    }

                    best_depth = depth;
                    fprintf(stderr, "DEBUG: Engine: Search completed at depth %d, best_depth=%d\n", depth, best_depth);

                    /* Don't continue if position is won or lost */
                    if (score == -(MAXEVAL-1) || score == MAXEVAL-1) {
                        fprintf(stderr, "DEBUG: Engine: Position is won/lost, breaking\n");
                        break;
                    }

                    if (sighup_received || sigalrm_received) {
                        fprintf(stderr, "DEBUG: Engine: Signal received, breaking\n");
                        break;
                    }
                }
                
                fprintf(stderr, "DEBUG: Engine: Search loop finished, best_depth=%d\n", best_depth);

                /* Cancel alarm */
                if (time_limit > 0) {
                    timer.it_value.tv_sec = 0;
                    timer.it_value.tv_usec = 0;
                    timer.it_interval.tv_sec = 0;
                    timer.it_interval.tv_usec = 0;
                    setitimer(ITIMER_REAL, &timer, NULL);
                }

                /* Send best move if we have one */
                fprintf(stderr, "DEBUG: Engine: best_depth=%d after search\n", best_depth);
                if (best_depth >= 1) {
                    Move m = principal_var[0];
                    fprintf(stderr, "DEBUG: Engine: sending move to main process, move=0x%x\n", m);
                    fprintf(stderr, "DEBUG: Engine: current board state - move_number=%d, player_to_move=%d\n",
                            move_number(bp), player_to_move(bp));
                    fprintf(stderr, "DEBUG: Engine: checking if move is legal: %d\n", legal_move(m, bp));
                    /* Print move BEFORE applying it (print_move needs pre-move board state) */
                    print_move(bp, m, stdout);
                    printf("\n");
                    fflush(stdout);
                    fprintf(stderr, "DEBUG: Engine: move printed to stdout, flushed\n");

                    /* Apply move to our board AFTER printing */
                    apply(bp, m);
                    setclock(player_to_move(bp) == X ? O : X);
                    /* Update search board copy */
                    copybd(bp, search_bp);
                    fprintf(stderr, "DEBUG: Engine: move applied to board\n");

                    /* Reset search depth */
                    current_depth = 1;
                    best_depth = 0;
                    fprintf(stderr, "DEBUG: Engine: move sent and applied to board, resetting state\n");
                    fprintf(stderr, "DEBUG: Engine: going back to wait loop\n");
                } else {
                    /* This shouldn't happen, but if it does, search to depth 1 */
                    fprintf(stderr, "Warning: No move ready, forcing depth 1 search\n");
                    depth = 1;
                    reset_stats();
                    {
                        time_t t;
                        time(&t);
                        searchtime = (int)t;
                    }
                    bestmove(bp, player_to_move(bp), 0, principal_var, -MAXEVAL, MAXEVAL);
                    timings(1);
                    best_depth = 1;
                    
                    Move m = principal_var[0];
                    print_move(bp, m, stdout);
                    printf("\n");
                    fflush(stdout);
                    apply(bp, m);
                    setclock(player_to_move(bp) == X ? O : X);
                    current_depth = 1;
                    best_depth = 0;
                }
            } else if (cmd[0] == '>') {
                fprintf(stderr, "DEBUG: Engine: Main process sending opponent's move\n");
                /* Main process sending opponent's move */
                /* Read the move */
                char *move_str = cmd + 1;
                FILE *tmp = fmemopen(move_str, strlen(move_str), "r");
                if (tmp) {
                    Move m = read_move_from_pipe(tmp, bp);
                    fclose(tmp);

                    fprintf(stderr, "DEBUG: Engine: read opponent move: 0x%x\n", m);
                    if (m != 0) {
                        fprintf(stderr, "DEBUG: Engine: before applying opponent move - move_number=%d, player_to_move=%d\n",
                                move_number(bp), player_to_move(bp));
                        /* Apply move to board */
                        apply(bp, m);
                        setclock(player_to_move(bp) == X ? O : X);
                        /* Update search board copy */
                        copybd(bp, search_bp);
                        fprintf(stderr, "DEBUG: Engine: after applying opponent move - move_number=%d, player_to_move=%d\n",
                                move_number(bp), player_to_move(bp));

                        /* If this matches our principal variation, keep it */
                        if (best_depth >= 1 && principal_var[0] == m && best_depth > 1) {
                            fprintf(stderr, "DEBUG: Engine: opponent move matches PV, keeping it\n");
                            /* Shift principal variation */
                            for (int i = 0; i < best_depth - 1; i++) {
                                principal_var[i] = principal_var[i + 1];
                            }
                            current_depth = best_depth - 1;
                            best_depth = current_depth;
                        } else {
                            fprintf(stderr, "DEBUG: Engine: opponent move doesn't match PV, resetting search\n");
                            /* Reset search */
                            current_depth = 1;
                            best_depth = 0;
                        }
                    }
                }

                /* Send acknowledgement */
                fprintf(stderr, "DEBUG: Engine: sending 'ok' acknowledgement\n");
                printf("ok\n");
                fflush(stdout);
                
                /* Now we can search on opponent's time */
                searching_on_opponent_time = 1;
                fprintf(stderr, "DEBUG: Engine: can now search on opponent's time\n");
            } else {
                fprintf(stderr, "DEBUG: Engine: unknown command: '%c'\n", cmd[0]);
            }
        }
    }
}
