/**
 * === DO NOT MODIFY THIS FILE ===
 * If you need some other prototypes or constants in a header, please put them
 * in another header file.
 *
 * === DO NOT MODIFY THIS FILE ===
 */

#include <stdio.h>

/* Constants used by the engine and static evaluator. */
#define MAXPLY 10			  // Maximum search depth in ply
#define MAXEVAL 100000                    // "Infinity" value supplied to alpha-beta */

/* Type representing a player in the game. */
typedef unsigned int Player;
#define X 0  // Value for first player
#define O 1  // Value for second player

/* Type representing a move (opaque format). */
typedef unsigned int Move;

/* The game board (opaque structure). */
typedef struct board Board;

/* Global variables used by the engine. */
extern int randomized;                    // If non-zero, then randomize play
extern int depth;                         // Current search depth limit in ply
extern int verbose;                       // If non-zero, then print search info to stderr
extern Move principal_var[];              // Current principal variation [0, depth-1]

/* Global variables used for statistics and time control. */
extern int searchtime;                    // Time (seconds since epoch) last search was begun
extern int movetime;                      // Time (seconds since epoch) last move was made
extern int xtime;                         // Total time (seconds) used by X
extern int otime;                         // Total time (seconds) used by O
extern int avgtime;                       // Average time (seconds) allowed per move
extern int times[];	                  // Search time estimates (indexed by depth)

/* Accessors for moves. */
int row_from(Move m);			  // Get row number of position move is from
int row_to(Move m);			  // Get row number of position move is to
int col_from(Move m);			  // Get column number of position move is from
int col_to(Move m);			  // Get column number of position move is to

/* Inputting moves. */

/**
 * Read a move from the specified input stream.
 * The move is checked for syntax and legality for the current board.
 * This function is used for communicating moves between processes.
 * It does not tolerate errors and will abort if it detects any.
 *
 * @param str  The stream from which to read the move.
 * @param bp  The current game board.
 * @return  The move that was read, or 0 if EOF was seen.
 * If any error occurs, or the move is ill-formed or illegal for the
 * current board, an abort will occur.
 */
Move read_move_from_pipe(FILE *str, Board *bp);

/**
 * Read a move interactively, using stdin/stdout.
 * This function is intended to be used for reading a move from a human user.
 * A prompt is printed to stdout, and the move is read fomr stdin.
 * The move is checked for syntax and legality for the current board.
 * If an error occurs, a message is printed and the input is re-tried.
 *
 * @param bp  The current game board.
 * @return  The move that was read, or 0 if EOF was seen.
 * If any error occurs, or the move is ill-formed or illegal for the
 * current board, the function does not return, but rather issues an error
 * message and a new prompt, and tries again to input a move.
 */
Move read_move_interactive(Board *bp);

/**
 * Check whether a move is legal for a specified board.
 *
 * @param m  The move to be checked.
 * @param bd  The board state against which the move is to be checked.
 * @return  1 if the move is legal, 0 if not.
 */
int legal_move(Move m, Board *bd);

/* Printing things. */

/**
 * Print a representation of a move to a specified output stream.
 *
 * @param bp  The board to which the move applies.  This is used to identify
 * intermediate hops between the starting and ending positions, which are
 * not represented directly as part of a Move value.
 * @param m  The move to be printed.
 * @param s  The output stream to which the move is to be printed.
 */
void print_move(Board *bp, Move m, FILE *s);

/**
 * Print a representation of a game board to a specified output stream.
 *
 * @param bp  The board to be printed.
 * @param s  The output stream to which the board is to be printed.
 */
void print_bd(Board *bp, FILE *s);

/**
 * Print a representation of the current principal variation to stderr.
 * The moves in the principal variation are printed in a space-separated
 * format, in increasing order of their depth in ply, until the specified
 * depth is reached.  The sequence of moves is followed by the evaluation of
 * the final position, as determined by the static evaluator.
 * Note that the principal variation itself is not a parameter, but is
 * contained in the global array "principal_var".
 *
 * @param bp  The current board at the start of the principal variation.
 * @param d  The depth in ply of the principal variation.
 */
void print_pvar(Board *bp, int d);

/* Operations on the game board. */

/**
 * Allocate a new game board object, in the state corresponding to the start
 * of a game.
 *
 * @return a pointer to the board that was allocated.
 */
Board *newbd();

/**
 * Copy the contents of one game board object to another, overwriting the
 * state of the destination board.  This function is used when it is required
 * to save the current board state, e.g. for later backtracking.
 *
 * @param obp  The board object to be copied.
 * @param bp  The destination for the copy.
 */
Board *copybd(Board *obp, Board *bp);

/**
 * Apply a move to a board.  It is assumed that the move is legal -- if it
 * is not, the results will be indeterminate.
 *
 * @param bp  The board to which the move is to be applied.
 * @param m  The move to apply.
 * The state of the board is modified.
 */
void apply(Board *bp, Move m);

/**
 * Get the number of the currently pending move in the game history for a specified board.
 * Moves are numbered starting from 0, so that calling this function on a pristine
 * board will return the value 0.  Application of a move to the board using apply
 * causes the move number to be incremented by one.
 *
 * @param bp  The board from which the move number is to be obtained.
 * @return  The number of the currently pending move.
 */
int move_number(Board *bp);

/**
 * Get the player whose turn it is to move for a specfied board.
 * When called on a pristine board, player X will be returned.
 * Players alternate with each successive application of a move to the board.
 *
 * @param bd  The board to be examined.
 * @return  The player whose turn it is to move for the specified board.
 */
Player player_to_move(Board *bp);

/**
 * Determine whether a specified board represents a game that has ended.
 *
 * @param bp  The board to be examined.
 * @return 1 if the first player (X) has won, -1 if the second player (O) has won,
 * or 0 if the game has not yet ended.
 */
int game_over(Board *bp);

/**
 * Search a game tree to a specified depth to determine the principal variation,
 * including the best move.  The algorithm used is minimax with alpha/beta pruning.
 *
 * @param bp  The starting board position for the search.
 * @param p  The player whose turn it is to move in the specified board position.
 * @param d  The current depth in ply.  A top-level call should pass d = 0;
 * the value increases as bestmove calls itself recursively.
 * @param alpha  The alpha cutoff threshold (in range [-MAXEVAL, MAXEVAL]).
 * @param beta  The beta cutoff threshold (in range [-MAXEVAL, MAXEVAL]).
 * @return  The score produced by the static evaluator for the position at the
 * end of the principal variation.
 *
 * The game tree is searched to a depth cutoff (in ply) given by the "depth"
 * global variable, using the static evaluator on nodes reached at the
 * cutoff depth and the minimax algorithm to determine the best move at each ply.
 * The alpha and beta values are used as part of a pruning heuristic.
 * The top-level caller of this function would typically pass alpha = -MAXEVAL
 * and beta = MAXEVAL.  As the recursive search deepens, the alpha/beta window
 * becomes narrower, and variations that result in evaluations outside of the
 * window are pruned.  As the search is completed at a particular level, the best
 * move found for that level is recorded in the corresponding element of the
 * "principal_var" array, so that once the top-level call returns, the moves of
 * the principal variation will be found in the entries 0..d-1 of this array.
 * In particular, if d was at least 1, then principal_var[0] will contain the
 * best move found for the specified initial board position.
 */
int bestmove(Board *bp, Player p, int d, Move *pvar, int alpha, int beta);

/* Statistics management. */

/**
 * Function to be called to reset search statistics just prior to a call to
 * bestmove.  Note that, once a call to bestmove has completed, this function
 * should not be called until the "timings" function has been used to record
 * statistics from the just-completed search.
 */
void reset_stats();

/**
 * Print the current values of some search statistics to stderr.
 */
void print_stats();

/**
 * Update timing estimates for search depth d (in ply)
 * This function should be called just after a call to bestmove has completed.
 * The value passed should be the value of the "depth" depth cutoff for which
 * the search has just completed.  The purpose of this call is to incorporate
 * the timing measurements for the search that has just completed into the
 * "times" array for use in forming later estimates of how long a search to
 * that same depth is likely to take.
 *
 * @param d  The depth for which the timing estimates are to be updated.
 * This modifies the "times" global array at indices d (measured) and d+1
 * (predicted from estimate at index d), using an exponential averaging scheme
 * to combine old estimates with new measurements and predictions based on
 * those measurements.
 */
void timings(int d);

/**
 * Charge the specified player for time elapsed since the last time this
 * function was called.  It is OK to call this function multiple times for
 * the same player during a single turn; each call will result in charging
 * that player for the increment of time spent since the last call.
 *
 * @param p  The player to be charged, which should be the player from whom
 * a move has been received but not yet applied to the board.
 */
void setclock(Player p);

/* FUNCTIONS TO BE IMPLEMENTED BY STUDENTS BELOW THIS POINT. */

/**
 * Main function.
 * This function is called from main() to start the game.  It is in a
 * separate file from main in order to satisfy requirements for using
 * Criterion to do some testing.
 *
 * @param argc  The argument count.
 * @param argv  The argument vector.
 */
int ccheck(int argc, char *argv[]);

/**
 * Function run by an "engine" process responsible for calculating moves to be
 * made by the computer player.  This function is intended to be called from
 * a process separate from the main game process.  It spends its time searching
 * the game tree to determine the best move it can in the current position.
 * It communicates with the main game process via a pipe and signalling protocol
 * that is described in more detail in the assignment document.
 *
 * @param abp  Board object with the starting game position.  Note that this is
 * not necessarily the initial position for a new game; for example, it could
 * be the result of reading a partial game history from a file and applying
 * the specified sequence of moves.
 */
void engine(Board *bp);
