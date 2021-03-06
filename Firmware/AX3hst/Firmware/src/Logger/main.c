/* 
 * Copyright (c) 2009-2012, Newcastle University, UK.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met: 
 * 1. Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, 
 *    this list of conditions and the following disclaimer in the documentation 
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE 
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF 
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN 
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE. 
 */

// AX3 Main Code
// Dan Jackson, Karim Ladha, 2011-2012

// Includes
#include <Compiler.h>
#include <TimeDelay.h>
#include "HardwareProfile.h"
#include "Peripherals/Nand.h"
#include "Peripherals/myI2C.h"
#include "Peripherals/Accel.h"
#include "Peripherals/ADC MCP3421.h"
#include "Peripherals/TEMP MCP9800.h"
#include "Peripherals/Rtc.h"
#include "USB/USB.h"
#include "USB/usb_function_msd.h"
#include "USB/usb_function_cdc.h"
#include "MDD File System/FSIO.h"
#include "Usb/USB_CDC_MSD.h"
#include "Ftl/FsFtl.h"
#include "Utils/Fifo.h"
#include "Utils/Util.h"
#include "Peripherals/Analog.h"
#include "Settings.h"
#include "Logger.h"
//#include <string.h>



// Declarations
void RunAttached(void);
void TimedTasks(void);
void LedTasks(void);
extern void LoggerRTCTasks(void); 
extern void LoggerTimerTasks(void);
extern void RunLogging(void);

// Interrupts
void __attribute__((interrupt,auto_psv)) _DefaultInterrupt(void)
{
 	static unsigned int INTCON1val;
	LED_SET(LED_MAGENTA);
	INTCON1val = INTCON1;
	Nop();
	Nop();
	Nop();
	Nop();
	INTCON1 = 0;
    Reset();
}

// RTC
void __attribute__((interrupt,auto_psv)) _RTCCInterrupt(void)
{
	RtcTasks();
    RtcSwwdtIncrement();    // Increment software RTC, reset if overflow
	LoggerRTCTasks(); 
}


// TMR1
void __attribute__((interrupt, shadow, auto_psv)) _T1Interrupt(void)
{
    RtcTimerTasks();
	LoggerTimerTasks();
}

// CN
void __attribute__((interrupt, shadow, auto_psv)) _CNInterrupt(void)
{
	IFS1bits.CNIF = 0;
}


// Restart flag
unsigned char restart = 0;
static unsigned char inactive = 0;
static unsigned short lastTime = 0;

// Main routine
int main(void)
{
    // Initialization
	InitIO();			// I/O pins
	CLOCK_SOSCEN(); 	// For the RTC
	WaitForPrecharge();	// ~0.5mA current

	// Peripherals - RTC and ADC always used
    LED_SET(LED_BLUE);  // Blue LED during startup
	CLOCK_INTOSC();     // 8 MHz
    // RTC and 'software WDT'
    RtcSwwdtReset();
    RtcStartup();
    RtcInterruptOn(4);  // Enable precise RTC, timer interrupts (and software WDT)

    // ADC
	AdcInit();
    AdcSampleWait();    // Ensure we have a valid battery level

    // Check the devices we have, put them to lowest power mode 
	// Note: On reset all interrupts are off - only RTC is running from RtcInterruptOn() above
	InitI2C(); // Once, first
    NandInitialize();
    NandVerifyDeviceId();

	AccelStandby();
    AccelVerifyDeviceId(); // Initialise later when we want to stream
	MCP3421Init(MCP3421_DEFAULT_TYPE_K);
	MCP9800Init(MCP9800_DEFAULT);

    // If we haven't detected the NAND or accelerometer this could be the wrong firmware for this device (reset to bootloader)
    if (!nandPresent || !accelPresent)
    {
        int i;
        for (i = 0; i < 5 * 3; i++) { LED_SET(LED_RED); DelayMs(111); LED_SET(LED_GREEN); DelayMs(111); if(!accelPresent){LED_SET(LED_BLUE); DelayMs(111);} }
    	Reset();                // Reset
    }

    // Read settings
    restart = 0;
    inactive = 0;
    SettingsInitialize();       // Initialize settings from ROM
	LED_SET(LED_WHITE);         // White LED during later startup
    FtlStartup();               // FTL & NAND startup
    FSInit();                   // Initialize the filesystem for reading
    SettingsReadFile(SETTINGS_FILE);// Read settings from script if present



    // Run as attached or logging
    if (USB_BUS_SENSE)
    {
        RunAttached();      // Run attached, returns when detatched
    }
    else
    {
        RunLogging();       // Run in logging mode, returns when attached
    }

	Reset();                // Reset
	return 0;
}


// Attached to USB
void RunAttached(void)
{
    // Clear the data capture buffer
    LoggerClear();

    // Initialize sensors that we may want to stream later the ADXL
    AccelStartup(settings.sampleRate);
    //AccelEnableInterrupts(ACCEL_INT_SOURCE_WATERMARK | ACCEL_INT_SOURCE_OVERRUN | ACCEL_INT_SOURCE_DOUBLE_TAP, 0x00);

    CLOCK_PLL();	// HS PLL clock for the USB module 12MIPS
    DelayMs(2); 	// Allow PLL to stabilise

    fsftlUsbDiskMounted = status.diskMounted;
    MDD_MediaInitialize();  // MDD initialize

    USBInitializeSystem(); 	// Initializes buffer, USB module SFRs and firmware

    #ifdef USB_INTERRUPT
    USBDeviceAttach();
    #endif

    while (USB_BUS_SENSE && restart != 1)
    {
        fsftlUsbDiskMounted = status.diskMounted;

        // Check bus status and service USB interrupts.
        #ifndef USB_INTERRUPT
        USBDeviceTasks(); 	// Interrupt or polling method.  If using polling, must call
        #endif
        USBProcessIO();
        if ((USBGetDeviceState() >= CONFIGURED_STATE) && (USBIsDeviceSuspended() == FALSE))
        {
            const char *line = _user_gets();
            status.attached = 1;
            if (line != NULL)
            {
                status.stream = 0;                  // Disable streaming
                SettingsCommand(line, SETTINGS_USB);
            }
        }
        else
        {
            status.attached = -1;
        }
        LedTasks();
        TimedTasks();
		
        // Stream accelerometer data
        if (status.stream)
		{
			#define STREAM_RATE 100
			#define STREAM_INTERVAL (0x10000UL / STREAM_RATE)
			static unsigned long lastSampleTicks = 0;
            unsigned long now = RtcTicks();
            if (lastSampleTicks == 0) { lastSampleTicks = now; }
            if (now - lastSampleTicks > STREAM_INTERVAL)
            {
                accel_t accelSample;
                lastSampleTicks += STREAM_INTERVAL;
                if (now - lastSampleTicks > 2 * STREAM_INTERVAL) { lastSampleTicks = now; } // not keeping up with sample rate
                
                AccelSingleSample(&accelSample);
#ifdef USE_GYRO
                if (gyroPresent)
                {
                    gyro_t gyroSample;
                    GyroSingleSample(&gyroSample);
                    printf("%d,%d,%d,%d,%d,%d\r\n", accelSample.x, accelSample.y, accelSample.z, gyroSample.x, gyroSample.y, gyroSample.z);
                }
                else
#endif
                {
                    printf("%d,%d,%d\r\n", accelSample.x, accelSample.y, accelSample.z);
                }
                USBCDCWait();
            }

        }

        // Experiment to see if this improves speed -- it doesn't seem to
        #ifdef FSFTL_READ_PREFETCH
		FsFtlPrefetch();
        #endif
    }
	#if defined(USB_INTERRUPT)
    USBDeviceDetach();
	#endif
    status.attached = -1;

    // Shutdown the FTL
    FtlShutdown();
    return;
}


// Timed tasks
void TimedTasks(void)
{
    if (lastTime != rtcTicksSeconds)
    {
        lastTime = rtcTicksSeconds;

        // Increment timer and toggle bit on overflow
        inactive = FtlIncrementInactivity();
        AdcSampleNow();
        if (adcResult.batt > BATT_CHARGE_FULL_USB && !status.batteryFull)
        {
            status.batteryFull = 1;
            if (status.initialBattery != 0 && status.initialBattery < BATT_CHARGE_MID_USB)
            {
                // Increment battery health counter
                SettingsIncrementLogValue(LOG_VALUE_BATTERY);
            }
        }

        // TODO: Change to be time-based
        if (inactive > 3)
        {
            FtlFlush(1);	// Inactivity time out on scratch hold
        }

        if (status.actionCountdown > 0)
        {
            status.actionCountdown--;
            if (status.actionCountdown == 0)
            {
                if (SettingsAction(status.actionFlags))
                {
                    restart = 1;
                }
            }
        }

        // Reset SW-WDT
        RtcSwwdtReset();
    }
    return;
}


// Led status while attached
void LedTasks(void)
{
    static unsigned int LEDTimer;
    static BOOL LEDtoggle;
//    static unsigned short inactive = 0;

    if (++LEDTimer == 0) { LEDtoggle = !LEDtoggle; }

    if (status.attached > 0)
    {
        if (status.actionCountdown)
        {
            if (((unsigned char)(LEDTimer)) < ((LEDTimer) >> 8)) { LED_SET(LEDtoggle ? LED_RED : LED_OFF); } else { LED_SET(LEDtoggle ? LED_RED : LED_OFF); }
        }
        else if (status.ledOverride >= 0)
        {
            LED_SET(status.ledOverride);
        }
        else
        {
            char c0, c1;

            if (inactive == 0)
            {
                if (status.batteryFull) { c0 = LED_OFF; c1 = LED_WHITE; }       // full - flushed
                else                    { c0 = LED_OFF; c1 = LED_YELLOW; }      // charging - flushed
            }
            else						// Red breath
            {
                if (status.batteryFull) { c0 = LED_RED; c1 = LED_WHITE; }       // full - unflushed
                else                    { c0 = LED_RED; c1 = LED_YELLOW; }      // charging - unflushed
            }
            if (((unsigned char)(LEDTimer)) < ((LEDTimer) >> 8)) { LED_SET(LEDtoggle ? c1 : c0); } else { LED_SET(LEDtoggle ? c0 : c1); }
        }
    }
    else
    {
        if (status.batteryFull) { LED_SET(LED_GREEN); }       // full - not enumerated
        else                    { LED_SET(LED_YELLOW); }      // charging - not enumarated (could change to red if yellow-green contrast not strong enough)
    }
    return;
}


