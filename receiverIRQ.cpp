#include "receiverI2S.h"
#include "driver/i2s.h"
#include <string.h>
#include "driver/gpio.h"  // Include this to access low-level GPIO functions


const int BUF_SIZE = 4000;  // Define the buffer size
//RingBuffer receiveBuffer(BUF_SIZE);  // Provide buffer size while creating the send buffer


// Pin definitions
#define DATA_PIN    35  // GPIO for Data pin (adjust to your wiring)
#define CLOCK_PIN     18  // GPIO for Clock pin (adjust to your wiring)



DRAM_ATTR volatile uint8_t bitCount = 0;  // Counter for the number of bits captured
DRAM_ATTR volatile uint8_t byteData = 0;  // Variable to store the byte being assembled
DRAM_ATTR volatile bool byteReady = false; // Flag to indicate when a byte is ready

int byteCount = 0;


// We have not woken a task at the start of the ISR.
BaseType_t  xHigherPriorityTaskWoken = pdFALSE;
#define QUEUE_SIZE 128
QueueHandle_t byteQueue;  // FreeRTOS queue to store received bytes


// Interrupt Service Routine (ISR) for clock rising edge
void IRAM_ATTR onClockRisingEdge_oneBit() 
{      
     // Read the current bit from the data pin
    int data_bit = digitalRead(DATA_PIN);

    // Read the current bit from the data pin (use direct GPIO read for efficiency)
    //int data_bit = (GPIO.in & (1 << DATA_PIN)) ? 1 : 0;


    // Shift the bit into the byte data
    //byteData = (byteData << 1) | (data_bit & 0x01);
    byteData >>=1; // shift to the right
    if ((data_bit & 0x01)!=0)
    {
        byteData |= 0x80;
    }
     

    // Increment bit count
    bitCount++;

    // If 8 bits have been captured, set the byte ready flag
    if (bitCount >= 8) {
        byteReady = true;
        byteCount++;
        bitCount = 0;  // Reset bit count for the next byte

        // Send the byte to the queue from the ISR
#if true        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        char cdata = (char)byteData;
        xQueueSendFromISR(byteQueue, &cdata, &xHigherPriorityTaskWoken);
#else
        receiveBuffer.writeBuffer(byteData);    
#endif

        byteData = 0;
    }
}


void IRAM_ATTR onClockRisingEdge_read_8_bits() {
    byteData = 0;  // Clear previous byte data
    for (int i = 0; i < 8; i++) {
        // Wait for the rising edge of the clock
        while (digitalRead(CLOCK_PIN) == LOW) delayMicroseconds(5);
        while (digitalRead(CLOCK_PIN) == HIGH) delayMicroseconds(5);  // Ensure complete cycle

        // Read the current bit from the data pin
        int data_bit = digitalRead(DATA_PIN);

        // Shift the bit into the byte data
        byteData = (byteData << 1) | (data_bit & 0x01);
    }
    //receiveBuffer.writeBuffer(byteData);   
    char cdata = (char)byteData;
    xQueueSendFromISR(byteQueue, &cdata, &xHigherPriorityTaskWoken);
    byteReady = true;  // Indicate that the byte is ready
}


// Set up the receicing logic
void receive_irq_setup() 
{
 // Set up clock pin as input with interrupt on rising edge
  pinMode(CLOCK_PIN, INPUT);
  //attachInterrupt(digitalPinToInterrupt(CLOCK_PIN), onClockRisingEdge_read_8_bits, RISING);

  attachInterrupt(digitalPinToInterrupt(CLOCK_PIN), onClockRisingEdge_oneBit, RISING);

  // Set up data pin as input
  pinMode(DATA_PIN, INPUT);

 // Create a FreeRTOS queue to hold received bytes
  byteQueue = xQueueCreate(QUEUE_SIZE, sizeof(uint8_t));


  Serial.printf("IRQ Slave initialized successfully\r\n");
  Serial.flush();
}


void irqTask()
{
  Serial.printf("RECEIVE IRQ task started.\r\n");  
  Serial.flush();

  vTaskDelay(pdMS_TO_TICKS(1000)); // 10ms delay
  receive_irq_setup();  

  while (true) 
  {

    uint8_t receivedByte;

    // Check if a byte is available in the queue
    if (xQueueReceive(byteQueue, &receivedByte, portMAX_DELAY) == pdPASS) {
        // Process the received byte (print it for demonstration)
         receiveBuffer.writeBuffer(receivedByte);   
    }

    //capture_data_dma();
    //capture_data_without_dma();
    vTaskDelay(pdMS_TO_TICKS(1)); // 10ms delay

    //try_receive();
    //try_receive_simple();    
  }
}


void loopTask()
{
  Serial.printf("RECEIVE NoIRQ task started.\r\n");  
  Serial.flush();

  while (true) 
  {
    byteData = 0;  // Clear previous byte data
    for (int i = 0; i < 8; i++) {
        // Wait for the rising edge of the clock
        while (digitalRead(CLOCK_PIN) == LOW);
        while (digitalRead(CLOCK_PIN) == HIGH);  // Ensure complete cycle

        // Read the current bit from the data pin
        int data_bit = digitalRead(DATA_PIN);

        // Shift the bit into the byte data
        byteData = (byteData << 1) | (data_bit & 0x01);
    }
    receiveBuffer.writeBuffer(byteData);   
  }
}

void irqReceiveTask(void *pvParameters) 
{
  //loopTask();
  irqTask();
}

