#include "commands.h"
#include "receiverI2S.h"
#include "driver/i2s.h"
#include <string.h>


const int BUF_SIZE = 4000;  // Define the buffer size
//RingBuffer receiveBuffer(BUF_SIZE);  // Provide buffer size while creating the send buffer


// Pin definitions
#define I2S_DATA_PIN    35  // GPIO for Data pin (adjust to your wiring)
#define I2S_CLK_PIN     18  // GPIO for Clock pin (adjust to your wiring)



// Buffer to hold received data
uint8_t i2s_read_buffer[1];

//    receiveBuffer.writeBuffer(received_byte);    


// Function to capture data with DMA
void capture_data_dma() {
    size_t bytes_read;

    // Continuously read data into the buffer
    esp_err_t result = i2s_read(I2S_NUM_0, (void *)i2s_read_buffer, sizeof(i2s_read_buffer), &bytes_read, portMAX_DELAY);
    
    uint8_t sample = i2s_read_buffer[0];

     if (result == ESP_OK) 
      {    
          // Process the received sample (in this case, print it)       
        Serial.printf("I2S DMA Data received: 0x%02X\n", sample);    
        Serial.flush();

        receiveBuffer.writeBuffer(sample);    
      } 
      else if (result == ESP_ERR_TIMEOUT) 
      {
        Serial.printf("T.");
      } 
      else 
      {        
         Serial.printf("Error dma I2S: [%d] %s\n",result, esp_err_to_name(result));
      }



    if (result != ESP_OK)
    {      
        vTaskDelay(pdMS_TO_TICKS(1)); // 1ms delay}
    }

    // Process the received data here
    // You can implement your HDLC protocol handler to parse bits/bytes
}

// Function to capture data without DMA (polling)
void capture_data_without_dma() {
    uint8_t sample;
    size_t bytes_read;
    
    // Continuously read data by polling I2S
    
      // Read a single 8-bit sample from I2S
      esp_err_t result = i2s_read(I2S_NUM_0, &sample, 1, &bytes_read, 10000);


      if (result == ESP_OK) 
      {    
        if (bytes_read>0)
        {
          // Process the received sample (in this case, print it)       
          Serial.printf("I2S Data received: 0x%02X\r\n", sample);    
          Serial.flush();
          receiveBuffer.writeBuffer(sample);    
        }
      } 
      else if (result == ESP_ERR_TIMEOUT) 
      {
        Serial.print("T");
      } 
      else 
      {
        Serial.printf("Error I2S: [%d] %s\n",result, esp_err_to_name(result));
      }


      if (result != ESP_OK)
      {
        //Serial.printf("i2s_read() timeout");    
        vTaskDelay(pdMS_TO_TICKS(1)); // 1ms delay}
      }

}


// Set up the receicing logic
void receive_i2s_setup() 
{

  // I2S configuration
  i2s_config_t i2s_config = {    
    .mode = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX),  // Receive data, slave mode (external clock)
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_8BIT, // 16-bit data size (adjust as needed)
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Single channel (left channel for data) //I2S_CHANNEL_FMT_RIGHT_LEFT
    .communication_format = I2S_COMM_FORMAT_PCM, // MSB first  // I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB,
    //.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,  // Interrupt level 1 (not used here..)
    .intr_alloc_flags = 0,  // default interrupt priority
    .dma_buf_count = 2,  // Number of DMA buffers (minimal..)
    .dma_buf_len = 64, // Length of each DMA buffer (in bytes) 
    .use_apll = false,
    .tx_desc_auto_clear = false,  // Not using TX
    .fixed_mclk = 0
  };

  // I2S pin configuration
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_CLK_PIN,      // Clock pin (externally provided)
    .ws_io_num = I2S_PIN_NO_CHANGE,       // Word select pin (use clock for both)
    .data_out_num = I2S_PIN_NO_CHANGE,  // Not transmitting
    .data_in_num = I2S_DATA_PIN     // Data input pin
  };


  Serial.printf("I2S Installing driver\r\n");  
  Serial.flush();

  // Install I2S driver
  esp_err_t result = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  if (result != ESP_OK) 
  {
    Serial.printf("i2s_driver_install() failed: [%d] %s\n",result, esp_err_to_name(result));
  }

  Serial.printf("I2S Set pin\r\n");  
  Serial.flush();

  // Set I2S pin configuration
  result = i2s_set_pin(I2S_NUM_0, &pin_config);
  if (result !=ESP_OK) 
  {
    Serial.printf("i2s_set_pin() failed: [%d] %s\n",result, esp_err_to_name(result));
  }

  Serial.printf("I2S Slave initialized successfully\r\n");
  Serial.flush();
}


void i2sReceiveTask(void *pvParameters) 
{
  Serial.printf("RECEIVE I2S task started.\r\n");  
  Serial.flush();

  Serial.printf("Free heap size: %d\n", esp_get_free_heap_size());
  Serial.flush();

  vTaskDelay(pdMS_TO_TICKS(1000)); // 10ms delay
  receive_i2s_setup();  

  while (true) 
  {

    //capture_data_dma();
    capture_data_without_dma();
    //vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay

    //try_receive();
    //try_receive_simple();    
  }
}

