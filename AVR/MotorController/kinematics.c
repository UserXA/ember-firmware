/*
 * kinematics.c - inverse kinematics routines
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2013 Alden S. Hart Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tinyg.h"
//#include "config.h"
//#include "gcode_parser.h"
#include "canonical_machine.h"
#include "kinematics.h"

//static void _inverse_kinematics(float travel[], float joint[], float microseconds);

/*
 * ik_kinematics() - wrapper routine for inverse kinematics
 *
 *	Calls kinematics function(s). 
 *	Performs axis mapping & conversion of length units to steps (see note)
 *	Also deals with inhibited axes
 *
 *	Note: The reason steps are returned as floats (as opposed to, say,
 *		  uint32_t) is to accommodate fractional DDA steps. The DDA deals 
 *		  with fractional step values as fixed-point binary in order to get
 *		  the smoothest possible operation. Steps are passed to the move prep
 *		  routine as floats and converted to fixed-point binary during queue 
 *		  loading. See stepper.c for details.
 */

float PulsesPerUnit = 0.0;

void ik_kinematics(float travel[], float steps[], float microseconds)
{
	//JL: unused uint8_t i;
	float joint[AXES];

//	_inverse_kinematics(travel, joint, microseconds);// you can insert inverse kinematics transformations here
	memcpy(joint, travel, sizeof(float)*AXES);		 //...or just do a memcopy for cartesian machines

	// Map motors to axes and convert length units to steps
	// Most of the conversion math has already been done in steps_per_unit
	// which takes axis travel, step angle and microsteps into account.

  /* JL: hardcode config and single motor
	for (i=0; i<AXES; i++) {
		if (cfg.a[i].axis_mode == AXIS_INHIBITED) { joint[i] = 0;}
		if (cfg.m[MOTOR_1].motor_map == i) { steps[MOTOR_1] = joint[i] * cfg.m[MOTOR_1].steps_per_unit;}
		if (cfg.m[MOTOR_2].motor_map == i) { steps[MOTOR_2] = joint[i] * cfg.m[MOTOR_2].steps_per_unit;}
		if (cfg.m[MOTOR_3].motor_map == i) { steps[MOTOR_3] = joint[i] * cfg.m[MOTOR_3].steps_per_unit;}
		if (cfg.m[MOTOR_4].motor_map == i) { steps[MOTOR_4] = joint[i] * cfg.m[MOTOR_4].steps_per_unit;}
	// the above is a loop unrolled version of this:
	//	for (uint8_t j=0; j<MOTORS; j++) {
	//		if (cfg.m[j].motor_map == i) { steps[j] = joint[i] * cfg.m[j].steps_per_unit;}
	//	}
	}
  */
	steps[0] = joint[0] * PulsesPerUnit;
}

/*
 * _inverse_kinematics() - inverse kinematics - example is for a cartesian machine
 *
 *	You can glue in inverse kinematics here, but be aware of time budget constrants.
 *	This function is run during the _exec() portion of the cycle and will therefore
 *	be run once per interpolation segment. The total time for the segment load, 
 *	including the inverse kinematics transformation cannot exceed the segment time, 
 *	and ideally should be no more than 25-50% of the segment time. Currently segments 
 *	run avery 5 ms, but this might be lowered. To profile this time look at the 
 *	time it takes to complete the mp_exec_move() function.
 */
/*
static void _inverse_kinematics(float travel[], float joint[], float microseconds)
{
	for (uint8_t i=0; i<AXES; i++) {
		joint[i] = travel[i];
	}	
}
*/

//############## UNIT TESTS ################

//#define __UNIT_TEST_KINEMATICS
#ifdef __UNIT_TESTS
#ifdef __UNIT_TEST_KINEMATICS

void _ik_test_inverse_kinematics(void);

void ik_unit_tests()
{
	_ik_test_inverse_kinematics();
}

void _ik_test_inverse_kinematics(void)
{
	return;
}

#endif
#endif