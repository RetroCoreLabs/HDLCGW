#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // For mutex


#define SIZE 4000  // Define size for the buffer

class RingBuffer {
public:

  RingBuffer(int size);  // Constructor that accepts buffer size
  ~RingBuffer();         // Destructor to free dynamically allocated memory

  void writeBuffer(unsigned char data);
  unsigned char readBuffer();
  void initBuffer();
  void Clear();
  
  // Getter functions
  bool isBufferEmpty() const;
  bool isBufferFull() const;
  int Length() const;  // Length getter to return the number of bytes in the buffer
    

private:
  unsigned char* buffer;  // Dynamically allocated buffer
  volatile int inPtr, outPtr;
  volatile int bufferEmpty = 1;
  volatile int bufferFull = 0;
  int bufferSize;         // Size of the buffer

  SemaphoreHandle_t mutex;  // Mutex for thread safety
};

#endif  // RINGBUFFER_H
