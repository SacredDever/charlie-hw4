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
#if 0

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>

#include "ccheck.h"
#include "debug.h"

int verbose;                     /* If non-zero, then print search info */

void engine(Board *bp)
{
    // If you remove the #if 0 above, then you have to provide an implementation of
    // this function.
    abort();

#ifdef THIS_SKETCH_WONT_COMPILE

    // NOTE: The following rough skeleton is not intended to function as-is.
    // Rather, it is an abstraction from the demo version of the engine
    // which has been provided to give you some idea of a workable control structure
    // and to show you some functions that the underlying search code expects you to call.

    // Initialize signal handling, targets for non-local jumps, etc.

    // Search loop.  Iteratively deepen search until either MAXPLY has been
    // reached or we are required to produce a move.  Note that this loop
    // may be executed both when it is our turn to move (i.e. "on our time")
    // and also  when it is the opponent's turn to move (i.e. "on the opponent's time").
    for(depth = 1; depth <= MAXPLY; depth++) {
	// If SIGHUP has been received, read what the main process has sent
	// and arrange to either accept a move or generate a move.

	// Search game tree at the current depth.
	if(verbose) {
	    fprintf(stderr, "Searching depth %d...", depth);
	    fflush(stderr);
	}

	// Function to compute a principal variation using minimax algorithm
	// with alpha/beta pruning.

	// The time taken in this function will grow exponentially with the
	// current depth cutoff (global variable "depth").  If it is our turn to move,
	// then we should make sure that we have a way of escaping from this function
	// in case it does not complete within the amount of time we have available
	// to produce our move.  Escaping can be accomplished upon receipt of
	// a signal by using siglongjmp to jump out of the signal handler to
	// a point previously marked with sigsetjmp.  One source of signals
	// will be the SIGHUP that the main process will send us when it is
	// our turn to move.  Another possiblity is to arrange for SIGALRM
	// to be received, e.g. using setitimer(2).

	int score = bestmove(bp, player_to_move(board), 0, principal_var, -MAXEVAL, MAXEVAL);

	// Update timing estimates.
	timings(depth);

	// Print information about the search, if verbose has been selected.
	if(verbose) {
	    print_stats();
	    print_pvar(bp, 0);
	    fprintf(stderr, "\n");
	}

	// Don't try to continue to search a won or lost position.
	if(score == -(MAXEVAL-1) || score == MAXEVAL-1)
	    break;
    }
    if(verbose) {
	fprintf(stderr, "Done with search");
	fflush(stderr);
    }

    // We've searched either to a won or lost position, or else to MAXPLY,
    // so there isn't anything else to do right now.

    // Wait to receive SIGHUP, then either accept a move or send the best move found.
    // If we have completed a search of the game tree to at least 1-ply, then the
    // best move will be the first move in the principal variation that was computed.
    // After applying the move, restart the search using the new board position.

    // Note that whenever a move is made, either by us or the opponent, if the move
    // matches the first move in the principal variation that we pre-computed, then
    // the rest of the principal variation will still be valid (for a depth of 1-ply less),
    // so we can keep that variation and continue searching from that depth, rather
    // than having to restart at depth 1.

#endif

}

#endif
