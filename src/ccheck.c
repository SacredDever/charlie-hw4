#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <string.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "ccheck.h"
#include "debug.h"

/*
 * Options (see the assignment document for details):
 *   -w           play white
 *   -b           play black
 *   -r           randomized play
 *   -v           give info about search
 *   -d           don't try to use X window system display
 *   -t           tournament mode
 *   -a <num>     set average time per move (in seconds)
 *   -i <file>    initialize from saved game score
 *   -o <file>    specify transcript file name
 */

/* Global variables for signal handling */
static volatile sig_atomic_t sigint_received = 0;
static volatile sig_atomic_t sighup_received = 0;
static volatile sig_atomic_t sigpipe_received = 0;
static volatile sig_atomic_t sigterm_received = 0;
static volatile sig_atomic_t sigchld_received = 0;

/* Process IDs */
static pid_t display_pid = 0;
static pid_t engine_pid = 0;

/* File descriptors for pipes */
static int display_to_main[2] = {-1, -1};  /* [0] = read, [1] = write */
static int main_to_display[2] = {-1, -1};
static int engine_to_main[2] = {-1, -1};
static int main_to_engine[2] = {-1, -1};

/* FILE streams for pipes */
static FILE *display_in = NULL;
static FILE *display_out = NULL;
static FILE *engine_in = NULL;
static FILE *engine_out = NULL;

/* Other state */
static FILE *transcript_file = NULL;
static int use_display = 1;
static int tournament_mode = 0;
static int play_white = 0;
static int play_black = 0;

/* Signal handler */
static void signal_handler(int sig)
{
    switch (sig) {
        case SIGINT:
            sigint_received = 1;
            break;
        case SIGHUP:
            sighup_received = 1;
            break;
        case SIGPIPE:
            sigpipe_received = 1;
            break;
        case SIGTERM:
            sigterm_received = 1;
            break;
        case SIGCHLD:
            sigchld_received = 1;
            break;
    }
}

/* Cleanup function to kill child processes */
static void cleanup_children(void)
{
    if (display_pid > 0) {
        kill(display_pid, SIGTERM);
        waitpid(display_pid, NULL, 0);
        display_pid = 0;
    }
    if (engine_pid > 0) {
        kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
        engine_pid = 0;
    }
}

/* Setup signal handlers */
static void setup_signals(void)
{
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction SIGINT");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        perror("sigaction SIGHUP");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        perror("sigaction SIGPIPE");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        perror("sigaction SIGTERM");
        exit(EXIT_FAILURE);
    }
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction SIGCHLD");
        exit(EXIT_FAILURE);
    }
}

/* Start display process */
static int start_display(void)
{
    if (pipe(display_to_main) < 0 || pipe(main_to_display) < 0) {
        perror("pipe");
        return -1;
    }

    display_pid = fork();
    if (display_pid < 0) {
        perror("fork");
        return -1;
    }

    if (display_pid == 0) {
        /* Child process */
        close(display_to_main[0]);
        close(main_to_display[1]);

        if (dup2(main_to_display[0], STDIN_FILENO) < 0 ||
            dup2(display_to_main[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }

        close(main_to_display[0]);
        close(display_to_main[1]);

        execlp("util/xdisp", "xdisp", (char *)NULL);
        perror("execlp xdisp");
        _exit(EXIT_FAILURE);
    }

    /* Parent process */
    close(display_to_main[1]);
    close(main_to_display[0]);

    display_in = fdopen(display_to_main[0], "r");
    display_out = fdopen(main_to_display[1], "w");

    if (!display_in || !display_out) {
        perror("fdopen");
        cleanup_children();
        return -1;
    }

    setbuf(display_out, NULL);
    setbuf(display_in, NULL);

    /* Wait for display to be ready */
    char line[256];
    if (fgets(line, sizeof(line), display_in) == NULL) {
        fprintf(stderr, "Failed to read from display process\n");
        cleanup_children();
        return -1;
    }

    return 0;
}

/* Start engine process */
static int start_engine(Board *bp)
{
    fprintf(stderr, "DEBUG: start_engine: creating pipes\n");
    if (pipe(engine_to_main) < 0 || pipe(main_to_engine) < 0) {
        perror("pipe");
        return -1;
    }

    fprintf(stderr, "DEBUG: start_engine: forking\n");
    engine_pid = fork();
    if (engine_pid < 0) {
        perror("fork");
        return -1;
    }

    if (engine_pid == 0) {
        /* Child process */
        fprintf(stderr, "DEBUG: Engine child process starting\n");
        close(engine_to_main[0]);
        close(main_to_engine[1]);

        if (dup2(main_to_engine[0], STDIN_FILENO) < 0 ||
            dup2(engine_to_main[1], STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(EXIT_FAILURE);
        }

        close(main_to_engine[0]);
        close(engine_to_main[1]);

        fprintf(stderr, "DEBUG: Engine child calling engine() function\n");
        engine(bp);
        fprintf(stderr, "DEBUG: Engine child exiting\n");
        _exit(EXIT_SUCCESS);
    }

    /* Parent process */
    fprintf(stderr, "DEBUG: start_engine: parent process, engine_pid=%d\n", engine_pid);
    close(engine_to_main[1]);
    close(main_to_engine[0]);

    engine_in = fdopen(engine_to_main[0], "r");
    engine_out = fdopen(main_to_engine[1], "w");

    if (!engine_in || !engine_out) {
        perror("fdopen");
        cleanup_children();
        return -1;
    }

    setbuf(engine_out, NULL);
    setbuf(engine_in, NULL);

    fprintf(stderr, "DEBUG: start_engine: engine process started successfully\n");
    return 0;
}

/* Send move to display and wait for acknowledgement */
static int send_move_to_display(Board *bp, Move m)
{
    fprintf(stderr, "DEBUG: send_move_to_display: starting, move=0x%x\n", m);
    if (!display_out) {
        fprintf(stderr, "DEBUG: send_move_to_display: display_out is NULL\n");
        return -1;
    }

    fprintf(stderr, "DEBUG: send_move_to_display: sending move to display (pid %d)\n", display_pid);
    fprintf(display_out, ">");
    print_move(bp, m, display_out);
    fprintf(display_out, "\n");
    fflush(display_out);

    fprintf(stderr, "DEBUG: send_move_to_display: sending SIGHUP to display\n");
    if (kill(display_pid, SIGHUP) < 0) {
        perror("kill display SIGHUP");
        fprintf(stderr, "DEBUG: send_move_to_display: failed to send SIGHUP\n");
        return -1;
    }

    /* Wait for acknowledgement */
    fprintf(stderr, "DEBUG: send_move_to_display: waiting for acknowledgement\n");
    char line[256];
    if (fgets(line, sizeof(line), display_in) == NULL) {
        fprintf(stderr, "DEBUG: send_move_to_display: failed to read acknowledgement (display may have crashed)\n");
        return -1;
    }
    fprintf(stderr, "DEBUG: send_move_to_display: received acknowledgement: '%s'\n", line);

    return 0;
}

/* Request move from display */
static Move get_move_from_display(Board *bp)
{
    fprintf(stderr, "DEBUG: get_move_from_display: starting\n");
    if (!display_out) {
        fprintf(stderr, "DEBUG: get_move_from_display: display_out is NULL\n");
        return 0;
    }

    fprintf(stderr, "DEBUG: get_move_from_display: sending '<' command to display (pid %d)\n", display_pid);
    fprintf(display_out, "<\n");
    fflush(display_out);

    fprintf(stderr, "DEBUG: get_move_from_display: sending SIGHUP to display\n");
    if (kill(display_pid, SIGHUP) < 0) {
        perror("kill display SIGHUP");
        return 0;
    }

    fprintf(stderr, "DEBUG: get_move_from_display: waiting for move from display\n");
    Move m = read_move_from_pipe(display_in, bp);
    fprintf(stderr, "DEBUG: get_move_from_display: received move (0x%x)\n", m);
    return m;
}

/* Send move to engine */
static int send_move_to_engine(Board *bp, Move m)
{
    if (!engine_out) return -1;

    /* Create a copy of the board for print_move (it needs pre-move state) */
    Board *temp_bp = newbd();
    copybd(bp, temp_bp);

    fprintf(engine_out, ">");
    print_move(temp_bp, m, engine_out);
    fprintf(engine_out, "\n");
    fflush(engine_out);

    /* Note: We can't easily free temp_bp, but it's just for printing */

    if (kill(engine_pid, SIGHUP) < 0) {
        perror("kill engine SIGHUP");
        return -1;
    }

    /* Wait for acknowledgement */
    char line[256];
    if (fgets(line, sizeof(line), engine_in) == NULL) {
        return -1;
    }

    return 0;
}

/* Request move from engine */
static Move get_move_from_engine(Board *bp)
{
    debug("get_move_from_engine: starting");
    if (!engine_out) {
        fprintf(stderr, "DEBUG: get_move_from_engine: engine_out is NULL\n");
        return 0;
    }

    fprintf(stderr, "DEBUG: get_move_from_engine: sending '<' command to engine (pid %d)\n", engine_pid);
    fprintf(engine_out, "<\n");
    fflush(engine_out);

    fprintf(stderr, "DEBUG: get_move_from_engine: sending SIGHUP to engine\n");
    if (kill(engine_pid, SIGHUP) < 0) {
        perror("kill engine SIGHUP");
        return 0;
    }

    fprintf(stderr, "DEBUG: get_move_from_engine: waiting for move from engine\n");
    fprintf(stderr, "DEBUG: get_move_from_engine: engine_in=%p, feof=%d, ferror=%d\n", 
            (void*)engine_in, feof(engine_in), ferror(engine_in));
    
    /* Peek at what's coming from the engine before reading */
    fprintf(stderr, "DEBUG: get_move_from_engine: about to call read_move_from_pipe\n");
    fprintf(stderr, "DEBUG: get_move_from_engine: current board state - move_number=%d, player_to_move=%d\n",
            move_number(bp), player_to_move(bp));
    
    /* Read move directly - read_move_from_pipe will block until data is available */
    Move m = read_move_from_pipe(engine_in, bp);
    fprintf(stderr, "DEBUG: get_move_from_engine: read_move_from_pipe returned (0x%x)\n", m);
    if (m == 0) {
        fprintf(stderr, "DEBUG: get_move_from_engine: move is 0, checking for errors\n");
        if (feof(engine_in)) {
            fprintf(stderr, "DEBUG: get_move_from_engine: EOF on engine_in\n");
        }
        if (ferror(engine_in)) {
            fprintf(stderr, "DEBUG: get_move_from_engine: error on engine_in\n");
        }
    }
    fprintf(stderr, "DEBUG: get_move_from_engine: received move (0x%x)\n", m);
    return m;
}

/* Read game history from file */
static int read_game_history(Board *bp, const char *filename)
{
    fprintf(stderr, "Initializing game from file %s\n", filename);
    FILE *f = fopen(filename, "r");
    if (!f) {
        perror("fopen history file");
        return -1;
    }

    char line[256];
    int move_count = 0;
    while (fgets(line, sizeof(line), f)) {
        /* Skip empty lines */
        if (line[0] == '\n' || line[0] == '\0') {
            fprintf(stderr, "DEBUG: read_game_history: skipping empty line\n");
            continue;
        }

        /* Parse line format: "N. white:MOVE" or "N. ... black:MOVE" */
        char *colon = strchr(line, ':');
        if (!colon) {
            fprintf(stderr, "DEBUG: read_game_history: skipping line without colon: %s", line);
            continue;
        }

        /* Determine which player this move is for based on the line format */
        /* Format is either "N. white:MOVE" or "N. ... black:MOVE" */
        Player move_player;
        if (strstr(line, "white:")) {
            move_player = X;  /* X is white */
        } else if (strstr(line, "black:")) {
            move_player = O;  /* O is black */
        } else {
            fprintf(stderr, "DEBUG: read_game_history: cannot determine player from line: %s", line);
            continue;
        }
        
        /* Extract move part (after colon) */
        char *move_str = colon + 1;
        move_count++;
        fprintf(stderr, "DEBUG: read_game_history: processing move %d, raw line: %s", move_count, line);
        
        /* Remove newline and carriage return if present */
        char *newline = strchr(move_str, '\n');
        if (newline) *newline = '\0';
        char *carriage = strchr(move_str, '\r');
        if (carriage) *carriage = '\0';
        
        /* Trim leading whitespace */
        while (*move_str == ' ' || *move_str == '\t') {
            move_str++;
        }
        
        /* Trim trailing whitespace */
        char *end = move_str + strlen(move_str) - 1;
        while (end > move_str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        
        /* Skip empty moves */
        if (*move_str == '\0') {
            fprintf(stderr, "DEBUG: read_game_history: skipping empty move string\n");
            continue;
        }

        /* Verify it's the correct player's turn */
        Player current_player = player_to_move(bp);
        if (current_player != move_player) {
            fprintf(stderr, "DEBUG: read_game_history: wrong player - expected %d, got %d for move '%s'\n", 
                    current_player, move_player, move_str);
            /* This might be okay if the file format is inconsistent, but log it */
        }

        /* Create a temporary file-like stream for the move */
        /* read_move_from_pipe expects a newline-terminated move, so we need to add one */
        /* Format should match what print_move outputs: "white:MOVE" or "black:MOVE" */
        char move_with_prefix[256];
        if (move_player == X) {
            snprintf(move_with_prefix, sizeof(move_with_prefix), "white:%s\n", move_str);
        } else {
            snprintf(move_with_prefix, sizeof(move_with_prefix), "black:%s\n", move_str);
        }
        
        FILE *tmp = fmemopen(move_with_prefix, strlen(move_with_prefix), "r");
        if (!tmp) {
            perror("fmemopen");
            fclose(f);
            return -1;
        }

        fprintf(stderr, "DEBUG: read_game_history: calling read_move_from_pipe with move '%s' (board state: move_num=%d, player=%d)\n", 
                move_with_prefix, move_number(bp), player_to_move(bp));
        Move m = read_move_from_pipe(tmp, bp);
        fclose(tmp);

        if (m == 0) {
            /* EOF or invalid move - stop reading */
            fprintf(stderr, "DEBUG: read_game_history: read_move_from_pipe returned 0 for move %d: '%s'\n", move_count, move_str);
            break;
        }
        
        fprintf(stderr, "DEBUG: read_game_history: successfully parsed move %d: '%s' -> 0x%x\n", move_count, move_str, m);

        /* Verify move is legal before proceeding */
        if (!legal_move(m, bp)) {
            fprintf(stderr, "Warning: Illegal move in history file: %s\n", move_str);
            break;
        }

        /* Determine player and move number BEFORE applying move */
        Player p = player_to_move(bp);
        int move_num_before = move_number(bp);

        /* Capture print_move output BEFORE applying move (print_move needs pre-move board state) */
        char move_buffer[256] = {0};
        FILE *capture_stream = fmemopen(move_buffer, sizeof(move_buffer) - 1, "w");
        if (capture_stream) {
            print_move(bp, m, capture_stream);
            fclose(capture_stream);
        }

        /* Strip player prefix (e.g., "black:" or "white:") if present */
        char *move_str_for_display = move_buffer;
        char *move_colon = strchr(move_buffer, ':');
        if (move_colon && (strncmp(move_buffer, "black:", 6) == 0 || strncmp(move_buffer, "white:", 6) == 0)) {
            move_str_for_display = move_colon + 1;  /* Skip past the colon */
        }

        /* Update display BEFORE applying move (print_move needs pre-move board state) */
        if (use_display && display_out) {
            /* Create a copy of the board for print_move (it needs pre-move state) */
            Board *temp_bp = newbd();
            copybd(bp, temp_bp);
            if (send_move_to_display(temp_bp, m) < 0) {
                fprintf(stderr, "Warning: Failed to update display with move %d, continuing anyway\n", move_count);
            }
            /* Note: We can't easily free temp_bp, but it's just for printing */
        }

        apply(bp, m);
        setclock(player_to_move(bp) == X ? O : X);

        /* Write to transcript if in use */
        if (transcript_file && move_str_for_display) {
            /* Write formatted move to transcript */
            /* Move numbers: move_number increments after each move, so we need to calculate the move pair number */
            /* For both white and black moves in the same pair, we use: (move_num_before / 2) + 1 */
            int transcript_move_num = (move_num_before / 2) + 1;
            if (p == X) {
                /* White move: format is "N. white:MOVE" */
                fprintf(transcript_file, "%d. white:", transcript_move_num);
            } else {
                /* Black move: format is "N. ... black:MOVE" where N is same as white's move number */
                fprintf(transcript_file, "%d. ... black:", transcript_move_num);
            }
            fprintf(transcript_file, "%s", move_str_for_display);  /* Write only the move notation, not the player prefix */
            fprintf(transcript_file, "\n");
            fflush(transcript_file);
        }
    }

    fclose(f);
    fprintf(stderr, "DEBUG: read_game_history: read %d moves total\n", move_count);
    return 0;
}

/* Global variable definition - verbose is not defined in the library */
int verbose;

int ccheck(int argc, char *argv[])
{
    int opt;
    char *init_file = NULL;
    char *output_file = NULL;
    int avg_time = 0;

    /* Initialize global variables */
    randomized = 0;
    verbose = 0;
    avgtime = 0;
    use_display = 1;
    tournament_mode = 0;
    play_white = 0;
    play_black = 0;

    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "wbrvdta:i:o:")) != -1) {
        switch (opt) {
            case 'w':
                play_white = 1;
                break;
            case 'b':
                play_black = 1;
                break;
            case 'r':
                randomized = 1;
                srand(time(NULL));  /* Seed random number generator for randomization */
                break;
            case 'v':
                verbose = 1;
                break;
            case 'd':
                use_display = 0;
                break;
            case 't':
                tournament_mode = 1;
                break;
            case 'a':
                avg_time = atoi(optarg);
                avgtime = avg_time;
                break;
            case 'i':
                init_file = optarg;
                break;
            case 'o':
                output_file = optarg;
                break;
            default:
                fprintf(stderr, "Usage: %s [-w] [-b] [-r] [-v] [-d] [-t] [-a time] [-i file] [-o file]\n", argv[0]);
                return EXIT_FAILURE;
        }
    }

    /* Setup signal handlers */
    setup_signals();

    /* Open transcript file if specified */
    if (output_file) {
        transcript_file = fopen(output_file, "w");
        if (!transcript_file) {
            perror("fopen transcript file");
            return EXIT_FAILURE;
        }
    }

    /* Create game board */
    Board *bp = newbd();
    if (!bp) {
        fprintf(stderr, "Failed to create game board\n");
        if (transcript_file) fclose(transcript_file);
        return EXIT_FAILURE;
    }

    /* Start display process if needed */
    if (use_display) {
        if (start_display() < 0) {
            fprintf(stderr, "Failed to start display process\n");
            if (transcript_file) fclose(transcript_file);
            return EXIT_FAILURE;
        }
    }

    /* Read game history if specified */
    if (init_file) {
        fprintf(stderr, "DEBUG: Reading game history from %s\n", init_file);
        if (read_game_history(bp, init_file) < 0) {
            fprintf(stderr, "Failed to read game history\n");
            cleanup_children();
            if (transcript_file) fclose(transcript_file);
            return EXIT_FAILURE;
        }
        fprintf(stderr, "DEBUG: Finished reading game history\n");
    }

    /* Start engine process if needed */
    if (play_white || play_black) {
        /* Create a copy of the board for the engine */
        Board *engine_board = newbd();
        copybd(bp, engine_board);
        if (start_engine(engine_board) < 0) {
            fprintf(stderr, "Failed to start engine process\n");
            cleanup_children();
            if (transcript_file) fclose(transcript_file);
            return EXIT_FAILURE;
        }
    }

    /* Main game loop */
    while (1) {
        /* Check for termination signals */
        if (sigint_received || sigterm_received) {
            break;
        }

        /* Check if child processes have terminated */
        if (sigchld_received) {
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                if (pid == display_pid) {
                    display_pid = 0;
                } else if (pid == engine_pid) {
                    engine_pid = 0;
                }
            }
            sigchld_received = 0;
            if (display_pid == 0 || engine_pid == 0) {
                break;
            }
        }

        /* Check if game is over */
        int game_result = game_over(bp);
        if (game_result != 0) {
            if (game_result > 0) {
                printf("White wins!\n");
            } else {
                printf("Black wins!\n");
            }
            break;
        }

        /* Determine whose turn it is */
        Player current_player = player_to_move(bp);
        int is_computer_turn = (current_player == X && play_white) || (current_player == O && play_black);

        fprintf(stderr, "DEBUG: Game loop - current_player=%d (X=%d, O=%d), play_white=%d, play_black=%d, is_computer_turn=%d\n",
                current_player, X, O, play_white, play_black, is_computer_turn);

        Move m = 0;

        if (is_computer_turn) {
            fprintf(stderr, "DEBUG: It's computer's turn, requesting move from engine\n");
            /* Get move from engine */
            m = get_move_from_engine(bp);
            if (m == 0) {
                fprintf(stderr, "Failed to get move from engine\n");
                break;
            }
            fprintf(stderr, "DEBUG: Received move from engine\n");

            /* Print move in tournament mode */
            if (tournament_mode) {
                printf("@@@");
            }
            print_move(bp, m, stdout);
            printf("\n");
            fflush(stdout);
        } else {
            fprintf(stderr, "DEBUG: It's user's turn, getting move\n");
            /* Get move from user */
            if (use_display && !tournament_mode) {
                m = get_move_from_display(bp);
            } else {
                m = read_move_interactive(bp);
            }

            if (m == 0) {
                fprintf(stderr, "DEBUG: User move is 0 (EOF or error)\n");
                /* EOF */
                break;
            }
            fprintf(stderr, "DEBUG: User move received: 0x%x\n", m);

            /* Send move to engine if it's playing */
            if ((play_white && current_player == O) || (play_black && current_player == X)) {
                fprintf(stderr, "DEBUG: Sending user move to engine\n");
                if (send_move_to_engine(bp, m) < 0) {
                    fprintf(stderr, "Failed to send move to engine\n");
                    break;
                }
            }
        }

        fprintf(stderr, "DEBUG: Applying move to board\n");
        
        /* Save move number and player before applying move (for transcript) */
        int move_num_before = move_number(bp);
        Player move_player = current_player;
        
        /* Update display BEFORE applying move (print_move needs pre-move board state) */
        /* In tournament mode, update display for both computer and user moves (user moves come from stdin, not display) */
        /* In non-tournament mode, only update display for computer moves (user moves from display already know about it) */
        if (use_display && display_out) {
            if (is_computer_turn || tournament_mode) {
                fprintf(stderr, "DEBUG: Updating display with move (before applying)\n");
                /* Create a copy of the board for print_move (it needs pre-move board state) */
                Board *temp_bp = newbd();
                copybd(bp, temp_bp);
                if (send_move_to_display(temp_bp, m) < 0) {
                    fprintf(stderr, "DEBUG: Failed to update display, but continuing\n");
                    /* Continue even if display update fails */
                }
                /* Note: We can't easily free temp_bp, but it's just for printing */
            } else {
                fprintf(stderr, "DEBUG: Skipping display update for user move (display already knows)\n");
            }
        }
        
        /* Write to transcript BEFORE applying move (print_move needs pre-move board state) */
        if (transcript_file) {
            /* Capture what print_move outputs and strip player prefix if present */
            char move_buffer[256] = {0};
            FILE *capture_stream = fmemopen(move_buffer, sizeof(move_buffer) - 1, "w");
            if (capture_stream) {
                print_move(bp, m, capture_stream);
                fclose(capture_stream);
            } else {
                /* Fallback: write directly if fmemopen fails */
                int transcript_move_num = (move_num_before / 2) + 1;
                if (move_player == X) {
                    fprintf(transcript_file, "%d. white:", transcript_move_num);
                } else {
                    fprintf(transcript_file, "%d. ... black:", transcript_move_num);
                }
                print_move(bp, m, transcript_file);
                fprintf(transcript_file, "\n");
                fflush(transcript_file);
                goto skip_transcript_write;
            }

            /* Strip player prefix (e.g., "black:" or "white:") if present */
            char *move_str = move_buffer;
            char *colon = strchr(move_buffer, ':');
            if (colon && (strncmp(move_buffer, "black:", 6) == 0 || strncmp(move_buffer, "white:", 6) == 0)) {
                move_str = colon + 1;  /* Skip past the colon */
            }

            /* Write formatted move to transcript */
            /* Move numbers: move_number increments after each move, so we need to calculate the move pair number */
            /* For both white and black moves in the same pair, we use: (move_num_before / 2) + 1 */
            int transcript_move_num = (move_num_before / 2) + 1;
            if (move_player == X) {
                /* White move: format is "N. white:MOVE" */
                fprintf(transcript_file, "%d. white:", transcript_move_num);
            } else {
                /* Black move: format is "N. ... black:MOVE" where N is same as white's move number */
                fprintf(transcript_file, "%d. ... black:", transcript_move_num);
            }
            fprintf(transcript_file, "%s", move_str);  /* Write only the move notation, not the player prefix */
            fprintf(transcript_file, "\n");
            fflush(transcript_file);
        }
        skip_transcript_write:;
        
        /* Apply move to board */
        apply(bp, m);
        setclock(current_player);

        if (!use_display) {
            print_bd(bp, stdout);
        }
    }

    /* Wait for termination signal or child process termination */
    while (!sigint_received && !sigterm_received) {
        if (sigchld_received) {
            int status;
            pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                if (pid == display_pid) {
                    display_pid = 0;
                } else if (pid == engine_pid) {
                    engine_pid = 0;
                }
            }
            sigchld_received = 0;
            if (display_pid == 0 && engine_pid == 0) {
                break;
            }
        }
        pause();
    }

    /* Cleanup */
    cleanup_children();

    if (display_in) fclose(display_in);
    if (display_out) fclose(display_out);
    if (engine_in) fclose(engine_in);
    if (engine_out) fclose(engine_out);
    if (transcript_file) fclose(transcript_file);

    return EXIT_SUCCESS;
}
