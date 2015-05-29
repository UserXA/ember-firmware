#include <math.h>       // isinfinite()
#include <avr/interrupt.h>

#include "tinyg.h"
#include "util.h"
#include "planner.h"
#include "stepper.h"
#include "Hardware.h"
#include "MachineDefinitions.h"

static void _exec_move(void);
static void _load_move(void);
static void _request_load_move(void);

/*
 * Stepper structures
 *
 *  There are 4 sets of structures involved in this operation;
 *
 *  data structure:           static to:    runs at:
 *    mpBuffer planning buffers (bf)    planner.c     main loop
 *    mrRuntimeSingleton (mr)       planner.c     MED ISR
 *    stPrepSingleton (sp)          stepper.c     MED ISR
 *    stRunSingleton (st)         stepper.c     HI ISR
 *  
 *  Care has been taken to isolate actions on these structures to the 
 *  execution level in which they run and to use the minimum number of 
 *  volatiles in these structures. This allows the compiler to optimize
 *  the stepper inner-loops better.
 */

// Runtime structs. Used exclusively by step generation ISR (HI)
typedef struct stRunMotor {     // one per controlled motor
  int32_t phase_increment;    // total steps in axis times substeps factor
  int32_t phase_accumulator;    // DDA phase angle accumulator for axis
  uint8_t polarity;       // 0=normal polarity, 1=reverse motor polarity
} stRunMotor_t;

typedef struct stRunSingleton {   // Stepper static values and axis parameters
  uint16_t magic_start;     // magic number to test memory integity 
  int32_t dda_ticks_downcount;  // tick down-counter (unscaled)
  int32_t dda_ticks_X_substeps; // ticks multiplied by scaling factor
  stRunMotor_t m[AXES_COUNT];     // runtime motor structures
} stRunSingleton_t;

// Prep-time structs. Used by exec/prep ISR (MED) and read-only during load 
// Must be careful about volatiles in this one

enum prepBufferState {
  PREP_BUFFER_OWNED_BY_LOADER = 0,// staging buffer is ready for load
  PREP_BUFFER_OWNED_BY_EXEC   // staging buffer is being loaded
};

typedef struct stPrepMotor {
  uint32_t phase_increment;     // total steps in axis times substep factor
  int8_t dir;           // b0 = direction
} stPrepMotor_t;

typedef struct stPrepSingleton {
  uint16_t magic_start;     // magic number to test memory integity 
  uint8_t move_type;        // move type
  uint8_t prep_state;       // set TRUE to load, false to skip
  volatile uint8_t exec_state;  // move execution state 
  volatile uint8_t reset_flag;  // TRUE if accumulator should be reset
  uint32_t prev_ticks;      // tick count from previous move
  //uint16_t dda_period;      // DDA or dwell clock period setting
  uint32_t dda_ticks;       // DDA or dwell ticks for the move
  uint32_t dda_ticks_X_substeps;  // DDA ticks scaled by substep factor
//  float segment_velocity;     // +++++ record segment velocity for diagnostics
  stPrepMotor_t m[AXES_COUNT];    // per-motor structs
} stPrepSingleton_t;

// Allocate static structures
static stRunSingleton_t st;
static struct stPrepSingleton sps;

static MotorController_t* mcState;

uint32_t stepCount[AXES_COUNT] = { 0 };

ISR(DDA_TIMER_ISR_vect)
{

  if ((st.m[Z_AXIS].phase_accumulator += st.m[Z_AXIS].phase_increment) > 0) {
    MOTOR_Z_STEP_PORT |= MOTOR_Z_STEP_BM;  // turn step bit on
    st.m[Z_AXIS].phase_accumulator -= st.dda_ticks_X_substeps;
    MOTOR_Z_STEP_PORT &= ~MOTOR_Z_STEP_BM; // turn step bit off in ~1 uSec
    stepCount[Z_AXIS]++;
  }

  if ((st.m[R_AXIS].phase_accumulator += st.m[R_AXIS].phase_increment) > 0) {
    MOTOR_R_STEP_PORT |= MOTOR_R_STEP_BM;  // turn step bit on
    st.m[R_AXIS].phase_accumulator -= st.dda_ticks_X_substeps;
    MOTOR_R_STEP_PORT &= ~MOTOR_R_STEP_BM; // turn step bit off in ~1 uSec
    stepCount[R_AXIS]++;
  }
  
  if (--st.dda_ticks_downcount == 0) {    // end move
      DDA_TIMER_CTRLB &= ~DDA_TIMER_CS_BM; // Disable DDA timer
      _load_move();             // load the next move
  }

}


/* Load move functions  */

// Request function - triggers software interrupt
static void _request_load_move()
{
  if (st.dda_ticks_downcount == 0) {        // bother interrupting
        LOAD_TIMER_CTRLB |= LOAD_TIMER_CS_BM; // Enable timer to generate load software interrupt
  }   // else don't bother to interrupt. You'll just trigger an 
    // interrupt and find out the load routine is not ready for you
}

// Interrupt service routine - responds to software interrupt
ISR(LOAD_TIMER_ISR_vect)
{
    LOAD_TIMER_CTRLB &= ~LOAD_TIMER_CS_BM; // Disable load software interrupt timer
    _load_move();
}

// Handler - called by ISR
/*
 * _load_move() - Dequeue move and load into stepper struct
 *
 *  This routine can only be called be called from an ISR at the same or 
 *  higher level as the DDA or dwell ISR. A software interrupt has been 
 *  provided to allow a non-ISR to request a load (see st_request_load_move())
 */
void _load_move()
{
  if (st.dda_ticks_downcount != 0) return;          // exit if it's still busy
  if (sps.exec_state != PREP_BUFFER_OWNED_BY_LOADER)
  {
      // there are no more moves
      mcState->motionComplete = true;
      return;
  }

  // handle aline loads first (most common case)  NB: there are no more lines, only alines
  if (sps.move_type == MOVE_TYPE_ALINE) {
    st.dda_ticks_downcount = sps.dda_ticks;
    st.dda_ticks_X_substeps = sps.dda_ticks_X_substeps;
    //TIMER_DDA_PERIOD = sps.dda_period;
 
    // This section is somewhat optimized for execution speed 
    // All axes must set steps and compensate for out-of-range pulse phasing. 
    // If axis has 0 steps the direction setting can be omitted
    // If axis has 0 steps enabling motors is req'd to support power mode = 1

    st.m[Z_AXIS].phase_increment = sps.m[Z_AXIS].phase_increment;     // set steps
    if (sps.reset_flag == true) {       // compensate for pulse phasing
      st.m[Z_AXIS].phase_accumulator = -(st.dda_ticks_downcount);
    }
    if (st.m[Z_AXIS].phase_increment != 0) {
      // For ideal optimizations, only set or clear a bit at a time.
      if (sps.m[Z_AXIS].dir == 0) {
        MOTOR_Z_DIRECTION_PORT &= ~MOTOR_Z_DIRECTION_BM; // CW motion (bit cleared)
      } else {
        MOTOR_Z_DIRECTION_PORT |= MOTOR_Z_DIRECTION_BM;  // CCW motion
      }
      // should already be enabled MOTOR_ENABLE_PORT &= ~MOTOR_ENABLE_BM;
    }

    st.m[R_AXIS].phase_increment = sps.m[R_AXIS].phase_increment;
    if (sps.reset_flag == true) {
      st.m[R_AXIS].phase_accumulator = -(st.dda_ticks_downcount);
    }
    if (st.m[R_AXIS].phase_increment != 0) {
      if (sps.m[R_AXIS].dir == 0) {
        MOTOR_R_DIRECTION_PORT &= ~MOTOR_R_DIRECTION_BM; // CW motion (bit cleared)
      } else {
        MOTOR_R_DIRECTION_PORT |= MOTOR_R_DIRECTION_BM;  // CCW motion
      }
    }

    // Enable the DDA timer
    // For some reason, the timer will not start properly unless the count and interrupt flag are cleared
    // According to the data sheet this occurs when the ISR executes
    DDA_TIMER_IFR |= DDA_TIMER_OCF_BM;
    DDA_TIMER_CNT = 0;
    DDA_TIMER_CTRLB |= DDA_TIMER_CS_BM;
  }

  // all other cases drop to here (e.g. Null moves after Mcodes skip to here) 
  sps.exec_state = PREP_BUFFER_OWNED_BY_EXEC;       // flip it back
  sps.prep_state = false;
  st_request_exec_move();                 // exec and prep next move
}


/* Exec move functions */

// Request function - triggers software interrupt
void st_request_exec_move()
{
  if (sps.exec_state == PREP_BUFFER_OWNED_BY_EXEC) {  // bother interrupting
    EXEC_TIMER_CTRLB |= EXEC_TIMER_CS_BM; // Enable timer to generate exec software interrupt
  }
}

// Interrupt service routine - responds to software interrupt
ISR(EXEC_TIMER_ISR_vect)
{
    EXEC_TIMER_CTRLB &= ~EXEC_TIMER_CS_BM; // Disable exec software interrupt timer
    _exec_move();               // NULL state
}

// Handler - called by ISR
static void _exec_move()
{
    if (sps.exec_state == PREP_BUFFER_OWNED_BY_EXEC) {
//    if (mp_exec_move(state) != STAT_NOOP) {
    if (mp_exec_move() != STAT_NOOP) {
      sps.exec_state = PREP_BUFFER_OWNED_BY_LOADER; // flip it back
      _request_load_move();
    }
  }
}



// functions not called from this file

/* 
 * st_init() - initialize stepper motor subsystem 
 *
 *  Notes:
 *    - This init requires sys_init() to be run beforehand
 *    This init is a precursor for gpio_init()
 *    - microsteps are setup during cfg_init()
 *    - motor polarity is setup during cfg_init()
 *    - high level interrupts must be enabled in main() once all inits are complete
 */

void st_init(MotorController_t* mc)
{
    mcState = mc;
//  You can assume all values are zeroed. If not, use this:
//  memset(&st, 0, sizeof(st)); // clear all values, pointers and status

  st.magic_start = MAGICNUM;
  sps.magic_start = MAGICNUM;

  sps.exec_state = PREP_BUFFER_OWNED_BY_EXEC;
}

/*
 *  * st_isbusy() - return TRUE if motors are running or a dwell is running
 *   */
uint8_t st_isbusy()
{
  if (st.dda_ticks_downcount == 0) {
    return (false);
  } 
  return (true);
}

/*
 * st_prep_line() - Prepare the next move for the loader
 *
 *  This function does the math on the next pulse segment and gets it ready for 
 *  the loader. It deals with all the DDA optimizations and timer setups so that
 *  loading can be performed as rapidly as possible. It works in joint space 
 *  (motors) and it works in steps, not length units. All args are provided as 
 *  floats and converted to their appropriate integer types for the loader. 
 *
 * Args:
 *  steps[] are signed relative motion in steps (can be non-integer values)
 *  Microseconds - how many microseconds the segment should run 
 */

stat_t st_prep_line(float steps[], uint8_t directions[], float microseconds)
{
  float f_dda = F_DDA;    // starting point for adjustment
  float dda_substeps = DDA_SUBSTEPS;

  // *** defensive programming ***
  // trap conditions that would prevent queueing the line
  if (sps.exec_state != PREP_BUFFER_OWNED_BY_EXEC) { return (STAT_INTERNAL_ERROR);
  } else if (isfinite(microseconds) == false) { return (STAT_MINIMUM_LENGTH_MOVE_ERROR);
  } else if (microseconds < EPSILON) { return (STAT_MINIMUM_TIME_MOVE_ERROR);
  }
  sps.reset_flag = false;   // initialize accumulator reset flag for this move.

  // setup motor parameters
    sps.m[Z_AXIS].dir = directions[Z_AXIS] ^ Z_AXIS_MOTOR_POLARITY;
    sps.m[Z_AXIS].phase_increment = (uint32_t)fabs(steps[Z_AXIS] * dda_substeps);
    sps.m[R_AXIS].dir = directions[R_AXIS] ^ R_AXIS_MOTOR_POLARITY;
    sps.m[R_AXIS].phase_increment = (uint32_t)fabs(steps[R_AXIS] * dda_substeps);
  //sps.dda_period = _f_to_period(f_dda);
  sps.dda_ticks = (uint32_t)((microseconds/1000000) * f_dda);
  sps.dda_ticks_X_substeps = sps.dda_ticks * dda_substeps;  // see FOOTNOTE

  // anti-stall measure in case change in velocity between segments is too great 
  if ((sps.dda_ticks * ACCUMULATOR_RESET_FACTOR) < sps.prev_ticks) {  // NB: uint32_t math
    sps.reset_flag = true;
  }
  sps.prev_ticks = sps.dda_ticks;
  sps.move_type = MOVE_TYPE_ALINE;
  sps.prep_state = true;
  return (STAT_OK);
}

/* 
 * st_prep_null() - Keeps the loader happy. Otherwise performs no action
 *
 *  Used by M codes, tool and spindle changes
 */

void st_prep_null()
{
  sps.move_type = MOVE_TYPE_NULL;
  sps.prep_state = true;
}
