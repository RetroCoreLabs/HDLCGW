#ifndef CONSOLE_H
#define CONSOLE_H

#include "RingBuffer.h"

void consoleTask(void *pvParameters);  // Task for receiving SPI data

extern RingBuffer receiveBuffer;  // Expose the receive buffer to main


void showPrompt();
void updatePrompt();
void handleSerialInput();
void handleReceiveBufffer();

#endif  // RECEICONSOLE_HVERIRQ_H
