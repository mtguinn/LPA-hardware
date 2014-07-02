/*
 * firmware.cpp
 *
 * Created: 28/02/2014 02:01:59 PM
 *  Author: Sebastian Castillo
 */ 

// Headers
#include <avr/interrupt.h>
#include <util/delay.h>
#include <util/atomic.h>

#include "Arduino.h"
#include <SD.h>

#include "StatusLeds/StatusLeds.h"
#include "Tlc5941/Tlc5941.h"
#include "MsTimer/MsTimer.h"

// System states
#define System_stateInitializing 0
#define System_stateRunning 1
#define System_stateFinished 2
#define System_stateErrorNoSdCard 3
#define System_stateErrorNoLpf 4
#define System_stateErrorWrongLpf 5
#define System_stateErrorTimeout 6
#define System_stateErrorLpfUnavailable 7

volatile uint8_t System_state;

#define System_SetState(state) System_state = state;
#define System_IsState(state) System_state == state
#define System_IsNotState(state) System_state != state

// Light program file
File lpf;

// Synchronization variable
volatile int8_t dataAvailableFlag = 0;
#define Flag_Set(f) f++
#define Flag_Release(f) f--
#define Flag_Wait(f) while(f)
#define Flag_IsSet(f) (f>0)
#define Flag_HasFailedRelease(f) (f<0)

void UpdateLeds(void) {
	// Release data available flag
	if (System_IsState(System_stateRunning))
	{
		Flag_Release(dataAvailableFlag);
	}

	// Set update flag for Tlc library
	if (!Flag_HasFailedRelease(dataAvailableFlag))
		Tlc5941_SetGSUpdateFlag();
}

void UpdateStatusLeds(void) {
	switch (System_state)
	{
	case System_stateInitializing:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	case System_stateRunning:
		StatusLeds_Toggle(StatusLeds_LedOn);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	case System_stateFinished:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_On);
		break;
	case System_stateErrorNoSdCard:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_On);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	case System_stateErrorNoLpf:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_Off);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_On);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_On);
		break;
	case System_stateErrorWrongLpf:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_On);
		StatusLeds_Set(StatusLeds_LedErr, StatusLeds_On);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	case System_stateErrorTimeout:
		StatusLeds_Toggle(StatusLeds_LedOn);
		StatusLeds_Toggle(StatusLeds_LedErr);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	case System_stateErrorLpfUnavailable:
		StatusLeds_Set(StatusLeds_LedOn, StatusLeds_Off);
		StatusLeds_Toggle(StatusLeds_LedErr);
		StatusLeds_Set(StatusLeds_LedFin, StatusLeds_Off);
		break;
	}
}

// This function is required for proper operation of the arduino libraries.
// It uses the same settings for timer 0 as init() in wiring.c
void timer0_init()
{
	#ifndef cbi
	#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
	#endif
	#ifndef sbi
	#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
	#endif
	
	// on the ATmega168, timer 0 is also used for fast hardware pwm
	// (using phase-correct PWM would mean that timer 0 overflowed half as often
	// resulting in different millis() behavior on the ATmega8 and ATmega168)
	sbi(TCCR0A, WGM01);
	sbi(TCCR0A, WGM00);

	// set timer 0 prescale factor to 64
	sbi(TCCR0B, CS01);
	sbi(TCCR0B, CS00);

	// enable timer 0 overflow interrupt
	sbi(TIMSK0, TOIE0);
}

int main(void) {
	// counter for led update
	uint8_t ledCounter = 0;

	// Enable interruptions
	sei();
	
	// Initialize TLC module
	Tlc5941_Init();
	// Set up grayscale value
	Tlc5941_SetAllDC(8);
	Tlc5941_ClockInDC();
	// Default all grayscale values to off
	Tlc5941_SetAllGS(0);
	// Force upload of grayscale values
	Tlc5941_SetGSUpdateFlag();
	while(Tlc5941_gsUpdateFlag);

	// Signal that the first set of grayscale values should be used during the first iteration
	Flag_Set(dataAvailableFlag);
	
	// Initialize Status LEDs
	StatusLeds_Init();
	
	// Initialize ms timer
	MsTimer_Init();
	// Assign callbacks
	MsTimer_AddCallback(&UpdateLeds, 10);
	MsTimer_AddCallback(&UpdateStatusLeds, 500);

	// Initialize timer 0 before using the SD card library
	timer0_init();
	// Initialize system state
	System_SetState(System_stateInitializing);
	
	// Test if SD card is present and initialize
	if (!SD.begin())
	{
		System_SetState(System_stateErrorNoSdCard);
	}
	// Test if SD card is present
	if (System_IsState(System_stateInitializing))
	{
		lpf = SD.open("program.lpf", FILE_READ);
		if (!lpf) {
				System_SetState(System_stateErrorNoLpf);
			}
	}

	// Get headers from LPF
	// TODO
	// Verify headers from LPF
	// TODO
	// Switch to running state
	if (System_IsState(System_stateInitializing))
	{
		System_SetState(System_stateRunning);
	}
	
	// Start timer
	MsTimer_Start();

	// Do led intensity decoding as necessary
	while(1) {
		if (System_IsState(System_stateRunning))
		{
			// Wait until data has been consumed
			while(Flag_IsSet(dataAvailableFlag));

			// Wait until TLC library is done transmitting
			while(Tlc5941_gsUpdateFlag);

			// Read data from LPF
			// TODO

			// Temporary data generation
			// Set all LEDs to a constant value
			for (uint8_t i = 0; i < Tlc5941_N*16; i++)
			{
				if (i == ledCounter)
					Tlc5941_SetGS(i, 4095);
				else
					Tlc5941_SetGS(i, 0);
			}
			ledCounter = (ledCounter + 1) % (Tlc5941_N*16);

			// Check if last data access was met
			// This should be run as an atomic block
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
				if (Flag_HasFailedRelease(dataAvailableFlag))
				{
					System_SetState(System_stateErrorTimeout);
				}
				else
				{
					Flag_Set(dataAvailableFlag);
				}
			}
		}
	}
	return 0;
}
