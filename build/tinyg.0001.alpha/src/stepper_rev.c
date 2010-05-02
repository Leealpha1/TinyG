/*
  stepper.c - stepper motor interface
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/
/* 
  TinyG Notes
  Modified to support Xmega family processors
  Modifications Copyright (c) 2010 Alden S. Hart, Jr.

  --- Line drawing, flow control and synchronization ---++++++++
  This code works differently than the Reprap or Grbl Bresenham implementations.
  Coordinated motion (line drawing) is performed by dedicating a timer to each axis
  and stepping each motor at a computed rate (timer period value) for a specified 
  number of pulses (counter value).Each timeout fires a high-priority interrupt 
  which generates a step and decrements the counter by one.
  
  When the counter reaches zero it clears the corresponding "active" bit in 
  ax.active_axes and tries to execute the next line by calling st_execute_line().
  When all active bits are zero st_execute_line() will succeed in loading the next
  line into the timers.
  
  You want the main_loop routines (gcode and motion_control non-ISRs) to run and 
  fill up the line buffer then sleep (idle) as the lines from the buffer are executed 
  The timer ISRs should continue to run lines and pull new lines out of the buffer,
  allowing the main routines to wake up and generate the next line segment.

  But you need some way to start the timers if they are not already running.
  So st_execute_line() must also be called from st_buffer_line() to start line
  execution of the timers are not already running.
  st_execute_line() therefore has a busy flag to prevent ISR and non-ISR invocation
  from stepping on each other,

  To Do
	- Remove steps_max from the line buffer. It's not needed.

*/

#include <math.h>
#include <stdlib.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include "xmega_support.h"
#include <util/delay.h>
#include "stepper.h"
#include "config.h"
#include "nuts_bolts.h"

#include "wiring_serial.h"	// ++++ NEEDED FOR DEBUG ONLY - OTHERWISE NO PRINTING
#include <avr/pgmspace.h>

struct Line {
// 	int32_t steps_max; 	//++++++++				// total steps it will take (max of the axes)
 	uint32_t steps_x; 					// total steps in x direction
	uint32_t steps_y;  					// total steps in y direction
	uint32_t steps_z; 					// total steps in z direction
 	uint32_t microseconds; 				// total microseconds the move will take
// 	uint8_t direction_bits;	//++++++			// bitmask for directions
};

#define LINE_BUFFER_SIZE 40					// number of lines buffered
struct Line line_buffer[LINE_BUFFER_SIZE];	// buffer for line instructions
struct Line *ln = NULL;						// pointer to current line
volatile int line_buffer_head = 0;
volatile int line_buffer_tail = 0;
volatile int busy; // TRUE when st_execute_line is running. Used to avoid retriggering ++++++

struct axes ax;


/* st_motor_test() - test motor subsystem */

void st_motor_test() {
	ax.x.counter = 0x00000100;					// number of steps
	ax.x.timer->CTRLA = TC_CLK_DIV_1;			// clock division ratio
	ax.x.timer->PERH = 0x10;					// step rate (period) high
	ax.x.timer->PERL = 0x00;					// step rate (period) low

	ax.y.counter = 0x00000100;					// number of steps
	ax.y.timer->CTRLA = TC_CLK_DIV_1;			// clock division ratio
	ax.y.timer->PERH = 0x20;					// step rate (period) high
	ax.y.timer->PERL = 0x00;					// step rate (period) low

	ax.z.counter = 0x00000100;					// number of steps
	ax.z.timer->CTRLA = TC_CLK_DIV_1;			// clock division ratio
	ax.z.timer->PERH = 0x30;					// step rate (period) high
	ax.z.timer->PERL = 0x00;					// step rate (period) low

	ax.a.counter = 0x00000100;					// number of steps
	ax.a.timer->CTRLA = TC_CLK_DIV_1;			// clock division ratio
	ax.a.timer->PERH = 0x40;					// step rate (period) high
	ax.a.timer->PERL = 0x00;					// step rate (period) low

	ax.active_axes |= (X_BIT_bm | Y_BIT_bm | Z_BIT_bm | A_BIT_bm);

}


/* st_init() - initialize and start the stepper motor subsystem 

   State at completion of initialization is:
	- each axis has a structure wth an initialized port and a timer bound to it
	- ports:
		- input and output directions set
	- each axis is enabled (

*/

void st_init()
{
	ax.active_axes = 0;							// clear all active bits

 /* initialize X axis */
 	// operating variables
	ax.x.counter = 0;							// down counts timer steps 

	// configuration variables
	ax.x.microsteps = X_MICROSTEPS;
	ax.x.max_seek_rate = X_SEEK_WHOLE_STEPS_PER_SEC;
	ax.x.max_seek_steps = X_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.x.max_feed_rate = DEFAULT_FEEDRATE;
	ax.x.max_feed_steps = X_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.x.steps_per_mm = X_STEPS_PER_MM;

	// motor control port bound to structure 
	ax.x.port = &X_MOTOR_PORT;
	ax.x.port->DIR = X_MOTOR_PORT_DIR_gm;		// set inputs and outputs
	ax.x.port->OUT = 0;							// set port bits to zero initially
	ax.x.port->OUT |= MICROSTEP_EIGHTH_bm;		// set microstep bits to eighth

	// motor control timer channel bound to structure
	ax.x.timer = &X_TIMER;
	ax.x.timer->CTRLA = TC_CLK_OFF;				// turn motor off
	ax.x.timer->CTRLB = TC_WGMODE;				// waveform generation mode
	ax.x.timer->INTCTRLA = TC_OVFINTLVL;		// interrupt mode
	ax.x.timer->PERH = 0x00;					// period high
	ax.x.timer->PERL = 0x00;					// period low

 /* initialize Y axis */
	ax.y.counter = 0;

	ax.y.microsteps = Y_MICROSTEPS;
	ax.y.max_seek_rate = Y_SEEK_WHOLE_STEPS_PER_SEC;
	ax.y.max_seek_steps = Y_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.y.max_feed_rate = DEFAULT_FEEDRATE;
	ax.y.max_feed_steps = Y_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.y.steps_per_mm = Y_STEPS_PER_MM;

	ax.y.port = &Y_MOTOR_PORT;					// bind port to structure 
	ax.y.port->DIR = Y_MOTOR_PORT_DIR_gm;
	ax.y.port->OUT = 0;							// set port bits to zero initially
	ax.y.port->OUT |= MICROSTEP_EIGHTH_bm;		// set microstep bits to eighth

	ax.y.timer = &Y_TIMER;
	ax.y.timer->CTRLA = TC_CLK_OFF;				// default division ratio
	ax.y.timer->CTRLB = TC_WGMODE;				// waveform generation mode
	ax.y.timer->INTCTRLA = TC_OVFINTLVL;		// interrupt mode
	ax.y.timer->PERH = 0x00;					// period high
	ax.y.timer->PERL = 0x00;					// period low

/* initialize Z axis */
	ax.z.counter = 0;

	ax.z.microsteps = Z_MICROSTEPS;
	ax.z.max_seek_rate = Z_SEEK_WHOLE_STEPS_PER_SEC;
	ax.z.max_seek_steps = Z_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.z.max_feed_rate = DEFAULT_FEEDRATE;
	ax.z.max_feed_steps = Z_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.z.steps_per_mm = Z_STEPS_PER_MM;

	ax.z.port = &Z_MOTOR_PORT;
	ax.z.port->DIR = Z_MOTOR_PORT_DIR_gm;
	ax.z.port->OUT = 0;							// set port bits to zero initially
	ax.z.port->OUT |= MICROSTEP_EIGHTH_bm;		// set microstep bits to eighth

	ax.z.timer = &Z_TIMER;
	ax.z.timer->CTRLA = TC_CLK_OFF;				// timer clock control or division
	ax.z.timer->CTRLB = TC_WGMODE;				// waveform generation mode
	ax.z.timer->INTCTRLA = TC_OVFINTLVL;		// interrupt mode
	ax.z.timer->PERH = 0x00;					// period high
	ax.z.timer->PERL = 0x00;					// period low

/* initialize A axis */
	ax.a.counter = 0;

	ax.a.microsteps = A_MICROSTEPS;
	ax.a.max_seek_rate = A_SEEK_WHOLE_STEPS_PER_SEC;
	ax.a.max_seek_steps = A_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.a.max_feed_rate = DEFAULT_FEEDRATE;
	ax.a.max_feed_steps = A_STEPS_PER_MM / DEFAULT_FEEDRATE;
	ax.a.steps_per_mm = A_STEPS_PER_MM;

	ax.a.port = &A_MOTOR_PORT;
	ax.a.port->DIR = A_MOTOR_PORT_DIR_gm;
	ax.a.port->OUT = 0;							// set port bits to zero initially
	ax.a.port->OUT |= MICROSTEP_EIGHTH_bm;		// set microstep bits to eighth

	ax.a.timer = &A_TIMER;
	ax.a.timer->CTRLA = TC_CLK_OFF;				
	ax.a.timer->CTRLB = TC_WGMODE;				// waveform generation mode
	ax.a.timer->INTCTRLA = TC_OVFINTLVL;		// interrupt mode
	ax.a.timer->PERH = 0x00;					// period high
	ax.a.timer->PERL = 0x00;					// period low

	// high level interrupts must be enabled in main()

	st_motor_test();							// run the motor test
}


/*******************************************************************************
 Motor timer interrupt service routines - service a tick from the axis timer
*******************************************************************************/

ISR(X_TIMER_vect)
{
	ax.x.port->OUTSET = STEP_BIT_bm;			// turn X step bit on
	if (--ax.x.counter == 0) {
		ax.x.timer->CTRLA = TC_CLK_OFF;			// stop the clock
		ax.active_axes &= ~X_BIT_bm;			// clear the X active bit
		st_execute_line();						// try to exec next line+++++
	}
//	_delay_us(STEP_PULSE_MICROSECONDS);			// delay for correct pulse width
	ax.x.port->OUTCLR = STEP_BIT_bm;			// turn X step bit off
}

ISR(Y_TIMER_vect)
{
	ax.y.port->OUTSET = STEP_BIT_bm;
	Y_MOTOR_PORT.OUTSET	= STEP_BIT_bm;
	if (--ax.y.counter == 0) {
		ax.y.timer->CTRLA = TC_CLK_OFF;			// stop the clock
		ax.active_axes &= ~Y_BIT_bm;			// clear the Y active bit
		st_execute_line();						// try to exec next line++++++
	}
//	_delay_us(STEP_PULSE_MICROSECONDS);
	Y_MOTOR_PORT.OUTCLR	= STEP_BIT_bm;
}

ISR(Z_TIMER_vect)
{
	Z_MOTOR_PORT.OUTSET	= STEP_BIT_bm;
	if (--ax.z.counter == 0) {
		ax.z.timer->CTRLA = TC_CLK_OFF;			// stop the clock
		ax.active_axes &= ~Z_BIT_bm;			// clear the Z active bit
		st_execute_line();						// try to exec next line+++++++++
	}
//	_delay_us(STEP_PULSE_MICROSECONDS);
	Z_MOTOR_PORT.OUTCLR	= STEP_BIT_bm;
}

ISR(A_TIMER_vect)
{
	A_MOTOR_PORT.OUTSET	= STEP_BIT_bm;
	if (--ax.a.counter == 0) {
		ax.a.timer->CTRLA = TC_CLK_OFF;			// stop the clock
		ax.active_axes &= ~A_BIT_bm;			// clear the A active bit
		st_execute_line();						// try to exec next line+++++++
	}
//	_delay_us(STEP_PULSE_MICROSECONDS);
	A_MOTOR_PORT.OUTCLR	= STEP_BIT_bm;
}

/*******************************************************************************
  st_execute_line() - run a stepper line +++++++

  Load next line into timers and set direction bits
  If the line is currently active it will not do the load
  If the routine is busy it will not do the load
  Busy flag is needed as the routine may be called by either an ISR or non-ISR,
  and we don't want it to execute over itself.

  Variables in ln mean:
	ln->steps_x = steps to take in X 
	ln->steps_y = steps to take in Y
	ln->steps_z = steps to take in Z
	ln->rate = total microseconds the move should take - calculates step rate

	step_rate is computed as steps_ / microseconds

*******************************************************************************/

void st_execute_line()
{
//	uint32_t step_rate_x; 					// step rate in microseconds per step
//	uint32_t step_rate_y; 
//	uint32_t step_rate_z; 
//++++++++++++
	if (busy) {  							// busy-flag to avoid reentry
		return; 
	}
	if (ax.active_axes != 0) {
		return;								// if any bit is set the robot is active
	}
	if ((ln = st_get_next_line()) == NULL) {
		return;
  	} 
	st_print_line(*ln);						// ++++ DEBUG CODE

	busy = TRUE;							//++++++++++++

	// compute the timer intervals we will need (in microseconds)
//	step_rate_x = (ln->microseconds / ln->steps_x);	// step_rate in microseconds per step
//	step_rate_y = (ln->microseconds / ln->steps_y);
//	step_rate_z = (ln->microseconds / ln->steps_z);
//	st_print_four_ints(step_rate_x, step_rate_y, step_rate_z, ln->microseconds);
//	st_print_four_ints(((step_rate_x >> 8) & 0x000000FF), (step_rate_x & 0x000000FF), 0, 0);
	
	// set direction bits
	/*
	if (ln->direction_bits & X_DIRECTION_BIT_bm) {			// if X is set (CW)
		ax.x.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.x.port->OUTCLR = DIRECTION_BIT_bm;
	}
	if (ln->direction_bits & Y_DIRECTION_BIT_bm) {
		ax.y.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.y.port->OUTCLR = DIRECTION_BIT_bm;
	}
	if (ln->direction_bits & Z_DIRECTION_BIT_bm) {
		ax.z.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.z.port->OUTCLR = DIRECTION_BIT_bm;
	}
	*/

	(ln->steps_x > 0) ? (ax.x.port->OUTSET=DIRECTION_BIT_bm) : 
						(ax.x.port->OUTCLR=DIRECTION_BIT_bm);
	(ln->steps_y > 0) ? (ax.y.port->OUTSET=DIRECTION_BIT_bm) : 
						(ax.y.port->OUTCLR=DIRECTION_BIT_bm);
	(ln->steps_z > 0) ? (ax.z.port->OUTSET=DIRECTION_BIT_bm) : 
						(ax.z.port->OUTCLR=DIRECTION_BIT_bm);

	// set direction bits
	/*
	if (ln->steps_x > 0) {							// X is positive (CW)
		ax.x.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.x.port->OUTCLR = DIRECTION_BIT_bm;
	}
	if (ln->steps_y > 0) {
		ax.y.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.y.port->OUTCLR = DIRECTION_BIT_bm;
	}
	if (ln->steps_z > 0) {
		ax.z.port->OUTSET = DIRECTION_BIT_bm;
	} else {
		ax.z.port->OUTCLR = DIRECTION_BIT_bm;
	}
	*/

	// transform step rates into timer load values and load timer
	st_load_timer(&ax.x, (ln->microseconds / ln->steps_x), ln->microseconds);
	st_load_timer(&ax.y, (ln->microseconds / ln->steps_y), ln->microseconds);
	st_load_timer(&ax.z, (ln->microseconds / ln->steps_z), ln->microseconds);

//	st_load_timer(&ax.x, step_rate_x, ln->microseconds);
//	st_load_timer(&ax.y, step_rate_y, ln->microseconds);
//	st_load_timer(&ax.z, step_rate_z, ln->microseconds);

	busy = FALSE;				//++++++++++++
}

/* st_load_timer() - helper routine for st_execute line */

void st_load_timer(struct axis *a, uint32_t step_rate, uint32_t microseconds) 
{
	if (step_rate < (DIV1_RANGE)) {				// short timer - up to 2000 uSec
		a->timer->CTRLA = TC_CLK_DIV_1;			// set clock divisor
		a->counter = (microseconds/step_rate);	// # of steps to make at this rate
		step_rate = (step_rate * 32);
		a->timer->PERH = (uint8_t)((step_rate >> 8) & 0x000000FF);	// period high
		a->timer->PERL = (uint8_t)(step_rate & 0x000000FF);			// period low
	}
}


/* st_get_next_line() - return the next line from the line buffer & advance buffer tail*/ 

struct Line *st_get_next_line()
{
	struct Line *ln;
	 
	if (line_buffer_head == line_buffer_tail) {	// buffer empty
		return (NULL);
	}
	ln = &line_buffer[line_buffer_tail];		 // get and save the current pointer
	if (++line_buffer_tail > LINE_BUFFER_SIZE) { // increment and wrap
		line_buffer_tail = 0;
	}
	return (ln); 
//	return (&line_buffer[line_buffer_tail]); 
}


/* st_synchronize() - block until all buffered steps are executed */

void st_synchronize()
{
	while(line_buffer_tail != line_buffer_head) {
		sleep_mode();
	}    
}

/* st_flush() - cancel all buffered steps */

void st_flush()
{
	cli();
	line_buffer_tail = line_buffer_head;
	ln = NULL;
	sei();
}

/* st_buffer_line()

	Add a new linear movement to the buffer. 
	steps_x, _y and _z is the signed relative motion in steps. 
	Microseconds specify how many microseconds the move should take to perform.
*/

void st_buffer_line(int32_t steps_x, int32_t steps_y, int32_t steps_z, uint32_t microseconds) 
{
	// Calculate the buffer head after we push this byte
	int next_buffer_head = (line_buffer_head + 1) % LINE_BUFFER_SIZE;	

	// If the buffer is full: good! That means we are well ahead of the robot. 
	// Nap until there is room in the buffer.
	while(line_buffer_tail == next_buffer_head) { 
		sleep_mode(); 
	};

	struct Line *line = &line_buffer[line_buffer_head];  	// Setup line record
	line->steps_x = labs(steps_x);
	line->steps_y = labs(steps_y);
	line->steps_z = labs(steps_z);  
//	line->steps_max = max(line->steps_x, max(line->steps_y, line->steps_z)); ++++++

  	// Bail if this is a zero-length line
	if (max(line->steps_x, max(line->steps_y, line->steps_z)) == 0) { 	//++++++
		return;
	};

//	line->rate = microseconds/line->maximum_steps;
	line->microseconds = microseconds;

	uint8_t direction_bits = 0;
	if (steps_x < 0) { 
		direction_bits |= X_DIRECTION_BIT_bm; 
	}
	if (steps_y < 0) { 
		direction_bits |= Y_DIRECTION_BIT_bm; 
	}
	if (steps_z < 0) { 
		direction_bits |= Z_DIRECTION_BIT_bm; 
	}
//	line->direction_bits = direction_bits;		//++++++++++
	line_buffer_head = next_buffer_head;		// Move buffer head

//	st_print_line(*line);						// ++++ DEBUG CODE
	st_execute_line();						// attempt to run this line +++++++
}

/* st_go_home() - perform the homing cycle */

void st_go_home()
{
  // Todo: Perform the homing cycle
}

// ++++ DEBUG CODE
void st_print_four_ints(long x, long y, long z, long u) {
	printPgmString(PSTR("Line: X="));
	printInteger(x);
	printPgmString(PSTR(" Y="));
	printInteger(y);
	printPgmString(PSTR(" Z="));
	printInteger(z);
	printPgmString(PSTR(" uS="));
	printInteger(u);
	printPgmString(PSTR("\r\n"));
}

void st_print_active() {
	printPgmString(PSTR("ACTIVE = "));
	printHex(ax.active_axes);
	printPgmString(PSTR("\r\n"));
}

void st_print_line(struct Line line) {
	printPgmString(PSTR("Line X="));		// ++++ DEBUG CODE
	printInteger(line.steps_x);
	printPgmString(PSTR(", Y="));
	printInteger(line.steps_y);
	printPgmString(PSTR(", Z="));
	printInteger(line.steps_z);
	printPgmString(PSTR(", uS="));
	printInteger(line.microseconds);
//	printPgmString(PSTR(", D="));
//	printHex(line.direction_bits);
//	printPgmString(PSTR(", Steps="));
//	printInteger(line.steps_max);
 	printPgmString(PSTR("\r\n"));
}