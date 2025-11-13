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
	 if (bp == NULL) {
		 fprintf(stderr, "ERROR: Engine: board pointer is NULL!\n");
		 abort();
	 }
	 setup_engine_signals();

	 setbuf(stdin, NULL);
	 setbuf(stdout, NULL);
	 setbuf(stderr, NULL);

	 /* Create a working copy of the board for searching */
	 Board *search_bp = newbd();
	 copybd(bp, search_bp);

	 int current_depth = 1;
	 int best_depth = 0;
	 int searching_on_opponent_time = 0;
	 int first_wait = 1;  /* Flag to check stdin on first wait */
	 while (1) {
		 /* Wait for SIGHUP to read command from main process */
		 if (!sighup_received) {
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
			 sighup_received = 0;
			 searching_on_opponent_time = 0;

			 char cmd[256];
			 if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
				 if (feof(stdin)) {
					 break; /* EOF - pipe closed */
				 } else if (ferror(stdin)) {
					 break; /* Error reading */
				 }
				 break; /* EOF or error */
			 }

			 if (cmd[0] == '<') {
				 /* Main process wants a move - it's our turn */
				 struct itimerval timer;
				 int time_limit = 0;
				 int max_depth = MAXPLY;
				 struct timeval move_start_time;
				 int use_delay = 0;

				 /* Record start time for this move */
				 if (avgtime > 0) {
					 gettimeofday(&move_start_time, NULL);
					 use_delay = 1;
				 }

				 /* Calculate available time and set limits */
				 if (avgtime > 0) {
					 int moves_made = move_number(bp);
					 Player current_player = player_to_move(bp);
					 int total_time_used = (current_player == X) ? xtime : otime;
					 int time_remaining = (avgtime * (moves_made + 1)) - total_time_used;
					 if (time_remaining > 0) {
						 time_limit = time_remaining;
					 }
				 } else {
					 /* If no time limit, cap depth to prevent excessive search time */
					 max_depth = 6; /* Reasonable default depth */
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
				 if (current_depth > 1 && best_depth == 0) {
					 current_depth = 1;
				 }
				 
				 for (depth = current_depth; depth <= max_depth; depth++) {
					 if (sighup_received) {
						 break;
					 }

					 /* Check if we have time for this depth */
					 if (avgtime > 0 && depth > 1 && times[depth] > 0) {
						 int moves_made = move_number(bp);
						 Player current_player = player_to_move(bp);
						 int total_time_used = (current_player == X) ? xtime : otime;
						 int time_remaining = (avgtime * (moves_made + 1)) - total_time_used;
						 /* Add some safety margin - stop if we'd use more than 80% of remaining time */
						 if (time_remaining < (times[depth] * 10 / 8)) {
							 break; /* Not enough time */
						 }
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

					 if (sighup_received || sigalrm_received) {
						 break;
					 }
				 }

				 /* Cancel alarm */
				 if (time_limit > 0) {
					 timer.it_value.tv_sec = 0;
					 timer.it_value.tv_usec = 0;
					 timer.it_interval.tv_sec = 0;
					 timer.it_interval.tv_usec = 0;
					 setitimer(ITIMER_REAL, &timer, NULL);
				 }

				 /* Add delay to ensure move takes approximately avgtime seconds */
				 /* Do this BEFORE printing the move so the delay is visible */
				 if (use_delay) {
					 struct timeval move_end_time;
					 gettimeofday(&move_end_time, NULL);
					 
					 /* Calculate elapsed time in seconds (with microsecond precision) */
					 long elapsed_sec = move_end_time.tv_sec - move_start_time.tv_sec;
					 long elapsed_usec = move_end_time.tv_usec - move_start_time.tv_usec;
					 
					 /* Handle negative microseconds (borrow from seconds) */
					 if (elapsed_usec < 0) {
						 elapsed_sec--;
						 elapsed_usec += 1000000;
					 }
					 
					 double elapsed_time = elapsed_sec + (elapsed_usec / 1000000.0);
					 double delay_needed = avgtime - elapsed_time;
					 
					 if (delay_needed > 0.001) {  /* Only delay if more than 1ms needed */
						 /* Sleep for the remaining time */
						 unsigned int sleep_sec = (unsigned int)delay_needed;
						 unsigned int sleep_usec = (unsigned int)((delay_needed - sleep_sec) * 1000000);
						 if (sleep_sec > 0) {
							 sleep(sleep_sec);
						 }
						 if (sleep_usec > 0) {
							 usleep(sleep_usec);
						 }
					 }
				 }

				 /* Send best move if we have one */
				 if (best_depth >= 1) {
					 Move m = principal_var[0];
					 /* Print move BEFORE applying it (print_move needs pre-move board state) */
					 print_move(bp, m, stdout);
					 printf("\n");
					 fflush(stdout);

					 /* Apply move to our board AFTER printing */
					 apply(bp, m);
					 setclock(player_to_move(bp) == X ? O : X);
					 /* Update search board copy */
					 copybd(bp, search_bp);

					 /* Reset search depth */
					 current_depth = 1;
					 best_depth = 0;
				 } else {
					 /* This shouldn't happen, but if it does, search to depth 1 */
					 depth = 1;
					 reset_stats();
					 {
						 time_t t;
						 time(&t);
						 searchtime = (int)t;
					 }
					 copybd(bp, search_bp);
					 bestmove(search_bp, player_to_move(search_bp), 0, principal_var, -MAXEVAL, MAXEVAL);
					 timings(1);
					 best_depth = 1;
					 
					 Move m = principal_var[0];
					 print_move(bp, m, stdout);
					 printf("\n");
					 fflush(stdout);
					 apply(bp, m);
					 setclock(player_to_move(bp) == X ? O : X);
					 copybd(bp, search_bp);
					 current_depth = 1;
					 best_depth = 0;
				 }
			 } else if (cmd[0] == '>') {
				 /* Main process sending opponent's move */
				 char *move_str = cmd + 1;
				 FILE *tmp = fmemopen(move_str, strlen(move_str), "r");
				 if (tmp) {
					 Move m = read_move_from_pipe(tmp, bp);
					 fclose(tmp);

					 if (m != 0) {
						 /* Apply move to board */
						 apply(bp, m);
						 setclock(player_to_move(bp) == X ? O : X);
						 /* Update search board copy */
						 copybd(bp, search_bp);

						 /* If this matches our principal variation, keep it */
						 if (best_depth >= 1 && principal_var[0] == m && best_depth > 1) {
							 /* Shift principal variation */
							 for (int i = 0; i < best_depth - 1; i++) {
								 principal_var[i] = principal_var[i + 1];
							 }
							 current_depth = best_depth - 1;
							 best_depth = current_depth;
						 } else {
							 /* Reset search */
							 current_depth = 1;
							 best_depth = 0;
						 }
					 }
				 }

				 /* Send acknowledgement */
				 printf("ok\n");
				 fflush(stdout);
				 
				 /* Now we can search on opponent's time */
				 searching_on_opponent_time = 1;
			 }
		 }
	 }
 }
 