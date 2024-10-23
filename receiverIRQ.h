#ifndef RECEIVERIRQ_H
#define RECEIVERIRQ_H

#include "RingBuffer.h"
#include "freertos/queue.h"

void irqReceiveTask(void *pvParameters);  // Task for receiving SPI data

extern RingBuffer receiveBuffer;  // Expose the receive buffer to main

#endif  // RECEIVERIRQ_H
