#include <Arduino.h>
#include "commands.h"
#include "sender.h"
#include "receiver.h"
#include "receiverI2S.h"

#include "receiverIRQ.h"
#include "console.h"

// Global variables for this module
QueueHandle_t notifyQueue;


// Made for the ESP32 S2 Mini
// https://done.land/components/microcontroller/families/esp/esp32/s2/s2mini

// Ref https://github.com/MattisLind/alfaskop_emu/blob/master/Utils/SDLCBridge/SDLCBridge.ino

void setup() 
{
  // Open Serial port on USB
  Serial.begin(115200);  

  // Wait for the serial port to open
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port on some boards.
  }

  // Send a hello message once the serial port is opened
  Serial.println("HDLC GW version 0.0.2");
  Serial.flush();
  
  // Set Default baud for SPI send
  SetDefaultSendBaudRate();

  // Create the read and write tasks
  notifyQueue = xQueueCreate(10, sizeof(uint8_t));


    xTaskCreatePinnedToCore(
    consoleTask,      // Task function
    "Console Task",  // Name of the task (optional, used for debugging)
    8192,             // Stack size in words (not bytes)
    NULL,             // Pointer to a parameter to pass to the task (if needed)
    2,                // Task priority
    NULL,             // Task handle (optional, for reference to the created task)
    0                 // Core where the task should run (0 or 1) (tskNO_AFFINITY)
  );


/*
  xTaskCreate(
    spiReceiveTask,  // Task function
    "SPI Receive Task",  // Name of the task (optional, used for debugging)
    8192,  // Stack size in words (not bytes)
    NULL,  // Pointer to a parameter to pass to the task (if needed)
    1,  // Task priority
    NULL  // Task handle (optional, for reference to the created task)
  );
*/


/*
  xTaskCreatePinnedToCore(
    i2sReceiveTask,  // Task function
    "I2S Receive Task",  // Name of the task (optional, used for debugging)
    8192,  // Stack size in words (not bytes)
    NULL,  // Pointer to a parameter to pass to the task (if needed)
    1,  // Task priority
    NULL,  // Task handle (optional, for reference to the created task)
    1      // Core where the task should run (0 or 1) (tskNO_AFFINITY)
  );
*/



  xTaskCreatePinnedToCore(
    irqReceiveTask,  // Task function
    "IRQ Receive Task",  // Name of the task (optional, used for debugging)
    8192,  // Stack size in words (not bytes)
    NULL,  // Pointer to a parameter to pass to the task (if needed)
    10,  // Task priority
    NULL,  // Task handle (optional, for reference to the created task)
    1      // Core where the task should run (0 or 1) (tskNO_AFFINITY)
  );



  xTaskCreatePinnedToCore(
    spiSendTask,      // Task function
    "SPI Send Task",  // Name of the task (optional, used for debugging)
    8192,             // Stack size in words (not bytes)
    NULL,             // Pointer to a parameter to pass to the task (if needed)
    8,                // Task priority
    NULL,             // Task handle (optional, for reference to the created task)
    0                 // Core where the task should run (0 or 1) (tskNO_AFFINITY)
  );

  showPrompt();          // Show the initial prompt
}




// Mainloop
void loop() {


}

