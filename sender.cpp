#include "sender.h"
#include <SPI.h>


// VSPI default pins (Receive uses HSPI bus, Sender uses VSPI bus)

//#define MISO_PIN 35  // MISO pin for sending SPI data
#define MOSI_PIN 37  // MOSI pin for sending SPI data
#define SCLK_PIN 36  // Clock pin for sending SPI data
#define SS_PIN 34    // Chip Select pin for sending data

SPIClass SPI_TX = SPIClass(SPI2);      // Default SPI class for sending HSPI/SPI3

const int BUF_SIZE = 4000;  // Define the buffer size
DRAM_ATTR RingBuffer sendBuffer(BUF_SIZE);  // Provide buffer size while creating the send buffer

extern SPISettings *spiSendSettings;


void spiSendTask(void *pvParameters) {

  Serial.printf("SEND task started.\r\n");
  Serial.flush();

  pinMode(SS_PIN, OUTPUT);  // Set SS_PIN as output
  digitalWrite(SS_PIN, HIGH);  // Set SS_PIN high to deselect at the beginning  
  SPI_TX.begin(SCLK_PIN, -1, MOSI_PIN, -1);  // Set up SPI_TX for sending
 
  
  // DEBUG - SEND ALWAYS
/*
  while (true) 
  {
    for (byte i=0;i<0xFF;i++)
    {             

      SPI_TX.beginTransaction(SPISettings(1000000, LSBFIRST, SPI_MODE0));

      // Select the SPI device by pulling SS_PIN low
      digitalWrite(SS_PIN, LOW);

      SPI_TX.transfer(i); // Send one byte              

      // Deselect the SPI device by pulling SS_PIN high
      digitalWrite(SS_PIN, HIGH);


      SPI_TX.endTransaction();  // End SPI transaction
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // Delay to prevent CPU overuse
  }  
*/

  
  digitalWrite(SS_PIN, LOW); // Select the SPI device by pulling SS_PIN low

  uint8_t sendData;
  while (true) {

    while (spiSendSettings == NULL)
    {
        Serial.println("Sender: No SEND SPI Setting!!!");
        vTaskDelay(pdMS_TO_TICKS(10000));  // Delay to prevent CPU overuse  
    }

    if (sendBuffer.isBufferEmpty()) 
    {
      vTaskDelay(pdMS_TO_TICKS(10));  // Delay to prevent CPU overuse
    }
    else    
    {        
        digitalWrite(SS_PIN, LOW); // Select the SPI device by pulling SS_PIN low
        SPI_TX.beginTransaction(*spiSendSettings);  // Start SPI transaction
  
        //while (!sendBuffer.isBufferEmpty())
        {          
          sendData = sendBuffer.readBuffer();                        
          SPI_TX.transfer(sendData); // Send one byte                
        }

        SPI_TX.endTransaction();  // End SPI transaction      
        digitalWrite(SS_PIN, HIGH);  // Deselect the SPI device by pulling SS_PIN high
    }

    
  }
  digitalWrite(SS_PIN, HIGH);  // Deselect the SPI device by pulling SS_PIN high
}
