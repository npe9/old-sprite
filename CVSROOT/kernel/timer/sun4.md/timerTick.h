/*
 * timerTick.h --
 *
 *      Declarations for the Sun3 machine dependent clock timer.
 *
 * Copyright 1985, 1988 Regents of the University of California
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies.  The University of California
 * makes no representations about the suitability of this
 * software for any purpose.  It is provided "as is" without
 * express or implied warranty.
 *
 * rcsid: $Header$ SPRITE (Berkeley) 
 */

#ifndef _TIMERTICK
#define _TIMERTICK

#include "time.h"


/* DATA STRUCTURES: */


/*
 * On the Sun-3, Timer_Ticks is just a Time value because the free-running
 * counter keeps track of time.
 */
typedef Time Timer_Ticks;


/*
 * Definitions for useful intervals and clock values. 
 * See timer.h for more details.
 */
 
 extern unsigned int 	timer_IntZeroSeconds; 
 extern unsigned int 	timer_IntOneMillisecond;   
 extern unsigned int 	timer_IntOneSecond;
 extern unsigned int 	timer_IntOneMinute;
 extern unsigned int 	timer_IntOneHour; 
 extern Timer_Ticks	timer_TicksZeroSeconds;
 extern Time 		timer_MaxIntervalTime; 



/*
 * Because the Sun3's timer speaks in the same units as type Time, we can
 * use the Time_ modules routines to Add and Subtract
 */

#define Timer_AddTicks(a,b,c)		Time_Add(a,b,c)
#define Timer_SubtractTicks(a,b,c)	Time_Subtract(a,b,c)

/*
 * Conversion to Ticks to Time and Time to Ticks is fast too.
 */

#define Timer_TicksToTime(a,b)		*(b) = a;
#define Timer_TimeToTicks(a,b)		*(b) = a;


extern void Timer_AddIntervalToTicks();
extern void Timer_GetCurrentTicks();


/*
 *----------------------------------------------------------------------
 *
 * Tick Comparisons --
 *
 *	Timer_TickLT:	tick1  <   tick2
 *	Timer_TickLE:	tick1  <=  tick2
 *	Timer_TickEQ:	tick1  ==  tick2
 *	Timer_TickGE:	tick1  >=  tick2
 *	Timer_TickGT:	tick1  >   tick2
 *
 * Results:
 *     TRUE	- the relation holds for the 2 values.
 *     FALSE	- the relation does not hold.
 *
 * Side effects:
 *     None.
 *
 *----------------------------------------------------------------------
 */

#define Timer_TickLT(a,b)	Time_LT(a,b)
#define Timer_TickLE(a,b)	Time_LE(a,b)
#define Timer_TickEQ(a,b)	Time_EQ(a,b)
#define Timer_TickGE(a,b)	Time_GE(a,b)
#define Timer_TickGT(a,b)	Time_GT(a,b)
#endif _TIMERTICK
