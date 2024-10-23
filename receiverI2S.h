#ifndef RECEIVERI2S_H
#define RECEIVERI2S_H

#include "RingBuffer.h"

void i2sReceiveTask(void *pvParameters);  // Task for receiving SPI data

extern RingBuffer receiveBuffer;  // Expose the receive buffer to main

#endif  // RECEIVERI2S_H
