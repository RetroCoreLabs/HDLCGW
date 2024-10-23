#ifndef SENDER_H
#define SENDER_H

#include "RingBuffer.h"

void spiSendTask(void *pvParameters);  // Task for sending SPI data

extern RingBuffer sendBuffer;  // Expose the send buffer to main

#endif  // SENDER_H