﻿#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include "epoll_timerfd_utilities.h"

#include <applibs/gpio.h>
#include <applibs/log.h>

#include "mt3620_rdb.h"
#include <OLEDlib.h>


// This sample C application for the MT3620 Reference Development Board (Azure Sphere)
// blinks an LED.
// The blink rate can be changed through a button press.
//
// It uses the API for the following Azure Sphere application libraries:
// - gpio (digital input for button)
// - log (messages shown in Visual Studio's Device Output window during debugging)

// File descriptors - initialized to invalid value
static int ledBlinkRateButtonGpioFd = -1;
static int buttonPollTimerFd = -1;
static int blinkingLedGpioFd = -1;
static int blinkingLedTimerFd = -1;
static int i2cFd = -1;
static int epollFd = -1;

// Button state variables
static GPIO_Value_Type buttonState = GPIO_Value_High;
static GPIO_Value_Type ledState = GPIO_Value_High;

// Blink interval variables
static const int numBlinkIntervals = 3;
static const struct timespec blinkIntervals[] = {{0, 125000000}, {0, 250000000}, {0, 500000000}};
static int blinkIntervalIndex = 0;

// Termination state
static volatile sig_atomic_t terminationRequired = false;

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    terminationRequired = true;
}

/// <summary>
///     Handle LED timer event: blink LED.
/// </summary>
static void BlinkingLedTimerEventHandler(EventData *eventData)
{
    if (ConsumeTimerFdEvent(blinkingLedTimerFd) != 0) {
        terminationRequired = true;
        return;
    }

    // The blink interval has elapsed, so toggle the LED state
    // The LED is active-low so GPIO_Value_Low is on and GPIO_Value_High is off
    ledState = (ledState == GPIO_Value_Low ? GPIO_Value_High : GPIO_Value_Low);
    int result = GPIO_SetValue(blinkingLedGpioFd, ledState);
    if (result != 0) {
        Log_Debug("ERROR: Could not set LED output value: %s (%d).\n", strerror(errno), errno);
        terminationRequired = true;
    }
}

/// <summary>
///     Handle button timer event: if the button is pressed, change the LED blink rate.
/// </summary>
static void ButtonTimerEventHandler(EventData *eventData)
{
    if (ConsumeTimerFdEvent(buttonPollTimerFd) != 0) {
        terminationRequired = true;
        return;
    }

    // Check for a button press
    GPIO_Value_Type newButtonState;
    int result = GPIO_GetValue(ledBlinkRateButtonGpioFd, &newButtonState);
    if (result != 0) {
        Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
        terminationRequired = true;
        return;
    }

    // If the button has just been pressed, change the LED blink interval
    // The button has GPIO_Value_Low when pressed and GPIO_Value_High when released
    if (newButtonState != buttonState) {
        if (newButtonState == GPIO_Value_Low) {
			OLED_ClearDisplay();
			OLED_SetTextPos(1, 1);
			OLED_PutString("Hello World!");
			OLED_SetTextPos(2, 2);
			OLED_PutString("Hello World!");
			OLED_SetTextPos(3, 3);
			OLED_PutString("Hello World!");
			OLED_SetTextPos(4, 4);
			OLED_PutString("Hello World!");
			OLED_ClearPos(7,3,5);

			OLED_SetVerticalScrollProperties(SCROLL_VERTICAL_LEFT, 3, 6, SCROLL_PER_25_FRAMES, 1);
			OLED_ActivateScroll();

			//OLED_DeactivateScroll();
			blinkIntervalIndex = (blinkIntervalIndex + 1) % numBlinkIntervals;
            if (SetTimerFdToPeriod(blinkingLedTimerFd, &blinkIntervals[blinkIntervalIndex]) != 0) {
                terminationRequired = true;
            }
        }
        buttonState = newButtonState;
    }
}

// event handler data structures. Only the event handler field needs to be populated.
static EventData buttonEventData = {.eventHandler = &ButtonTimerEventHandler};
static EventData blinkingLedTimerEventData = {.eventHandler = &BlinkingLedTimerEventHandler};

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }
	
    // Open button GPIO as input, and set up a timer to poll it
    Log_Debug("Opening MT3620_RDB_BUTTON_A as input.\n");
    ledBlinkRateButtonGpioFd = GPIO_OpenAsInput(MT3620_RDB_BUTTON_A);
    if (ledBlinkRateButtonGpioFd < 0) {
        Log_Debug("ERROR: Could not open button GPIO: %s (%d).\n", strerror(errno), errno);
        return -1;
    }
    struct timespec buttonPressCheckPeriod = {0, 1000000};
    buttonPollTimerFd =
        CreateTimerFdAndAddToEpoll(epollFd, &buttonPressCheckPeriod, &buttonEventData, EPOLLIN);
    if (buttonPollTimerFd < 0) {
        return -1;
    }

    // Open LED GPIO, set as output with value GPIO_Value_High (off), and set up a timer to poll it
    Log_Debug("Opening MT3620_RDB_LED1_RED.\n(not really, opening MT3620_GPIO0)\n");
    blinkingLedGpioFd =
        GPIO_OpenAsOutput(MT3620_GPIO0, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (blinkingLedGpioFd < 0) {
        Log_Debug("ERROR: Could not open LED GPIO: %s (%d).\n", strerror(errno), errno);
        return -1;
    }
    blinkingLedTimerFd = CreateTimerFdAndAddToEpoll(epollFd, &blinkIntervals[blinkIntervalIndex],
                                                    &blinkingLedTimerEventData, EPOLLIN);
    if (blinkingLedTimerFd < 0) {
        return -1;
    }


	i2cFd = I2CMaster_Open(MT3620_I2C_ISU2);
	if (i2cFd < 0) {
		return -1;
	}
	I2CMaster_SetBusSpeed(i2cFd,I2C_BUS_SPEED_HIGH );

	OLED_Init(i2cFd, true);
	OLED_SetTextPos(0,3);
	OLED_PutString("Display checked!");
	OLED_SetInverseDisplay();
	struct timespec waitPeriod = { 0, 250 * 1000 * 1000 };
	nanosleep(&waitPeriod, NULL);
	OLED_SetNormalDisplay();

    return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
    // Leave the LED off
    if (blinkingLedGpioFd >= 0) {
        GPIO_SetValue(blinkingLedGpioFd, GPIO_Value_High);
    }

    Log_Debug("Closing file descriptors.\n");
    CloseFdAndPrintError(blinkingLedTimerFd, "BlinkingLedTimer");
    CloseFdAndPrintError(blinkingLedGpioFd, "BlinkingLedGpio");
    CloseFdAndPrintError(buttonPollTimerFd, "ButtonPollTimer");
    CloseFdAndPrintError(ledBlinkRateButtonGpioFd, "LedBlinkRateButtonGpio");
    CloseFdAndPrintError(epollFd, "Epoll");
}

/// <summary>
///     Main entry point for this application.
/// </summary>
int main(int argc, char *argv[])
{
    Log_Debug("Blink application starting.\n");
    if (InitPeripheralsAndHandlers() != 0) {
        terminationRequired = true;
    }

    // Use epoll to wait for events and trigger handlers, until an error or SIGTERM happens
    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }
    }

    ClosePeripheralsAndHandlers();
    Log_Debug("Application exiting.\n");
    return 0;
}
