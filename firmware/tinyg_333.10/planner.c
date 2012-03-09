/*
 * planner.c - cartesian trajectory planning and motion execution
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2012 Alden S. Hart Jr.
 *
 * TinyG is free software: you can redistribute it and/or modify it 
 * under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, 
 * or (at your option) any later version.
 *
 * TinyG is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License 
 * for details. You should have received a copy of the GNU General Public 
 * License along with TinyG  If not, see <http://www.gnu.org/licenses/>.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* --- TinyG Notes ----
 *
 *	This layer works below the canonical machine.and above the motor
 *	mapping and queues. It is responsible only for cartesian motions.
 *	The calls to the routines are simple and do not need to know about
 *	the state of the gcode model. A rudimentary multitasking capability 
 *	is implemented for lines, arcs, dwells, and program control. 
 *
 *	Routines are coded as non-blocking continuations - which are simple 
 *	state machines that are re-entered multiple times until a particular 
 *	operation is complete.
 */

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>				// precursor for xio.h
#include <avr/pgmspace.h>		// precursor for xio.h
#include <string.h>

#include "tinyg.h"
#include "config.h"
#include "controller.h"
#include "gcode_parser.h"
#include "canonical_machine.h"
#include "arc.h"
#include "planner.h"
#include "kinematics.h"
#include "stepper.h"
#include "report.h"
#include "util.h"
#include "xio/xio.h"			// supports trap and debug statements

// All the enums that equal zero must be zero. Don't change this

enum mpBufferState {			// bf->buffer_state values 
	MP_BUFFER_EMPTY = 0,		// struct is available for use (MUST BE 0)
	MP_BUFFER_LOADING,			// being written ("checked out")
	MP_BUFFER_QUEUED,			// in queue
	MP_BUFFER_PENDING,			// marked as the next buffer to run
	MP_BUFFER_RUNNING			// current running buffer
};

struct mpBuffer {				// See Planning Velocity Notes for variable usage
	uint32_t linenum;			// line number; or block count if not numbered
	struct mpBuffer *pv;		// static pointer to previous buffer
	struct mpBuffer *nx;		// static pointer to next buffer
	uint8_t buffer_state;		// used to manage queueing/dequeueing
	uint8_t move_type;			// used to dispatch to run routine
	uint8_t move_code;			// M code or other indicator
	uint8_t move_state;			// move state machine sequence
	uint8_t replannable;		// TRUE if move can be replanned
	uint8_t hold_point;			// marks the first buffer after a feedhold

	double target[AXES];		// target position in floating point
	double unit[AXES];			// unit vector for axis scaling & planning

	double time;				// line, helix or dwell time in minutes
	double length;				// total length of line or helix in mm
	double head_length;
	double body_length;
	double tail_length;
								// *** SEE NOTES ON THESE VARIABLES, in aline() ***
	double entry_velocity;		// entry velocity requested for the move
	double cruise_velocity;		// cruise velocity requested & achieved
	double exit_velocity;		// exit velocity requested for the move

	double entry_vmax;			// max junction velocity at entry of this move
	double cruise_vmax;			// max cruise velocity requested for move
	double exit_vmax;			// max exit velocity possible (redundant)
	double delta_vmax;			// max velocity difference for this move
	double braking_velocity;	// current value for braking velocity

	double jerk;				// linear jerk term for this move
	double recip_jerk;			// 1/Jm				compute-once term
	double cubert_jerk;			// pow(Jm,(1/3))	compute-once term
};
typedef struct mpBuffer mpBuf;

struct mpBufferPool {			// ring buffer for sub-moves
	mpBuf *w;					// get_write_buffer pointer
	mpBuf *q;					// queue_write_buffer pointer
	mpBuf *r;					// get/end_run_buffer pointer
	mpBuf bf[PLANNER_BUFFER_POOL_SIZE];// buffer storage
};

struct mpMoveMasterSingleton {	// common variables for planning (move master)
	double position[AXES];		// final move position for planning purposes
#ifdef __UNIT_TEST_PLANNER
	double test_case;
	double test_velocity;
	double a_unit[AXES];
	double b_unit[AXES];
#endif
};

struct mpMoveRuntimeSingleton {	// persistent runtime variables
	uint32_t linenum;			// line/block number of BF being processed
	uint8_t (*run_move)(mpBuf *m); // currently running move
	uint8_t move_state;			// state of the overall move
	uint8_t section_state;		// state within a move section

	double endpoint[AXES];		// final target for entire move (used to correct rounding errors)
	double position[AXES];		// final move position for a given segment
	double target[AXES];		// target move position for a given segment
	double unit[AXES];			// unit vector for axis scaling & planning

	double head_length;			// copies of bf variables of same name
	double body_length;
	double tail_length;
	double entry_velocity;
	double cruise_velocity;
	double exit_velocity;

	double length;				// length of line or helix in mm
	double move_time;			// total running time (derived)
	double accel_time;			// total pseudo-time for acceleration calculation
	double elapsed_accel_time;	// current running time for accel calculation
	double midpoint_velocity;	// velocity at accel/decel midpoint
	double midpoint_acceleration;//acceleration at the midpoint
	double jerk;				// max linear jerk
	double jerk_div2;			// max linear jerk divided by 2

	double segments;			// number of segments in arc or blend
	uint32_t segment_count;		// count of running segments
	double segment_move_time;	// actual time increment per aline segment
	double segment_accel_time;	// time increment for accel computation purposes
	double microseconds;		// line or segment time in microseconds
	double segment_length;		// computed length for aline segment
	double segment_velocity;	// computed velocity for aline segment
};

static struct mpBufferPool mb;			// move buffer queue
static struct mpMoveMasterSingleton mm;	// static context for planning
static struct mpMoveRuntimeSingleton mr;// static context for runtime

/*
 * Local Scope Data and Functions
 */
#define _bump(a) ((a<PLANNER_BUFFER_POOL_SIZE-1)?(a+1):0) // buffer incr & wrap

// planning buffer routines
static void _init_buffers(void);
static void _unget_write_buffer(void);
static void _clear_buffer(mpBuf *bf); 
static void _copy_buffer(mpBuf *bf, const mpBuf *bp);
static void _queue_write_buffer(const uint8_t move_type);
static void _free_run_buffer(void);
static mpBuf * _get_write_buffer(void); 
static mpBuf * _get_run_buffer(void);
static mpBuf * _get_first_buffer(void);
static mpBuf * _get_last_buffer(void);

//static mpBuf * _get_prev_buffer(const mpBuf *bf);
//static mpBuf * _get_next_buffer(const mpBuf *bf);
#define _get_prev_buffer(b) ((mpBuf *)(b->pv))
#define _get_next_buffer(b) ((mpBuf *)(b->nx))

// aline planner routines / feedhold planning
static void _plan_block_list(mpBuf *bf);
static void _calculate_trapezoid(mpBuf *bf);
static void _calculate_trapezoid_finalize(mpBuf *bf);
static double _get_target_length(const double Vi, const double Vt, const mpBuf *bf);
static double _get_target_velocity(const double Vi, const double L, const mpBuf *bf);
static double _get_junction_vmax(const double a_unit[], const double b_unit[]);
static double _get_junction_deviation(const double a_unit[], const double b_unit[]);
static void _reset_replannable_list(void);

// execute routines (NB: These are all called from the LO interrupt)
static uint8_t _exec_line(mpBuf *bf);
static uint8_t _exec_dwell(mpBuf *bf);
static uint8_t _exec_mcode(mpBuf *bf);
static uint8_t _exec_tool(mpBuf *bf);
static uint8_t _exec_spindle_speed(mpBuf *bf);
static uint8_t _exec_aline(mpBuf *bf);
static uint8_t _exec_aline_head(void);
static uint8_t _exec_aline_body(void);
static uint8_t _exec_aline_tail(void);
static uint8_t _exec_aline_segment(uint8_t correction_flag);

#ifdef __DEBUG
static uint8_t _get_buffer_index(mpBuf *bf); 
static void _dump_plan_buffer(mpBuf *bf);
#endif

/* 
 * mp_init()
 *
 * The memset does:
 *	- clears all values
 *	- sets buffer states to MP_EMPTY
 *	- sets other states to their zero values - which is typically OFF
 */

void mp_init()
{
	memset(&mr, 0, sizeof(mr));	// clear all values, pointers and status
	memset(&mm, 0, sizeof(mm));	// clear all values, pointers and status
	_init_buffers();
}

/* 
 * mp_isbusy() - return TRUE if motion control busy (i.e. robot is moving)
 *
 *	Use this function to sync to the queue. If you wait until it returns
 *	FALSE you know the queue is empty and the motors have stopped.
 */

uint8_t mp_isbusy()
{
	if ((st_isbusy() == true) || (mr.move_state > MOVE_STATE_NEW)) {
		return (true);
	}
	return (false);
}

/* 
 * mp_zero_segment_velocity() - correct velocity in last segment for reporting purposes
 */

void mp_zero_segment_velocity() { mr.segment_velocity = 0;}

/* 
 * mp_flush_planner() - flush all moves in the planner and all arcs
 *
 *	Does not affect the move currently running in mr.
 *	This function is designed to be called during a hold to reset the planner
 *	and is also useful for jogs and other console-driven commands
 */

void mp_flush_planner()
{
	ar_abort_arc();
	_init_buffers();
	cm.motion_state = MOTION_STOP;
}

/*
 * mp_set_plan_position() - sets planning position (for G92)
 * mp_get_plan_position() - returns planning position
 * mp_set_axis_position() - sets both planning and runtime positions (for G2/G3)
 *
 * mp_get_runtime_position() - returns current position of queried axis
 * mp_get_runtime_velocity() - returns current velocity (aggregate)
 * mp_get_runtime_linenum()	 - returns currently executing line number
 *
 * 	Keeping track of position is complicated by the fact that moves 
 *	require multiple reference frames. The scheme to keep this straight is:
 *
 *	 - mm.position	- start and end position for planning
 *	 - mr.position	- current position of runtime segment
 *	 - mr.target	- target position of runtime segment
 *	 - bf->target / mr.endpoint	- final target position of runtime segment
 *
 *	Note that the positions are set immediately when they are computed and 
 *	are not an accurate representation of the tool position. In reality 
 *	the motors will still be processing the action and the real tool 
 *	position is still close to the starting point.
 */

double *mp_get_plan_position(double position[])
{
	copy_axis_vector(position, mm.position);	
	return (position);
}

void mp_set_plan_position(const double position[])
{
	copy_axis_vector(mm.position, position);
}

void mp_set_axis_position(const double position[])
{
	copy_axis_vector(mm.position, position);
	copy_axis_vector(mr.position, position);
}

double mp_get_runtime_position(uint8_t axis) { return (mr.position[axis]);}
double mp_get_runtime_velocity(void) { return (mr.segment_velocity);}
double mp_get_runtime_linenum(void) { return (mr.linenum);}

/*************************************************************************/
/* mp_exec_move() - execute runtime functions to prep move for steppers
 *
 *	Dequeues the buffer queue and executes the move continuations.
 *	Manages run buffers and other details
 *	NB: This routine runs under the LO interrupt (and all subroutines, of course)
 */

uint8_t mp_exec_move() 
{
	mpBuf *bf;

	if ((bf = _get_run_buffer()) == NULL) { return (TG_NOOP);}	// NULL means nothing's running
	if (cm.cycle_state == CYCLE_OFF) { cm_cycle_start();} 		// cycle state management

	if ((cm.motion_state == MOTION_STOP) && (bf->move_type == MOVE_TYPE_ALINE)) {
		cm.motion_state = MOTION_RUN;							// auto state-change
	}
	switch (bf->move_type) {									// dispatch the move
		case MOVE_TYPE_LINE: { return (_exec_line(bf));}
		case MOVE_TYPE_ALINE: { return (_exec_aline(bf));}
		case MOVE_TYPE_DWELL: { return (_exec_dwell(bf));}
		case MOVE_TYPE_MCODE: { return (_exec_mcode(bf));}
		case MOVE_TYPE_TOOL: { return (_exec_tool(bf));}
		case MOVE_TYPE_SPINDLE_SPEED: { return (_exec_spindle_speed(bf));}
	}
	return (TG_INTERNAL_ERROR);		// never supposed to get here
}

/**** M codes ************************************************************
 * mp_queue_program_stop() / _exec_stop() - entry point and execution
 * mp_queue_program_end()  / _exec_end()
 * mp_queue_Mcode() / _exec_Mcode()
 *
 *	M code execution must be synchronized with planning buffer execution. 
 *	These commands all work the same way:
 *	  - The M command is called by the Gcode interpreter (cm_<command>)
 *	  - cm_ function calls mp_queue_<command> which puts it in the planning queue
 *	  - the planning queue gets to the function and calls the _exec<command>
 *	  - ...which is typically a callback to the cm_exec<command> function.
 *
 *	Doing it this way instead of synchronizing on queue empty simplifes the
 *	handling of feedholds, feed overrides, buffer flushes, and thread blocking.
 */

void mp_queue_mcode(uint8_t mcode) 
{
	mpBuf *bf;

	if ((bf = _get_write_buffer()) == NULL) { return;}
	bf->move_code = mcode;
	_queue_write_buffer(MOVE_TYPE_MCODE);
}

static uint8_t _exec_mcode(mpBuf *bf)
{
	switch(bf->move_code) {
		case MCODE_PROGRAM_STOP: case MCODE_OPTIONAL_STOP: { cm_exec_program_stop(); break;}
		case MCODE_PROGRAM_END: { cm_exec_program_end(); break;}
		case MCODE_SPINDLE_CW: { cm_exec_spindle_control(SPINDLE_CW); break;}	
		case MCODE_SPINDLE_CCW: { cm_exec_spindle_control(SPINDLE_CCW); break;}
		case MCODE_SPINDLE_OFF: { cm_exec_spindle_control(SPINDLE_OFF); break;}
//		case MCODE_CHANGE_TOOL:	 // M6 - not yet
		case MCODE_MIST_COOLANT_ON:	{ cm_exec_mist_coolant_control(true); break;}
		case MCODE_FLOOD_COOLANT_ON: { cm_exec_flood_coolant_control(true); break;}
		case MCODE_FLOOD_COOLANT_OFF: { cm_exec_flood_coolant_control(false); break;}
		case MCODE_FEED_OVERRIDE_ON: { cm_exec_feed_override_enable(true); break;}
		case MCODE_FEED_OVERRIDE_OFF: { cm_exec_feed_override_enable(false); break;}
		default: { 
			_free_run_buffer();
			return (TG_INTERNAL_ERROR);
		}
	}
	// Must call a prep to keep the loader happy. See Move Execution in:
	// http://www.synthetos.com/wiki/index.php?title=Projects:TinyG-Module-Details#planner.c.2F.h
	st_prep_null();
	_free_run_buffer();
	return (TG_OK);
}

void mp_queue_tool(uint8_t tool) 
{
	mpBuf *bf;

	if ((bf = _get_write_buffer()) == NULL) { return;}
	bf->move_code = tool;
	_queue_write_buffer(MOVE_TYPE_TOOL);
}
static uint8_t _exec_tool(mpBuf *bf) { return (TG_OK);}

void mp_queue_spindle_speed(double speed) 
{
	mpBuf *bf;

	if ((bf = _get_write_buffer()) == NULL) { return;}
	_queue_write_buffer(MOVE_TYPE_SPINDLE_SPEED);
}
static uint8_t _exec_spindle_speed(mpBuf *bf) { return (TG_OK);}

/*************************************************************************
 * mp_dwell() 	 - queue a dwell
 * _exec_dwell() - dwell execution
 *
 * Dwells are performed by passing a dwell move to the stepper drivers.
 * When the stepper driver sees a dwell it times the dwell on a separate 
 * timer than the stepper pulse timer.
 */

uint8_t mp_dwell(double seconds) 
{
	mpBuf *bf; 

	if ((bf = _get_write_buffer()) == NULL) {	// get write buffer or fail
		return (TG_BUFFER_FULL_FATAL);		   // (not supposed to fail)
	}
	bf->time = seconds;						   // in seconds, not minutes
	_queue_write_buffer(MOVE_TYPE_DWELL);
	return (TG_OK);
}

static uint8_t _exec_dwell(mpBuf *bf)
{
	st_prep_dwell((uint32_t)(bf->time * 1000000));// convert seconds to uSec
	_free_run_buffer();
	return (TG_OK);
}

/*************************************************************************
 * mp_line() 	- queue a linear move (simple version - no accel/decel)
 * _exec_line() - run a line to generate and load a linear move
 *
 * Compute and queue a line segment to the move buffer.
 * Executes linear motion in absolute millimeter coordinates. 
 * Feed rate has already been converted to time (minutes).
 * Zero length lines are skipped at this level. 
 * 
 * The run_line routine is a continuation and can be called multiple times 
 * until it can successfully load the line into the move buffer.
 */

uint8_t mp_line(const double target[], const double minutes)
{
	mpBuf *bf;

	if (minutes < EPSILON) {
		return (TG_ZERO_LENGTH_MOVE);
	}
	if ((bf = _get_write_buffer()) == NULL) {	// get write buffer or fail
		return (TG_BUFFER_FULL_FATAL);			// (not supposed to fail)
	}
	bf->time = minutes;
	copy_axis_vector(bf->target, target);		// target to bf_target
	bf->length = get_axis_vector_length(target, mr.position);
	if (bf->length < EPSILON) {
		_unget_write_buffer();					// free buffer if early exit
		return (TG_ZERO_LENGTH_MOVE);
	}
	bf->cruise_vmax = bf->length / bf->time;	// for yuks
	_queue_write_buffer(MOVE_TYPE_LINE);
	copy_axis_vector(mm.position, bf->target);	// update planning position
	return(TG_OK);
}

static uint8_t _exec_line(mpBuf *bf) 
{
	uint8_t i;
	double travel[AXES];
	double steps[MOTORS];

	for (i=0; i < AXES; i++) {
		travel[i] = bf->target[i] - mr.position[i];
	}
	mr.microseconds = uSec(bf->time);
	(void)ik_kinematics(travel, steps, mr.microseconds);
	if (st_prep_line(steps, mr.microseconds) == TG_OK) {
		copy_axis_vector(mr.position, bf->target);	// update runtime position
	}
	_free_run_buffer();
	return (TG_OK);
}

/**************************************************************************
 * mp_aline() - plan a line with acceleration / deceleration
 *
 *	This function uses constant jerk motion equations to plan acceleration 
 *	and deceleration. The jerk is the rate of change of acceleration; it's
 *	the 1st derivative of acceleration, and the 3rd derivative of position. 
 *	Jerk is a measure of impact to the machine. Controlling jerk smoothes 
 *	transitions between moves and allows for faster feeds while controlling 
 *	machine oscillations and other undesirable side-effects.
 *
 *	A detailed explanation of how this module works can be found on the wiki:
 *  http://www.synthetos.com/wiki/index.php?title=Projects:TinyG-Developer-Info:#Acceleration_Planning
 *
 * 	Note: All math is done in absolute (machine) coordinates using AVRGCC 
 *	"double" floating point (which is actually only 32 bit single precision)
 */

uint8_t mp_aline(const double target[], const double minutes)
{
	mpBuf *bf; 						// current move pointer
	double exact_stop = 0;
	double junction_velocity;
	double length = get_axis_vector_length(target, mm.position);

	// trap error conditions
	if (minutes < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}
	if (length < EPSILON) { return (TG_ZERO_LENGTH_MOVE);}

	// get a cleared buffer (or die trying - never supposed to fail)
	if ((bf = _get_write_buffer()) == NULL) { return (TG_BUFFER_FULL_FATAL);}

	// setup move variables (these steps are in careful sequence)
	bf->linenum = cm_get_linenum();			// debugging convenience only
	bf->time = minutes;
	bf->length = length;
	copy_axis_vector(bf->target, target); 	// set target endpoint for _exec's
	set_unit_vector(bf->unit, bf->target, mm.position);
	bf->jerk = sqrt(square(bf->unit[X] * cfg.a[X].jerk_max) + // initialize jerk terms 
					square(bf->unit[Y] * cfg.a[Y].jerk_max) +
					square(bf->unit[Z] * cfg.a[Z].jerk_max) +
					square(bf->unit[A] * cfg.a[A].jerk_max) +
					square(bf->unit[B] * cfg.a[B].jerk_max) +
					square(bf->unit[C] * cfg.a[C].jerk_max));
	bf->recip_jerk = 1/bf->jerk;			  // compute-once term used in planning
	bf->cubert_jerk = pow(bf->jerk, 0.333333);// compute-once term used in planning

	// finish up the current block variables
	if (cm_get_path_control() != PATH_EXACT_STOP) { // exact stop cases already zeroed
		bf->replannable = true;
		exact_stop = 12345678;					// an arbitrarily large number
	}
	bf->cruise_vmax = bf->length / bf->time;	// target velocity requested
	junction_velocity = _get_junction_vmax(bf->pv->unit, bf->unit);
	bf->entry_vmax = min3(bf->cruise_vmax, junction_velocity, exact_stop);
	bf->delta_vmax = _get_target_velocity(0, bf->length, bf);
	bf->exit_vmax = min3(bf->cruise_vmax, (bf->entry_vmax + bf->delta_vmax), exact_stop);
	bf->braking_velocity = bf->delta_vmax;

	_plan_block_list(bf);	// replan the block list and commit the current block
	copy_axis_vector(mm.position, bf->target);	// update planning position
	_queue_write_buffer(MOVE_TYPE_ALINE);
	return (TG_OK);
}

/***** ALINE HELPERS *****
 * _plan_block_list()
 * _calculate_trapezoid()
 * _get_target_length()
 * _get_target_velocity()
 * _get_junction_vmax()
 * _get_junction_deviation()
 * _reset_replannable_list()
 */

/* _plan_block_list() - plans the entire block list
 *
 *	Plans all blocks between and including the first block and the block provided (bf).
 *	Sets entry, exit and cruise v's from vmax's then calls trapezoid generation. 
 *
 *	Variables that must be provided in the mpBuffers that will be processed:
 *
 *	  bf (function arg)		- end of block list (last block in time)
 *	  bf->replannable		- start of block list set by last FALSE value [Note 1]
 *	  bf->move_type			- Must be ALINE. Other mode types will fail.
 *								TODO: handle DWELL, START, STOP, END blocks in list
 *
 *	  bf->length			- provides block length
 *	  bf->entry_vmax		- used during forward planning to set entry velocity
 *	  bf->cruise_vmax		- used during forward planning to set cruise velocity
 *	  bf->exit_vmax			- used during forward planning to set exit velocity
 *	  bf->delta_vmax		- used during forward planning to set exit velocity
 *
 *	  bf->recip_jerk		- used during trapezoid generation
 *	  bf->cubert_jerk		- used during trapezoid generation
 *
 *	Variables that will be set during processing:
 *
 *	  bf->replannable		- set if the block becomes optimally planned
 *
 *	  bf->braking_velocity	- set during backward planning
 *	  bf->entry_velocity	- set during forward planning
 *	  bf->cruise_velocity	- set during forward planning
 *	  bf->exit_velocity		- set during forward planning
 *
 *	  bf->head_length		- set during trapezoid generation
 *	  bf->body_length		- set during trapezoid generation
 *	  bf->tail_length		- set during trapezoid generation
 *
 *	Variables that are ignored but here's what you would expect them to be:
 *	  bf->move_state		- NEW for all blocks but the earliest
 *	  bf->target[]			- block target position
 *	  bf->unit[]			- block unit vector
 *	  bf->time				- gets set later
 *	  bf->jerk				- source of the other jerk variables. Used in mr.
 */
/* Notes:
 *	[1]	Whether or not a block is planned is controlled by the bf->replannable 
 *		setting (set TRUE if it should be). Replan flags are checked during the 
 *		backwards pass and prune the replan list to include only the the latest 
 *		blocks that require planning.
 *
 *		In normal operation the first block (currently running block) is not 
 *		replanned, but may be for feedholds and feed overrides. In these cases 
 *		the prep routines modify the contents of the mr buffer and re-shuffle 
 *		the block list, re-enlisting the current bf buffer with new parameters.
 *		These routines also set all blocks in the list to be replannable so the 
 *		list can be recomputed regardless of exact stops and previous replanning 
 *		optimizations.
 */
void _plan_block_list(mpBuf *bf)
{
	mpBuf *bp = bf;

	// Backward planning pass. Find beginning of the list and update the braking velocities.
	// At the end *bp points to the first buffer before the list.
	while ((bp = _get_prev_buffer(bp)) != bf) {
		if (bp->replannable == false) { break; }
		bp->braking_velocity = min(bp->nx->entry_vmax, bp->nx->braking_velocity) + bp->delta_vmax;
	}

	// forward planning pass - recomputes trapezoids in the list.
	while ((bp = _get_next_buffer(bp)) != bf) {
		if (bp->pv == bf) {	
			bp->entry_velocity = bp->entry_vmax;		// first block in the list
		} else {
			bp->entry_velocity = bp->pv->exit_velocity;	// other blocks in the list
		}
		bp->cruise_velocity = bp->cruise_vmax;
		bp->exit_velocity = min4(bp->exit_vmax, bp->nx->braking_velocity, bp->nx->entry_vmax,
								(bp->entry_velocity + bp->delta_vmax));
		_calculate_trapezoid(bp);
		// test for optimally planned trapezoids - only need to check the exit
		if (bp->exit_velocity == bp->exit_vmax) { bp->replannable = false;}
	}
	// finish up the last block move
	bp->entry_velocity = bp->pv->exit_velocity;
	bp->cruise_velocity = bp->cruise_vmax;
	bp->exit_velocity = 0;
	_calculate_trapezoid(bp);
}

/*
 * _calculate_trapezoid() - calculate trapezoid parameters
 *
 *	This rather brute-force function sets section lengths and velocities based 
 *	on the line length and velocities requested. Requested velocities are specified
 *	as bf->entry_velocity (Ve), bf->cruise_velocity (Vt), and bf->exit_velocity (Vx). 
 *	Target length is specified using bf->length (L). 
 *	Note: The following conditions must be met on entry: Ve <= Vt >= Vx 
 *
 *	Modifies the buffer and returns accurate head_length, body_length and tail_length,
 *	and accurate or reasonably approximate velocities. We care about accuracy on lengths, 
 *	less so for velocity (as long as velocity err's on the side of too slow). 
 *	We need the velocities to be set even for zero-length sections so we can compute entry
 *	and exits for adjacent sections.
 *
 *	Various cases handled;
 *	  ZERO	zero sections - the line is too short to plan
 *	  HBT	Ve<Vt>Vx	3 section trapezoid
 *	  HB	Ve<Vt=Vx	head accelerates to cruise - exits at full speed
 *	  BT	Ve=Vt>Vx	enter at full speed and decelerate
 *	  HT	Ve & Vx		does not achieve cruise velocity
 *	  H		Ve<Vx		head accelerates to exit velocity (perfect fit)
 *	  H'	Ve<Vx'		line too short to meet Jm - Ve is met but Vx is degraded
 *	  B		Ve=Vt=Vx	entire line it run at Vt. No head or tail processed
 *	  T		Ve>Vx		tail decelerates to exit velocity (perfect fit)
 *	  T'	Ve'<Vx		line too short to meet Jm - Ve is degraded but Vx is met
 *
 *	The HT cases (2 section cases) deserve special attention There are 2 sub-cases: 
 *		- symmetric case where Ve = Vx,
 *		- asymmetic case where Ve != Vx. 
 *
 *	The order of the cases/tests in the code is pretty important
 */
static void _calculate_trapezoid(mpBuf *bf) 
{
	// initialize lengths
	bf->head_length = 0;
	bf->body_length = 0;
	bf->tail_length = 0;

	// ZERO case - the line is too short to plan
	if (bf->length < EPSILON) {
		bf->length = 0;
		return;
	}

	// B case - only has a body because all velocities are equal
	if (((bf->cruise_velocity - bf->entry_velocity) < PLANNER_VELOCITY_TOLERANCE) && 
		((bf->cruise_velocity - bf->exit_velocity) < PLANNER_VELOCITY_TOLERANCE)) {
		bf->body_length = bf->length;
		return;
	}

	// HBT case - trapezoid has a cruise region
	if ((bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf)) < bf->length) { 
		 bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
		 bf->body_length = bf->length - bf->head_length - bf->tail_length;
		if (bf->body_length > EPSILON) {
			_calculate_trapezoid_finalize(bf);
			return;
		}
	}

	// HT symmetric case - Ve=Vx. Vt is set accordingly. 
	// Velocity tolerance allows fitting around FP rounding errors
	if (fabs(bf->entry_velocity - bf->exit_velocity) < PLANNER_VELOCITY_TOLERANCE) {
		bf->body_length = 0;
		bf->head_length = bf->length/2;
		bf->tail_length = bf->head_length;
		bf->cruise_velocity = _get_target_velocity(bf->entry_velocity, bf->head_length, bf);
		return;
	}

	// H' and T' degraded cases - these only happen if line is too short to fit the required accel/decel
	double minimum_length = _get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
	if (bf->length < (minimum_length - PLANNER_LENGTH_TOLERANCE)) {
		if (bf->entry_velocity < bf->exit_velocity)	{ // degrade entry velocity to meet line constraints
			bf->head_length = bf->length;
			bf->tail_length = 0;
			bf->exit_velocity = _get_target_velocity(bf->entry_velocity, bf->length, bf);
		} else {							// degrade exit velocity to meet line constraints
			bf->head_length = 0;
			bf->tail_length = bf->length;
			bf->entry_velocity = _get_target_velocity(bf->exit_velocity, bf->length, bf);
		}
		bf->body_length = 0;
//		fprintf_P(stderr, PSTR("Degraded line in _calculate_trapezoid() %f  %f  %f  %f"), 
//			bf->length, bf->entry_velocity, bf->cruise_velocity, bf->exit_velocity);
		return;
	}

	// H, T, HB and BT cases - Vt=Vx, or close enough. Vt is set to match exit velocity.
	if (bf->length < (minimum_length * PLANNER_LENGTH_FACTOR)) {
		if (bf->entry_velocity < bf->exit_velocity)	{ // make an acceleration section (head)
			bf->cruise_velocity = bf->exit_velocity;
			bf->head_length = _get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
			bf->body_length = bf->length - bf->head_length;
			bf->tail_length = 0;
		} else {							// make a deceleration section (tail)
			bf->cruise_velocity = bf->entry_velocity;
			bf->tail_length = _get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
			bf->body_length = bf->length - bf->tail_length;
			bf->head_length = 0;
		}
		_calculate_trapezoid_finalize(bf);
		return;
	}

	// HT asymmetric case - this is relatively expensive but it's not called very often
	double computed_velocity = bf->cruise_vmax;
	do {
		bf->cruise_velocity = computed_velocity;	// initialize from previous iteration 
		bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
		bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
		if (bf->head_length > bf->tail_length) {
			bf->head_length = (bf->head_length / (bf->head_length + bf->tail_length)) * bf->length;
			computed_velocity = _get_target_velocity(bf->entry_velocity, bf->head_length, bf);
		} else {
			bf->tail_length = (bf->tail_length / (bf->head_length + bf->tail_length)) * bf->length;
			computed_velocity = _get_target_velocity(bf->exit_velocity, bf->tail_length, bf);
		}
//		if (++i > PLANNER_ITERATION_MAX) { 
//			fprintf_P(stderr, PSTR("_calculate_trapezoid() failed to converge")); 
//		}
	} while ((fabs(bf->cruise_velocity - computed_velocity) / computed_velocity) > PLANNER_ITERATION_ERROR_PERCENT);
	bf->cruise_velocity = computed_velocity;
	bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
	bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
	bf->body_length = 0;
	_calculate_trapezoid_finalize(bf);
	return;
}

/*	Handle various edge cases where sectoins are less than min line lengths. 
 * 	Also ensure that lengths are accurate, as this is what we care about most.
 */
static void _calculate_trapezoid_finalize(mpBuf *bf)
{
	if (bf->head_length < MIN_LINE_LENGTH) {
		bf->head_length = 0;
		bf->body_length = bf->length - bf->tail_length;
	}
	if (bf->body_length < MIN_LINE_LENGTH) {
		bf->body_length = 0;
		bf->tail_length = bf->length - bf->head_length;
	}
	if (bf->tail_length < MIN_LINE_LENGTH) {
		bf->tail_length = 0;
		if (bf->head_length > bf->body_length) {
			bf->head_length = bf->length - bf->body_length;	
		} else {
			bf->body_length = bf->length - bf->head_length;	
		}
	}
}

/*	
 * _get_target_length()
 * _get_target_velocity()
 *
 *	This pair of functions returns the fourth thing knowing the other three.
 *	
 *	_get_target_length() is a convenient function for determining the 
 *	optimal_length (L) of a line given the inital velocity (Vi), 
 *	target velocity (Vt) and maximum jerk (Jm).
 *
 *	The length (distance) equation is derived from: 
 *
 *	 a)	L = (Vt-Vi) * T - (Ar*T^2)/2	... which becomes b) with substitutions for Ar and T
 *	 b) L = (Vt-Vi) * 2*sqrt((Vt-Vi)/Jm) - (2*sqrt((Vt-Vi)/Jm) * (Vt-Vi))/2
 *	 c)	L = (Vt-Vi)^(3/2) / sqrt(Jm)	...is an alternate form of b) (see Wolfram Alpha)
 *	 c')L = (Vt-Vi) * sqrt((Vt-Vi)/Jm) ... second alternate form; requires Vt >= Vi
 *
 *	 Notes: Ar = (Jm*T)/4					Ar is ramp acceleration
 *			T  = 2*sqrt((Vt-Vi)/Jm)			T is time
 *			Assumes Vt, Vi and L are positive or zero
 *			Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *			  necessitating the introduction of fabs()
 *
 * 	_get_target_velocity() is a convenient function for determining Vt target 
 *	velocity for a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Equation d) is b) solved for Vt. Equation e) is c) solved for Vt. Use e) (obviously)
 *
 *	 d)	Vt = (sqrt(L)*(L/sqrt(1/Jm))^(1/6)+(1/Jm)^(1/4)*Vi)/(1/Jm)^(1/4)
 *	 e)	Vt = L^(2/3) * Jm^(1/3) + Vi
 */
static double _get_target_length(const double Vi, const double Vt, const mpBuf *bf)
{
	return (fabs(Vi-Vt) * sqrt(fabs(Vi-Vt) * bf->recip_jerk));
}

static double _get_target_velocity(const double Vi, const double L, const mpBuf *bf)
{
	return (pow(L, 0.66666666) * bf->cubert_jerk + Vi);
}

/*
 * _get_junction_vmax() - Sonny Jeon's cornerting algorithm extended for 
 *						  multi-axis and axis compensation on junction delta
 *
 *  Computes the maximum allowable junction speed by finding the velocity
 *	that will yield the centripetal acceleration in the corner_acceleration 
 *	value. The value of delta sets the effective radius of curvature.
 *	Here's Sonny's explanation of what's going on:
 *
 *	http://onehossshay.wordpress.com/2011/09/24/improving_grbl_cornering_algorithm/
 *
 *	"First let's assume that at a junction we only look a centripetal 
 *	acceleration to simply things. At a junction of two lines, let's place 
 *	a circle such that both lines are tangent to the circle. The circular 
 *	segment joining the lines represents the path for constant centripetal 
 *	acceleration. This creates a deviation from the path (let's call this 
 *	delta), which is the distance from the junction to the edge of the 
 *	circular segment. Delta needs to be defined, so let's replace the 
 *	term max_jerk with max_junction_deviation( or delta). This indirectly 
 *	sets the radius of the circle, and hence limits the velocity by the 
 *	centripetal acceleration. Think of the this as widening the race track.
 *	If a race car is driving on a track only as wide as a car, it'll have 
 *	to slow down a lot to turn corners. If we widen the track a bit, the 
 *	car can start to use the track to go into the turn. The wider it is, 
 *	the faster through the corner it can go.
 *
 *	If you do the geometry in terms of the known variables, you get:
 *	sin(theta/2) = R/(R+delta)  Re-arranging in terms of circle radius (R)
 *	R = delta*sin(theta/2)/(1-sin(theta/2). Theta is the angle between 
 *	line segments given by: cos(theta) = dot(a,b)/(norm(a)*norm(b)). 
 *	Most of these calculations are already done in the planner. To remove 
 *	the acos() and sin() computations, use the trig half angle identity: 
 *	sin(theta/2) = +/- sqrt((1-cos(theta))/2). For our applications, this 
 *	should always be positive. Now just plug and chug the equations into 
 *	the centripetal acceleration equation: v_c = sqrt(a_max*R). You'll see
 *	that there are only two sqrt computations and no sine/cosines."
 *
 *	How to compute the radius using brute-force trig:
 *	double theta = acos(costheta);
 *	double radius = delta * sin(theta/2)/(1-sin(theta/2));
 */

static double _get_junction_vmax(const double a_unit[], const double b_unit[])
{
	double costheta = - a_unit[X] * b_unit[X]
					  - a_unit[Y] * b_unit[Y]
					  - a_unit[Z] * b_unit[Z]
					  - a_unit[A] * b_unit[A]
					  - a_unit[B] * b_unit[B]
					  - a_unit[C] * b_unit[C];

	if (costheta < -0.99) { return (10000000); } 		// straight line cases
	if (costheta > 0.99)  { return (0); } 				// reversal cases
	double delta = _get_junction_deviation(a_unit, b_unit);// with axis compensation
//	double delta = cfg.a[X].junction_deviation;			// without axis compensation
	double sintheta_over2 = sqrt((1 - costheta)/2);
	double radius = delta * sintheta_over2 / (1-sintheta_over2);
	return(sqrt(radius * cfg.junction_acceleration));
}

/*	
 * _get_junction_deviation() - Compute junction delta for cornering
 *
 *  This helper function extends Sonny Jeon's algorithm by computing a value
 *	for delta that takes the contributions of the individual axes in the 
 *	move into account. It allows the radius of curvature to vary by axis.
 *	This is necessary to support axes that have different dynamics; such 
 *	as a Z axis that doesn't move as fast as X and Y (such as a screw driven 
 *	Z axis on machine with a belt driven XY - like a makerbot), or rotary 
 *	axes ABC that have completely different dynamics than their linear 
 *	counterparts.
 *
 *	The function takes the absolute values of the sum of the unit vector
 *	components as a measure of contribution to the move, then scales the 
 *	delta values from the non-zero axes into a composite delta to be used
 *	for the move. Shown for an XY vector:
 *
 *	 U[i]	Unit sum of i'th axis	fabs(unit_a[i]) + fabs(unit_b[i])
 *	 Usum	Length of sums			Ux + Uy
 *	 d		Delta of sums			(Dx*Ux+DY*UY)/Usum
 */

static double _get_junction_deviation(const double a_unit[], const double b_unit[])
{
	double a_delta = 0;
	double b_delta = 0;

	for (uint8_t i=0; i<AXES; i++) {
		a_delta += square(a_unit[i] * cfg.a[i].junction_dev);
		b_delta += square(b_unit[i] * cfg.a[i].junction_dev);
	}
	double d = (sqrt(a_delta) + sqrt(b_delta))/2;
	return (d);
}

/*
 *	_reset_replannable_list() - resets all blocks in the planning list to be replannable
 */	

void _reset_replannable_list()
{
	mpBuf *bf = _get_first_buffer();
	if (bf == NULL) { return;}
	mpBuf *bp = bf;
	do {
		bp->replannable = true;
	} while (((bp = _get_next_buffer(bp)) != bf) && (bp->move_state != MOVE_STATE_OFF));
}

/*************************************************************************
 * feedholds - functions for performing holds
 *
 * mp_plan_hold_callback() - replan block list to execute hold
 * mp_end_hold_callback() - remove the hold and restart block list
 *
 *	Feedhold is executed as cm.hold_state transitions executed as _exec_aline() 
 *	post-processing and main loop callbacks to mp_plan_hold() and mp_end_hold().
 */
/*	Holds work like this:
 * 
 * 	- Hold is asserted by calling cm_feedhold()
 *		If hold_state is OFF and motion_state is RUNing it sets 
 *		hold_state to SYNC and motion_state to HOLD.
 *
 *	- Hold state == SYNC tells the aline exec routine to execute the next aline 
 *		segment then set hold_state to PLAN. This gives the planner sufficient 
 *		time to replan the block list for the hold before the next aline 
 *		segment needs to be processed.
 *
 *	- Hold state == PLAN tells the planner to replan the mr buffer, the current
 *		run buffer (bf), and subsequent bf buffers as necessary to execute a 
 *		hold. Hold planning plans down to zero and then back up from zero. The 
 *		buffer that releases the hold (i.e. the buffer following the one that 
 *		decelerates to zero) is marked as the hold_point. Hold state is set to  
 *		DECEL when planning is complete.
 *
 *	- Hold state == DECEL persists until the aline execution gets to the hold
 *		point (decel to zero), at which point hold state transitions to HOLD
 *		and machine_state is set to HOLD.
 *
 *	- Hold state == HOLD persists until the cycle is restarted, which is when
 *		cm_cycle_start() is called and motion_state transitions to END_HOLD.
 *		Typically cycle start will be re-asserted after motion stops, but it 
 *		may have been re-asserted during any phase of feedhold execution.
 */

/*
 * mp_plan_hold_callback() - callback from main loop to replan for hold
 */

uint8_t mp_plan_hold_callback()
{
	mpBuf *bf; 	// first bf (the one associated with mr buffer)
	mpBuf *bp; 	// working pointer

	if (cm.hold_state != FEEDHOLD_PLAN) { return (TG_NOOP);}
	if ((bf = _get_run_buffer()) == NULL) { return (TG_NOOP);} // nothing's running
	bp = bf;

	// examine and process mr buffer
	double braking_velocity = mr.segment_velocity;			// velocity to shed
	double braking_length = _get_target_length(braking_velocity, 0, bp);
	double remaining_length = get_axis_vector_length(bf->target, mr.position);

	// Case 1: feedhold deceleration fits in remaining distance in mr buffer
	if (braking_length < remaining_length) {
		// replan mr to zero exit velocity
		mr.move_state = MOVE_STATE_TAIL;		
		mr.section_state = MOVE_STATE_NEW;
		mr.tail_length = braking_length;
		mr.cruise_velocity = braking_velocity;
		mr.exit_velocity = 0;

		// replan the current bf to be the feed release point
		bp->length = remaining_length - braking_length;
		bp->delta_vmax = _get_target_velocity(0, bp->length, bp); // recompute vmax
		bp->entry_vmax = 0;
		bp->move_state = MOVE_STATE_NEW;

	} else { // Case 2: feedhold deceleration exceeds remaining distance in mr buffer
		// replan mr to minimum (but non-zero) exit velocity
		mr.move_state = MOVE_STATE_TAIL;		
		mr.section_state = MOVE_STATE_NEW;
		mr.tail_length = remaining_length;
		mr.cruise_velocity = braking_velocity;
		mr.exit_velocity = braking_velocity - _get_target_velocity(0, remaining_length, bp);	

		// replan next buffer into current bf (while advancing bp)
		braking_velocity = mr.exit_velocity;

		do {
			_copy_buffer(bp, bp->nx);
			braking_length = _get_target_length(braking_velocity, 0, bp);
			remaining_length = bp->length - braking_length;
			bp->entry_vmax = braking_velocity;
			if (braking_length > bp->length) {	// feedhold decel does not fit in bp buffer
				bp->exit_vmax = braking_velocity - _get_target_velocity(0, bp->length, bp);	
				braking_velocity = bp->exit_vmax;
				bp = _get_next_buffer(bp);
			} else {	// feedhold deceleration fits in the bp buffer
				bp->length = braking_length;
				bp->exit_vmax = 0;
				bp = _get_next_buffer(bp);
				break;
			}
		} while (bp != bf);				// cutout if for some reason it wraps

		// setup the feed release point and replan the list
		bp->entry_vmax = 0;
		bp->length = remaining_length - braking_length;
		bp->delta_vmax = _get_target_velocity(0, bp->length, bp);
	}
	bp->hold_point = true;
	_reset_replannable_list();
	_plan_block_list(_get_last_buffer());
	cm.hold_state = FEEDHOLD_DECEL;		// set state to decelerate and exit
	return (TG_OK);
}

/* 
 * mp_end_hold_callback() - callback from main loop to end a feedhold
 *
 * 	This function is a callback that is called from the controller. To end a 
 *	hold do not call mp_end_feedhold() directly, instead call cm_cycle_start().
 */

uint8_t mp_end_hold_callback()
{
	mpBuf *bf;

	if (cm.hold_state != FEEDHOLD_END_HOLD) { return (TG_NOOP); }
	cm.hold_state = FEEDHOLD_OFF;
	if ((bf = _get_run_buffer()) == NULL) {	// NULL means nothing's running
		cm.motion_state = MOTION_STOP;
		return (TG_NOOP);
	}
	cm.motion_state = MOTION_RUN;
	bf->hold_point = false;					// allows the move to be executed
	st_request_exec_move();					// restart the steppers
	return (TG_OK);
}

/*************************************************************************/
/**** ALINE EXEC ROUTINE *************************************************/
/*************************************************************************
 * ---> Everything here runs in LO interrupt and must be interrupt safe
 *
 *  _exec_aline()			- acceleration line main routine
 *	_exec_aline_head()		- helper for acceleration section
 *	_exec_aline_body()		- helper for cruise section
 *	_exec_aline_tail()		- helper for deceleration section
 *	_exec_aline_segment()	- helper for running a segment
 *
 *	Returns:
 *	 TG_OK		move is done
 *	 TG_EAGAIN	move is not finished - has more segments to run
 *	 TG_NOOP	cause no operation from the steppers - do not load the move
 *	 TG_xxxxx	fatal error. Ends the move and frees the bf buffer
 *	
 *	This routine is called from the (LO) interrupt level. The interrupt 
 *	sequencing relies on the behaviors of the routines being exactly correct.
 *	Each call to _exec_aline() must execute and prep *one and only one* 
 *	segment. If the segment is the not the last segment in the bf buffer the 
 *	_aline() must return TG_EAGAIN. If it's the last segment it must return 
 *	TG_OK. If it encounters a fatal error that would terminate the move it 
 *	should return a valid error code. Failure to obey this will introduce 
 *	subtle and very difficult to diagnose bugs (trust me on this).
 *
 *	Note 1 Returning TG_OK ends the move and frees the bf buffer. 
 *		   Returning TG_OK at this point does NOT advance position meaning any
 *		   position error will be compensated by the next move.
 *
 *	Note 2 Solves a potential race condition where the current move ends but the 
 * 		   new move has not started because the previous move is still being run 
 *		   by the steppers. Planning can overwrite the new move.
 */
/* OPERATION:
 *	Aline generates jerk-controlled S-curves as per Ed Red's course notes:
 *	http://www.et.byu.edu/~ered/ME537/Notes/Ch5.pdf
 *	http://www.scribd.com/doc/63521608/Ed-Red-Ch5-537-Jerk-Equations
 *
 *	A full trapezoid is divided into 5 periods Periods 1 and 2 are the 
 *	first and second halves of the acceleration ramp (the concave and convex 
 *	parts of the S curve in the "head"). Periods 3 and 4 are the first 
 *	and second parts of the deceleration ramp (the tail). There is also 
 *	a period for the constant-velocity plateau of the trapezoid (the body).
 *	There are various degraded trapezoids possible, including 2 section 
 *	combinations (head and tail; head and body; body and tail), and single 
 *	sections - any one of the three.
 *
 *	The equations that govern the acceleration and deceleration ramps are:
 *	  Period 1	  V = Vi + Jm*(T^2)/2
 *	  Period 2	  V = Vh + As*T - Jm*(T^2)/2
 *	  Period 3	  V = Vi - Jm*(T^2)/2
 *	  Period 4	  V = Vh + As*T + Jm*(T^2)/2
 *
 * 	These routines play some games with the acceleration and move timing 
 *	to make sure this actually all works out.
 */
/* --- State transitions - hierarchical state machine ---
 *
 *	bf->move_state transitions:
 *	 from _NEW to _RUN on first call (sub_state set to _OFF)
 *	 from _RUN to _OFF on final call
 * 	 or just remains _OFF
 *
 *	mr.move_state transitions on first call from _OFF to one of _HEAD, _BODY, _TAIL
 *	Within each section state may be 
 *	 _NEW - trigger initialization
 *	 _RUN1 - run the first part
 *	 _RUN2 - run the second part 
 */
static uint8_t _exec_aline(mpBuf *bf)
{
	uint8_t status;

	if (bf->move_state == MOVE_STATE_OFF) { return (TG_NOOP);} 
	if (mr.move_state == MOVE_STATE_OFF) {
		if (bf->hold_point == true) { return (TG_NOOP);} // implements the feedhold

		// initialization to process the new incoming bf buffer
		bf->replannable = false;
		if (bf->length < EPSILON) { return (TG_NOOP);}
		bf->move_state = MOVE_STATE_RUN;
		mr.move_state = MOVE_STATE_HEAD;
		mr.section_state = MOVE_STATE_NEW;
		mr.linenum = bf->linenum;
		mr.jerk = bf->jerk;
		mr.jerk_div2 = bf->jerk/2;
		mr.head_length = bf->head_length;
		mr.body_length = bf->body_length;
		mr.tail_length = bf->tail_length;
		mr.entry_velocity = bf->entry_velocity;
		mr.cruise_velocity = bf->cruise_velocity;
		mr.exit_velocity = bf->exit_velocity;
		copy_axis_vector(mr.unit, bf->unit);
		copy_axis_vector(mr.endpoint, bf->target);	// save the final target of the move
	}
	// NB: from this point on the contents of the bf buffer do not affect execution

	//**** main dispatcher to process segments ***
	switch (mr.move_state) {
		case (MOVE_STATE_HEAD): { status = _exec_aline_head(); break;}
		case (MOVE_STATE_BODY): { status = _exec_aline_body(); break;}
		case (MOVE_STATE_TAIL): { status = _exec_aline_tail(); break;}
	}

	// feed hold post-processing
	if (cm.hold_state == FEEDHOLD_SYNC) { 
		cm.hold_state = FEEDHOLD_PLAN;
	}
	if ((cm.hold_state == FEEDHOLD_DECEL) && (status == TG_OK) && (bf->hold_point == true)) {
		cm.motion_state = MOTION_HOLD;
		cm.hold_state = FEEDHOLD_HOLD;			// we are now holding
		rpt_queue_status_report();				// queue a hold status report
	}

	// There are 3 things that can happen here depending on return conditions:
	//	  status	 bf->move_state	 Description
	//    ---------	 --------------	 ----------------------------------------
	//	  TG_EAGAIN	 <don't care>	 mr buffer has more segments to run
	//	  TG_OK		 MOVE_STATE_RUN	 mr and bf buffers are done
	//	  TG_OK		 MOVE_STATE_NEW	 mr done; bf must be run again (it's been reused)

	if (status == TG_EAGAIN) { 
		rpt_decr_status_report(); 				// continue running mr buffer
	} else {
		mr.move_state = MOVE_STATE_OFF;			// reset mr buffer
		mr.section_state = MOVE_STATE_OFF;
		bf->nx->replannable = false;			// prevent overplanning (Note 2)
		if (bf->move_state == MOVE_STATE_RUN) {
			_free_run_buffer();					// free bf if it's actually done
		}
	}
	return (status);
}

/*
 * _exec_aline_head()
 */
static uint8_t _exec_aline_head()
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.head_length < EPSILON) { 
			mr.move_state = MOVE_STATE_BODY;
			return(_exec_aline_body());			// skip ahead
		}
		mr.midpoint_velocity = (mr.entry_velocity + mr.cruise_velocity) / 2;
		mr.move_time = mr.head_length / mr.midpoint_velocity;	// time for entire accel
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.entry_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.entry_velocity) / mr.accel_time;
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec)); // number of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2; // elapsed time starting point (offset)
		mr.segment_count = (uint32_t)mr.segments;
		mr.microseconds = uSec(mr.segment_move_time);
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {
		mr.segment_velocity = mr.entry_velocity + (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) { 	  		// set up for second half
			mr.elapsed_accel_time = mr.segment_accel_time / 2;	// start time from midpoint of segment
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {
		mr.segment_velocity = mr.midpoint_velocity + (mr.elapsed_accel_time * mr.midpoint_acceleration) -
							 (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) {
			if ((mr.body_length < MIN_LINE_LENGTH) && 
				(mr.tail_length < MIN_LINE_LENGTH)) { return(TG_OK);}	// end the move
			mr.move_state = MOVE_STATE_BODY;
			mr.section_state = MOVE_STATE_NEW;
		}
	}
	return(TG_EAGAIN);
}

/*
 * _exec_aline_body() - break the body into teeny tiny little segments and run them
 */
static uint8_t _exec_aline_body() 
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.body_length < EPSILON) {
			mr.move_state = MOVE_STATE_TAIL;
			return(_exec_aline_tail());			// skip ahead
		}
		mr.move_time = mr.body_length / mr.cruise_velocity;
		mr.segments = ceil(uSec(mr.move_time) / cfg.estd_segment_usec);
		mr.segment_move_time = mr.move_time / mr.segments;
		mr.segment_velocity = mr.cruise_velocity;
		mr.segment_count = (uint32_t)mr.segments;
		mr.microseconds = uSec(mr.segment_move_time);
		mr.section_state = MOVE_STATE_RUN;
	}
	if (mr.section_state == MOVE_STATE_RUN) {
		if (_exec_aline_segment(false) == TG_COMPLETE) {
			if (mr.tail_length < MIN_LINE_LENGTH) { return(TG_OK);}	// end the move
			mr.move_state = MOVE_STATE_TAIL;
			mr.section_state = MOVE_STATE_NEW;
		}
	}
	return(TG_EAGAIN);
}

/*
 * _exec_aline_tail()
 */
static uint8_t _exec_aline_tail() 
{
	if (mr.section_state == MOVE_STATE_NEW) {
		if (mr.tail_length < EPSILON) { return(TG_OK);}		// end the move
		mr.midpoint_velocity = (mr.cruise_velocity + mr.exit_velocity) / 2;
		mr.move_time = mr.tail_length / mr.midpoint_velocity;
		mr.accel_time = 2 * sqrt((mr.cruise_velocity - mr.exit_velocity) / mr.jerk);
		mr.midpoint_acceleration = 2 * (mr.cruise_velocity - mr.exit_velocity) / mr.accel_time;
		mr.segments = ceil(uSec(mr.move_time) / (2 * cfg.estd_segment_usec));// number of segments in *each half*
		mr.segment_move_time = mr.move_time / (2 * mr.segments);// time to advance for each segment
		mr.segment_accel_time = mr.accel_time / (2 * mr.segments);// time to advance for each segment
		mr.elapsed_accel_time = mr.segment_accel_time / 2; //compute time from midpoint of segment
		mr.segment_count = (uint32_t)mr.segments;
		mr.microseconds = uSec(mr.segment_move_time);
		mr.section_state = MOVE_STATE_RUN1;
	}
	if (mr.section_state == MOVE_STATE_RUN1) {
		mr.segment_velocity = mr.cruise_velocity - (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(false) == TG_COMPLETE) { 	  		// set up for second half
			mr.elapsed_accel_time = mr.segment_accel_time / 2;	// start time from midpoint of segment
			mr.segment_count = (uint32_t)mr.segments;
			mr.section_state = MOVE_STATE_RUN2;
		}
		return(TG_EAGAIN);
	}
	if (mr.section_state == MOVE_STATE_RUN2) {
		mr.segment_velocity = mr.midpoint_velocity - 
							 (mr.elapsed_accel_time * mr.midpoint_acceleration) +
							 (square(mr.elapsed_accel_time) * mr.jerk_div2);
		if (_exec_aline_segment(true) == TG_COMPLETE) { return (TG_OK);}	// end the move
	}
	return(TG_EAGAIN);
}

/*
 * _exec_aline_segment() - segment runner helper
 */
static uint8_t _exec_aline_segment(uint8_t correction_flag)
{
	uint8_t i;
	double travel[AXES];
	double steps[MOTORS];

	// Multiply computed length by the unit vector to get the contribution for
	// each axis. Set the target in absolute coords and compute relative steps.
	for (i=0; i < AXES; i++) {	// don;t do the error correction if you are going into a hold
		if ((correction_flag == true) && (mr.segment_count == 1) && 
			(cm.motion_state == MOTION_RUN) && (cm.cycle_state == CYCLE_STARTED)) {
			mr.target[i] = mr.endpoint[i];	// rounding error correction for last segment
		} else {
			mr.target[i] = mr.position[i] + (mr.unit[i] * mr.segment_velocity * mr.segment_move_time);
		}
		travel[i] = mr.target[i] - mr.position[i];
	}
	// prep the segment for the steppers and adjust the variables for the next iteration
	(void)ik_kinematics(travel, steps, mr.microseconds);
	SEGMENT_LOGGER				// conditional DEBUG statement
	if (st_prep_line(steps, mr.microseconds) == TG_OK) {
		copy_axis_vector(mr.position, mr.target); 	// update runtime position	
	}
	mr.elapsed_accel_time += mr.segment_accel_time; // NB: ignored if running the body
	if (--mr.segment_count == 0) {
		return (TG_COMPLETE);	// this section has run all its segments
	}
	return (TG_EAGAIN);			// this section still has more segments to run
}


/**** PLANNER BUFFER HANDLING ROUTINES ****************************************
 *
 * mp_test_write_buffer()	Returns TRUE if a write buffer is available
 * _init_buffers()			Initializes or resets buffers
 * _get_write_buffer()		Get pointer to next available write buffer
 *							Returns pointer or NULL if no buffer available.
 *
 * _unget_write_buffer()	Free write buffer if you decide not to queue it.
 * _queue_write_buffer()	Commit the next write buffer to the queue
 *							Advances write pointer & changes buffer state
 *
 * _get_run_buffer()		Get pointer to the next or current run buffer
 *							Returns a new run buffer if prev buf was ENDed
 *							Returns same buf if called again before ENDing
 *							Returns NULL if no buffer available
 *							The behavior supports continuations (iteration)
 *
 * _finalize_run_buffer()	Release the run buffer & return to buffer pool.
 * _request_finalize_run_buffer() Request that a finalize be run before 
 *							the next planning pass. This allows the exec routine
 *							to free the buffer w/o stomping on the main loop.
 *							
 * mp_invoke_finalize_run_buffer() Execute the finalize request. This is how 
 *							the main loop completes a request. 
 *
 * _test_buffer_queue_empty() Returns TRUE if buffer queue is empty
 *
 * _get_prev_buffer(bf)		Returns pointer to prev buffer in linked list
 * _get_next_buffer(bf)		Returns pointer to next buffer in linked list 
 * _get_first_buffer(bf)	Returns pointer to first buffer, i.e. the running block
 * _get_last_buffer(bf)		Returns pointer to last buffer, i.e. last block (zero)
 * _clear_buffer(bf)		Zeroes the contents of the buffer
 * _copy_buffer(bf,bp)		Copies the contents of bp into bf - preserves links
 *
 * Notes:
 *	The write buffer pointer only moves forward on queue_write, and 
 *	the read buffer pointer only moves forward on finalize_read calls.
 *	(test, get and unget have no effect)
 */

uint8_t mp_test_write_buffer()
{
	if (mb.w->buffer_state == MP_BUFFER_EMPTY) { return (true); }
	return (false);
}

static void _init_buffers()
{
	mpBuf *pv;
	uint8_t i;

	memset(&mb, 0, sizeof(mb));			// clear all values, pointers and status
	mb.w = &mb.bf[0];					// init write and read buffer pointers
	mb.q = &mb.bf[0];
	mb.r = &mb.bf[0];
	pv = &mb.bf[PLANNER_BUFFER_POOL_SIZE-1];
	for (i=0; i < PLANNER_BUFFER_POOL_SIZE; i++) {  // setup ring pointers
		mb.bf[i].nx = &mb.bf[_bump(i)];
		mb.bf[i].pv = pv;
		pv = &mb.bf[i];
	}
}

static mpBuf * _get_write_buffer() 	// get & clear a buffer
{
	if (mb.w->buffer_state == MP_BUFFER_EMPTY) {
		mpBuf *w = mb.w;
		mpBuf *nx = mb.w->nx;					// save pointers
		mpBuf *pv = mb.w->pv;
		memset(mb.w, 0, sizeof(mpBuf));
		w->nx = nx;								// restore pointers
		w->pv = pv;
		w->buffer_state = MP_BUFFER_LOADING;
		mb.w = w->nx;
		return (w);
	}
	return (NULL);
}

static void _unget_write_buffer()
{
	mb.w = mb.w->pv;							// queued --> write
	mb.w->buffer_state = MP_BUFFER_EMPTY; 		// not loading anymore
}

static void _queue_write_buffer(const uint8_t move_type)
{
	mb.q->move_type = move_type;
	mb.q->move_state = MOVE_STATE_NEW;
	mb.q->buffer_state = MP_BUFFER_QUEUED;
	mb.q = mb.q->nx;							// advance the queued buffer pointer
	st_request_exec_move();						// request a move exec if not busy
}

static mpBuf * _get_run_buffer() 
{
	// condition: fresh buffer; becomes running if queued or pending
	if ((mb.r->buffer_state == MP_BUFFER_QUEUED) || 
		(mb.r->buffer_state == MP_BUFFER_PENDING)) {
		 mb.r->buffer_state = MP_BUFFER_RUNNING;
	}
	// condition: asking for the same run buffer for the Nth time
	if (mb.r->buffer_state == MP_BUFFER_RUNNING) {	// return same buffer
		return (mb.r);
	}
	return (NULL);								// condition: no queued buffers. fail it.
}

static void _free_run_buffer()					// EMPTY current run buf & adv to next
{
	_clear_buffer(mb.r);						// clear it out (& reset replannable)
	mb.r->buffer_state = MP_BUFFER_EMPTY;
	mb.r = mb.r->nx;							 // advance to next run buffer
	if (mb.r->buffer_state == MP_BUFFER_QUEUED) {// only if queued...
		mb.r->buffer_state = MP_BUFFER_PENDING;  // pend next buffer
	}
	if (mb.w == mb.r) { cm_cycle_end();}		// end the cycle if the queue empties
}

static mpBuf * _get_first_buffer(void)
{
	return(_get_run_buffer());	// returns buffer or NULL if nothing's running
}

static mpBuf * _get_last_buffer(void)
{
	mpBuf *bf = _get_run_buffer();
	mpBuf *bp = bf;

	if (bf == NULL) { return(NULL);}

	do {
		if ((bp->nx->move_state == MOVE_STATE_OFF) || (bp->nx == bf)) { 
			return (bp); 
		}
	} while ((bp = _get_next_buffer(bp)) != bf);
	return (bp);
}

// Use the macro instead
//static mpBuf * _get_prev_buffer(const mpBuf *bf) { return (bf->pv);}
//static mpBuf * _get_next_buffer(const mpBuf *bf) { return (bf->nx);}

static void _clear_buffer(mpBuf *bf) 
{
	mpBuf *nx = bf->nx;	// save pointers
	mpBuf *pv = bf->pv;
	memset(bf, 0, sizeof(mpBuf));
	bf->nx = nx;					// restore pointers
	bf->pv = pv;
}

static void _copy_buffer(mpBuf *bf, const mpBuf *bp)
{
	mpBuf *nx = bf->nx;	// save pointers
	mpBuf *pv = bf->pv;
 	memcpy(bf, bp, sizeof(mpBuf));
	bf->nx = nx;					// restore pointers
	bf->pv = pv;
}

#ifdef __DEBUG	// currently this routine is only used by debug routines
static uint8_t _get_buffer_index(mpBuf *bf) 
{
	mpBuf *b = bf;		// temp buffer pointer

	for (uint8_t i=0; i < PLANNER_BUFFER_POOL_SIZE; i++) {
		if (b->pv > b) {
			return (i);
		}
		b = b->pv;
	}
	return (PLANNER_BUFFER_POOL_SIZE);	// should never happen
}
#endif



//####################################################################################
//##### UNIT TESTS AND DEBUG CODE ####################################################
//####################################################################################

/****** DEBUG Code ******	(see beginning of file for static function prototypes) */

#ifdef __DEBUG
void mp_dump_running_plan_buffer() { _dump_plan_buffer(mb.r);}
void mp_dump_plan_buffer_by_index(uint8_t index) { _dump_plan_buffer(&mb.bf[index]);	}

static void _dump_plan_buffer(mpBuf *bf)
{
	fprintf_P(stderr, PSTR("***Runtime Buffer[%d] bstate:%d  mtype:%d  mstate:%d  replan:%d\n"),
			_get_buffer_index(bf),
			bf->buffer_state,
			bf->move_type,
			bf->move_state,
			bf->replannable);

	print_scalar(PSTR("line number:     "), bf->linenum);
	print_vector(PSTR("position:        "), mm.position, AXES);
	print_vector(PSTR("target:          "), bf->target, AXES);
	print_vector(PSTR("unit:            "), bf->unit, AXES);
	print_scalar(PSTR("jerk:            "), bf->jerk);
	print_scalar(PSTR("time:            "), bf->time);
	print_scalar(PSTR("length:          "), bf->length);
	print_scalar(PSTR("head_length:     "), bf->head_length);
	print_scalar(PSTR("body_length:     "), bf->body_length);
	print_scalar(PSTR("tail_length:     "), bf->tail_length);
	print_scalar(PSTR("entry_velocity:  "), bf->entry_velocity);
	print_scalar(PSTR("cruise_velocity: "), bf->cruise_velocity);
	print_scalar(PSTR("exit_velocity:   "), bf->exit_velocity);
	print_scalar(PSTR("exit_vmax:       "), bf->exit_vmax);
	print_scalar(PSTR("entry_vmax:      "), bf->entry_vmax);
	print_scalar(PSTR("cruise_vmax:     "), bf->cruise_vmax);
	print_scalar(PSTR("delta_vmax:      "), bf->delta_vmax);
	print_scalar(PSTR("braking_velocity:"), bf->braking_velocity);
}

void mp_dump_runtime_state(void)
{
	fprintf_P(stderr, PSTR("***Runtime Singleton (mr)\n"));
	print_scalar(PSTR("line number:       "), mr.linenum);
	print_vector(PSTR("position:          "), mr.position, AXES);
	print_vector(PSTR("target:            "), mr.target, AXES);
	print_scalar(PSTR("length:            "), mr.length);

	print_scalar(PSTR("move_time:         "), mr.move_time);
	print_scalar(PSTR("accel_time;        "), mr.accel_time);
	print_scalar(PSTR("elapsed_accel_time:"), mr.elapsed_accel_time);
	print_scalar(PSTR("midpoint_velocity: "), mr.midpoint_velocity);
	print_scalar(PSTR("midpoint_accel:    "), mr.midpoint_acceleration);
	print_scalar(PSTR("jerk_div2:         "), mr.jerk_div2);

	print_scalar(PSTR("segments:          "), mr.segments);
	print_scalar(PSTR("segment_count:     "), mr.segment_count);
	print_scalar(PSTR("segment_move_time: "), mr.segment_move_time);
	print_scalar(PSTR("segment_accel_time:"), mr.segment_accel_time);
	print_scalar(PSTR("microseconds:      "), mr.microseconds);
	print_scalar(PSTR("segment_length:	  "), mr.segment_length);
	print_scalar(PSTR("segment_velocity:  "), mr.segment_velocity);
}
#endif // __DEBUG

/****** UNIT TESTS ******/

#ifdef __UNIT_TEST_PLANNER

#define JERK_TEST_VALUE (double)50000000

void _test_calculate_trapezoid(void);
void _test_get_junction_vmax(void);
void _make_unit_vector(double unit[], double x, double y, double z, 
						  double a, double b, double c);
void _test_calculate_trapezoid(void);
void _test_trapezoid(double entry_velocity, double cruise_velocity, double exit_velocity, double length, mpBuf *bf);


void mp_unit_tests()
{
	_test_calculate_trapezoid();
//	_test_get_junction_vmax();
}

void _test_trapezoid(double entry_velocity, double cruise_velocity, double exit_velocity, double length, mpBuf *bf)
{
	bf->jerk = JERK_TEST_VALUE;
	bf->entry_vmax = entry_velocity;
	bf->cruise_vmax = cruise_velocity;
	bf->exit_vmax = exit_velocity;
	_calculate_trapezoid(bf);
}

void _test_calculate_trapezoid()
{
	mpBuf *bf = _get_write_buffer();

// these tests are calibrated the following parameters:
//	jerk_max 				50 000 000		(all axes)
//	jerk_corner_offset		   		 0.1	(all exes)
//	jerk_corner_acceleration   200 000		(global)

	_test_trapezoid(0,	400, 	400,	0.8, bf);

// test cases drawn from braid_600mm					 		// expected results
//				   	   Ve  		Vt		Vx			L
	_test_trapezoid(000.000,	600,	000.000,	0.327, bf); // Ve=0 	   	Vc=110.155
	_test_trapezoid(000.000,	600,	174.538,	0.327, bf); // Ve=0, 	   	Vc=174.744	Vx=174.537
	_test_trapezoid(174.873,	600,	173.867,	0.327, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(173.593,	600,	000.000,	0.327, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(347.082,	600,	173.214,	0.327, bf); // Ve=174.873	Vc=185.356	Vx=173.867


// ZERO section cases: line below minimum velocity or length
//				   	   Ve  	Vt		Vx		L
	_test_trapezoid(0,	0.001,	0,		1.0, bf);
	_test_trapezoid(0,	100,	0,		0.0, bf);

// 1 section cases: line shorter than minimum transition length cases
//				   	   Ve  	Vt		Vx		L
	_test_trapezoid(400,	400, 	0, 		0.8, bf);
	_test_trapezoid(600,	600, 	200,	0.8, bf);
	_test_trapezoid(0,	400, 	400,	0.8, bf);
	_test_trapezoid(200,	600, 	600,	0.8, bf);

// HBT - 3 section cases
//				   	   Ve  	Vt		Vx		L
	_test_trapezoid(0,	190, 	0, 		0.8, bf);
	_test_trapezoid(200,	400, 	0, 		2.0, bf);

// 2 section cases (HT)
//				   	   Ve  	Vt		Vx		L
	_test_trapezoid(0,	200, 	0, 		0.8, bf);
	_test_trapezoid(0,	400, 	0, 		0.8, bf);
	_test_trapezoid(200,	400, 	0, 		0.8, bf);
	_test_trapezoid(400,	400, 	0, 		2.0, bf);
	_test_trapezoid(0,	400, 	200,	0.8, bf);

// 1 section cases (H,B and T)
//				   	   Ve  	Vt		Vx		L
	_test_trapezoid(800,	800, 	800, 	1.0, bf);

	_test_trapezoid(0,	400, 	0, 		0.8, bf);
	_test_trapezoid(200,	400, 	0, 		0.8, bf);
	_test_trapezoid(400,	400, 	0, 		2.0, bf);
	_test_trapezoid(0,	400, 	200,	0.8, bf);
}


void _make_unit_vector(double unit[], double x, double y, double z, 
					      double a, double b, double c)
{
	double length = sqrt(x*x + y*y + z*z + a*a + b*b + c*c);
	unit[X] = x/length;
	unit[Y] = y/length;
	unit[Z] = z/length;
	unit[A] = a/length;
	unit[B] = b/length;
	unit[C] = c/length;
}

void _test_get_junction_vmax()
{
	// See "Chamnit's" tab in acceleration spreadsheet for description
	//	of the following cases. 

//	cfg.a[X].jerk_max = JERK_TEST_VALUE;
//	cfg.a[Y].jerk_max = JERK_TEST_VALUE;
//	cfg.a[Z].jerk_max = JERK_TEST_VALUE;
//	cfg.a[A].jerk_max = JERK_TEST_VALUE;
//	cfg.a[B].jerk_max = JERK_TEST_VALUE;
//	cfg.a[C].jerk_max = JERK_TEST_VALUE;
//	mm.jerk_transition_size = 0.5;
//	mm.jerk_limit_max = 184.2;

	mm.test_case = 1;				// straight line along X axis
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 2;				// angled straight line
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.7071,	0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 3;				// 5 degree bend
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.9962,	0.0872, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 4;				// 30 degrees
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.8660,	0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 5;				// 45 degrees
	_make_unit_vector(mm.a_unit, 0.8660,	0.5000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.2588,	0.9659, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 6;				// 60 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 7;				// 90 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.0000,	1.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 8;				// 90 degrees rotated 45 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071,	0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 9;				// 120 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 10;				// 150 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.8660,	0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 11;				// 180 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071,-0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);
}

#endif // __UNIT_TEST_PLANNER
