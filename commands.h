#ifndef COMMANDS_H
#define COMMANDS_H



// Define this to enable TASK command after you have fixed FreeRTOSConfig.h
//#define TASK_COMMAND 1


/*
  MANUAL things to do to get tracing to work so "TASK" will work

  Edit Arduino file FreeRTOSConfig.h: Make sure that the following settings are enabled in your FreeRTOSConfig.h:

  #define configUSE_TRACE_FACILITY 1
  #define configUSE_STATS_FORMATTING_FUNCTIONS 1

  Arduino Core Path: hardware/espressif/esp32/tools/sdk/include/freertos/FreeRTOSConfig.h

*/

#include <Arduino.h>
#include <SPI.h>  // Include SPI library for SPISettings
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "RingBuffer.h"

// Typedef for the command handler function pointer
typedef void (*CommandHandler)(const char* args);

// Struct to represent a command
struct Command {
  const char* name;
  const char* helpText;
  CommandHandler handler;
};



// Enum to represent the available baud rates
enum BaudRate {
  BAUD_9600,
  BAUD_19200,
  BAUD_38400,
  BAUD_307200,
  BAUD_983000,
  BAUD_INVALID
};

// Struct to hold SPI settings and their associated baud rate
struct BaudRateConfig {
  uint32_t baudRate;
  SPISettings spiSettings;
};


// Function prototypes for command handlers
void handleHelp(const char* args);
void handleSend(const char* args);
void handleBaud(const char* args);
void handleLoopback(const char* args);
void handleEcho(const char* args);
void handleDump(const char* args);
void handleClear(const char* args);
void handleSendDebug(const char* args);
void handleHDLC(const char* args);
void handleSendHDLCFrame(const char* args);


#if TASK_COMMAND
void handleTask(const char* args);
#endif


// Declare functions for baud rate handling
BaudRate findBaudRate(uint32_t baudRate);
void setClockSpeed(BaudRate selectedBaud);

void SetDefaultSendBaudRate();

// Declare global variables
extern uint32_t currentBaudRate;
extern const uint32_t maxBaudRate;
extern bool loopbackEnabled;
extern bool receiveToSerialEnabled;
extern bool hdlcMode;
extern SPISettings *spiSendSettings;

// Function to process incoming commands
void processCommand(char* input);

extern RingBuffer receiveBuffer;  // Expose the receive buffer to main

#endif  // COMMANDS_H
