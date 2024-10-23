#include "RingBuffer.h"


// Constructor: Initialize the mutex and buffer with the given size
RingBuffer::RingBuffer(int size) 
{
  bufferSize = size;
  buffer = new unsigned char[bufferSize];  // Dynamically allocate the buffer
  mutex = xSemaphoreCreateMutex();         // Create the mutex
  initBuffer();
}

// Destructor: Delete the buffer and the mutex
RingBuffer::~RingBuffer() 
{
  delete[] buffer;             // Free the dynamically allocated buffer
  vSemaphoreDelete(mutex);     // Delete the mutex to free resources
}


// Write data to the buffer
void RingBuffer::writeBuffer(unsigned char data) 
{
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {  // Acquire the mutex  
    if (!bufferFull)
    {
      buffer[inPtr] = data;
      bufferEmpty = 0;
      inPtr++;
      if (inPtr == bufferSize) {
        inPtr = 0;
      }
      if (inPtr == outPtr) {
        bufferFull = 1;
      } else {
        bufferFull = 0;
      }
    }
    xSemaphoreGive(mutex);  // Release the mutex
  }
}


// Read data from the buffer
unsigned char RingBuffer::readBuffer() 
{
  unsigned char out = 0;

  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {  // Acquire the mutex

    if (!bufferEmpty) {
      out = buffer[outPtr];
      bufferFull = 0;
      outPtr++;
      if (outPtr == bufferSize) {
        outPtr = 0;
      }
      if (inPtr == outPtr) {
        bufferEmpty = 1;
      } else {
        bufferEmpty = 0;
      }
    }

    xSemaphoreGive(mutex);  // Release the mutex
  }

  return out;
}

// Initialize the buffer
void RingBuffer::initBuffer() 
{
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {  // Acquire the mutex

    bufferEmpty = 1;
    bufferFull = 0;
    inPtr = 0;
    outPtr = 0;

    xSemaphoreGive(mutex);  // Release the mutex
  }
}

void RingBuffer::Clear() 
{
  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE)  // Acquire the mutex
  {  
    bufferEmpty = 1;
    bufferFull = 0;
    inPtr = 0;
    outPtr = 0;
    xSemaphoreGive(mutex);  // Release the mutex
  }
}

bool RingBuffer::isBufferEmpty() const 
{
  return bufferEmpty;
}

bool RingBuffer::isBufferFull() const 
{
  return bufferFull;
}

// Get the number of bytes in the buffer (Length)
int RingBuffer::Length() const 
{
  int length = 0;

  if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {  // Acquire the mutex

    if (bufferFull) {
      length = bufferSize;
    } else if (inPtr >= outPtr) {
      length = inPtr - outPtr;
    } else {
      length = bufferSize - outPtr + inPtr;
    }

    xSemaphoreGive(mutex);  // Release the mutex
  }

  return length;
}