#ifndef RECEIVER_H
#define RECEIVER_H

#include "RingBuffer.h"

void spiReceiveTask(void *pvParameters);  // Task for receiving SPI data

extern RingBuffer receiveBuffer;  // Expose the receive buffer to main

#endif  // RECEIVER_H
