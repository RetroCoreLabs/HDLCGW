#include "receiver.h"
#include <string.h>
#include "esp_intr_alloc.h"
#include "esp_log.h"

//#include "hal/spi_struct.h"

#include <SPI.h>
#include "driver/spi_slave.h"
#include "driver/spi_common.h"

// Ref STM32 SPI doc
// https://github.com/rogerclarkmelbourne/Arduino_STM32/blob/master/STM32F1/libraries/SPI/src/SPI.cpp

//#include "esp_log.h"
#include <soc/spi_struct.h>
#include <soc/periph_defs.h>
// spi_dev_t
// C:\Users\ronny\AppData\Local\Arduino15\packages\esp32\tools\esp32-arduino-libs\idf-release_v5.1-33fbade6\esp32s2\include\soc\esp32s2\include\soc\spi_struct.h

// https://asf.microchip.com/docs/latest/common.services.basic.spi.master_example.evk1104/html/avr32_drivers_spi_quick_start.html

//#include "hal/spi_ll.h" 


// https://done.land/components/microcontroller/families/esp/esp32/s2/s2mini#hardware-spi-interface
#define MISO_PIN 9 // MISO pin for receiving SPI data  
#define MOSI_PIN 11  // MOSI pin for sending SPI data  <== OR WTF receivng
#define SS_PIN 12    // Chip Select pin for SPI 
//#define SS_PIN -1    // Chip Select pin for SPI 
#define SCLK_PIN 7  // Clock pin for SPI data 


#define RCV_HOST    SPI2_HOST //SPI2_HOST
//#define RCV_HOST    HSPI_HOST  

//#define RCV_DMA SPI_DMA_CH_AUTO
#define RCV_DMA 0

const int BUF_SIZE = 4000;  // Define the buffer size
DRAM_ATTR RingBuffer receiveBuffer(BUF_SIZE);  // Provide buffer size while creating the send buffer

// Buffer for SPI to store data in
//uint8_t recv_buf[1];  // Adjust size according to the expected data length. DMA must be used if buffer size is larger then 32 bytes

#define TRANSACTION_TIMEOUT 10000  // portMAX_DELAY
#define NUM_TRANSACTIONS 10
spi_slave_transaction_t trans[NUM_TRANSACTIONS];
uint8_t recv_buf[NUM_TRANSACTIONS][1];  // Buffers for each transaction



// This function will handle received data in the SPI transaction
void IRAM_ATTR spi_post_trans_cb(spi_slave_transaction_t *t) {
    // Insert received byte into the ring buffer
    uint8_t received_byte = ((uint8_t *)t->rx_buffer)[0];
    receiveBuffer.writeBuffer(received_byte);    

    // Re-queue the transaction for the next byte
    esp_err_t ret = spi_slave_queue_trans(RCV_HOST, t, TRANSACTION_TIMEOUT);
}


// Set up the receicing logic
void receive_spi_setup() 
{
    // Configure the SPI bus for the slave
    Serial.printf("Configure the SPI bus for the slave\r\n");

    pinMode(SS_PIN, INPUT);  // Set SS_PIN as input
    pinMode(MISO_PIN, INPUT);  // Set MISO_PIN as input
    pinMode(SCLK_PIN, INPUT);  // Set SCLK_PIN as input
    
    spi_bus_config_t buscfg = {        
        .mosi_io_num = MOSI_PIN,
        .miso_io_num = MISO_PIN,
        .sclk_io_num = SCLK_PIN,
        .quadwp_io_num = -1,  // Not used
        .quadhd_io_num = -1   // Not used
    };
    
    
    spi_slave_interface_config_t slvcfg = 
    {
        .spics_io_num = SS_PIN,  // Chip select pin (SS)
        //.spics_io_num = -1,  // Chip select pin (SS)
        .flags = SPICOMMON_BUSFLAG_SLAVE,
        .queue_size = 10,  // Multiple transactions
        .mode = 0,        // SPI Mode 0: CPOL=0, CPHA=0
        .post_setup_cb = NULL,
        .post_trans_cb = spi_post_trans_cb  // Callback when transaction completes        
    };

    esp_err_t ret = spi_slave_initialize(RCV_HOST, &buscfg, &slvcfg, RCV_DMA);
    if (ret != ESP_OK) {
        Serial.printf("Failed to initialize SPI slave: %s\n", esp_err_to_name(ret));
        return;
    }

    Serial.printf("SPI Slave initialized successfully\r\n");
}



void receive_spi_setup_nocs() 
{    
  spi_bus_config_t buscfg1 = 
  {        
    .mosi_io_num = MOSI_PIN,
    .miso_io_num = MISO_PIN,
    .sclk_io_num = SCLK_PIN,
    .quadwp_io_num = -1,  // Not used
    .quadhd_io_num = -1   // Not used
  };
  
    
  spi_slave_interface_config_t slvcfg1 = 
  {        
      .spics_io_num = SS_PIN,  // Chip select pin (SS)
      //.spics_io_num = -1,  // NO Chip select pin (SS)
      .flags = SPICOMMON_BUSFLAG_SLAVE,
      .queue_size = 10,  // Multiple transactions
      .mode = 0,        // SPI Mode 0: CPOL=0, CPHA=0
      .post_setup_cb = NULL,
      .post_trans_cb = NULL  // Callback when transaction completes   
      //.post_trans_cb = spi_post_trans_cb  // Callback when transaction completes             
  };

    // Initialize the SPI bus and SPI slave     
    esp_err_t ret = spi_slave_initialize(RCV_HOST, &buscfg1, &slvcfg1, RCV_DMA);
    if (ret != ESP_OK) {
        Serial.printf("Failed to initialize SPI slave: %s\n", esp_err_to_name(ret));
        return;
    }

    Serial.printf("SPI Slave NOCS initialized successfully\r\n");
}

void queue_transactions() 
{

  // Set transaction details
   for (int i = 0; i < NUM_TRANSACTIONS; i++) {
        memset(&trans[i], 0, sizeof(spi_slave_transaction_t));
        trans[i].length = 8;  // 1 byte per transaction (8 bits)
        trans[i].rx_buffer = recv_buf[i];  // Assign individual buffers
    }


  // Queue for receive
    for (int i = 0; i < NUM_TRANSACTIONS; i++) {
        // Queue each transaction
        esp_err_t ret = spi_slave_queue_trans(RCV_HOST, &trans[i], portMAX_DELAY);
        if (ret != ESP_OK) {
            Serial.printf("Error queuing transaction %d: %d\n", i, ret);
        }
    }
}

spi_dev_t* SPI_LL_GET_HW(int ID)
{
 // ((ID)==0? ({abort();NULL;}):((ID)==1? &GPSPI2 : &GPSPI3))

  if (ID ==0) abort();
  //if (ID == 1) return &GPSPI2;
  return &GPSPI3;
} 
DRAM_ATTR void try_receive()
{

  //TODO: Manually activate Chip Select, then in the end manually deactivate - to be able to receive a continous stream of DATA + CLOCK

  // Initialize empty transacrion structt
  spi_slave_transaction_t t;
  memset(&t, 0, sizeof(t));  // Clear the transaction structure
  t.length = 8 * sizeof(recv_buf);  // Transaction length in bits 
  t.rx_buffer = recv_buf;  // Buffer to hold received data

  // Perform the transaction and wait for it to complete
  esp_err_t ret = spi_slave_transmit(RCV_HOST, &t, 10000);

  if (ret != ESP_OK) 
  {
    //Serial.printf("spi_slave_transmit() Failed: %s\r\n", esp_err_to_name(ret));
    // Add delay to avoid CPU overuse
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
  }
}

void spi_receive_no_transactions(spi_dev_t *spi) {
    // Continuously check for received data
    while (1) {
        // Check if there's data in the RX FIFO
        int rx_fifo_len = spi->dma_in_status.in_fifo_cnt;
        
        // If RX FIFO has data, read it
        if (rx_fifo_len > 0) {
            // Read received data from the data buffer
            for (int i = 0; i < rx_fifo_len; i++) {
                uint8_t received_byte = spi->data_buf[i] & 0xFF;  // Read the received byte
                printf("Received byte: 0x%02X\n", received_byte);

                // Do whatever processing you need with the received byte
                 receiveBuffer.writeBuffer(received_byte);    
            }            
        

          // Clear the FIFO after reading all bytes
          spi->dma_conf.in_rst = 1;  // Reset the input FIFO
          spi->dma_conf.in_rst = 0;  // Clear the reset flag to resume operation
        }

        // Small delay to prevent CPU overuse (optional)
        vTaskDelay(pdMS_TO_TICKS(1));  // Adjust the delay as needed
    }
}



// Write - spi_tx_reg(SPI2, 0xFF);
void spi_tx_reg(spi_dev_t* spi, uint32_t data) 
{
      // Write the data to the transmit buffer
    spi->data_buf[0] = data;
    
    // Trigger the SPI transaction by setting the user-defined command bit
    spi->cmd.usr = 1;
}



// Enable SPI RX interrupts
void enable_spi_interrupt(spi_dev_t *spi) {
    spi->dma_int_ena.in_suc_eof = 1;  // Enable RX interrupt
 //   esp_intr_enable(ETS_SPI2_INTR_SOURCE);  // Enable ESP32 interrupt for SPI
}

// Disable SPI RX interrupts
void disable_spi_interrupt(spi_dev_t *spi) {
    spi->dma_int_ena.in_suc_eof = 0;  // Disable RX interrupt
    //esp_intr_disable(ETS_SPI2_INTR_SOURCE);  // Disable ESP32 interrupt for SPI
}


// Clear the RX FIFO
void clear_rx_fifo(spi_dev_t *spi) {
    spi->dma_conf.in_rst = 1;  // Reset RX FIFO
    spi->dma_conf.in_rst = 0;  // Clear the reset
}

bool spi_is_rx_nonempty(spi_dev_t *spi) 
{
  // Check if the RX FIFO is not empty (dma_in_status.in_fifo_empty)
  return !(spi->dma_in_status.in_fifo_empty);
}
bool spi_is_tx_empty(spi_dev_t *spi) 
{
  // Check if the transmit FIFO is empty
  return (spi->dma_out_status.out_fifo_empty);  
}

uint32_t spi_rx_reg(spi_dev_t *spi) {
    if (!spi_is_rx_nonempty(spi)) {
        return 0;
    }
    uint32_t data = spi->data_buf[0];  // Read from RX FIFO
    clear_rx_fifo(spi);  // Clear FIFO after reading
    return data;
}

// Define the interrupt handler
void IRAM_ATTR spi_isr_handler(void* arg) {
    // Handle the interrupt here
    spi_dev_t* spi = (spi_dev_t*)arg;
    
    // Example: Read data from the RX FIFO
    if (!spi->dma_in_status.in_fifo_empty) {
        uint32_t data = spi->data_buf[0];  // Read data from RX FIFO
        // Process the data
        receiveBuffer.writeBuffer(data & 0xFF);   
    }

    // Clear the interrupt flag
    spi->dma_in_link.restart = 1;
    spi->dma_int_clr.val = spi->dma_int_st.val;
}

// Attach the interrupt handler
void attach_spi_interrupt(spi_dev_t *spi) {
    esp_err_t err = esp_intr_alloc(ETS_SPI2_INTR_SOURCE, ESP_INTR_FLAG_IRAM, spi_isr_handler, (void*)spi, NULL);
    if (err != ESP_OK) {
        printf("Failed to allocate SPI interrupt: %d\n", err);
    }
}

void spiReceiveTask(void *pvParameters) 
{
  Serial.printf("RECEIVE task started.\r\n");  
  Serial.flush();

#if true  // try to receive without transaction/chip-select
  receive_spi_setup_nocs();
  //receive_spi_setup();  

  spi_dev_t *spi = SPI_LL_GET_HW(RCV_HOST);  // Use the low-level SPI handle

  Serial.printf("Slave mode: %d\r\n",spi->slave.slave_mode );
  Serial.printf("dma_rd_bytelen: %d \r\n",spi->slv_rdbuf_dlen.dma_rd_bytelen);


  spi_tx_reg(spi, 0xff); // dummy write 
  spi_rx_reg(spi); // dummy read

  enable_spi_interrupt(spi);
  
  // Attach the interrupt handler 
  attach_spi_interrupt(spi);

  //spi->dma_in_link.dma_inlink_start = 1; // cleared automatically by HW

  queue_transactions();

  spi_receive_no_transactions(spi);
  while (1) 
  {
    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay

    
    if (spi_is_rx_nonempty(spi)) 
    {
      uint32_t received_data = spi_rx_reg(spi);
      // Process the received data

      uint8_t received_byte = received_data & 0xFF;
      printf("Received byte: 0x%02X\n", received_byte);

      // Do whatever processing you need with the received byte
      receiveBuffer.writeBuffer(received_byte);    
    } 
             
  }
#else
  receive_spi_setup();  
  queue_transactions();
#endif

  while (true) 
  {

    vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay

    //try_receive();
    //try_receive_simple();    

/*
    Serial.printf("spi_slave_queue_trans()\r\n");  
    // Queue the transaction and wait for the master to trigger it
    esp_err_t ret = spi_slave_queue_trans(RCV_HOST, &t, portMAX_DELAY);

    Serial.printf("spi_slave_queue_trans() returned\r\n");  


    if (ret == ESP_OK) 
    {
        // Wait for the transaction to complete
        spi_slave_transaction_t *trans;
        
        //ret = spi_slave_get_trans_result(RCV_HOST, &trans, portMAX_DELAY);
        ret = spi_slave_get_trans_result(RCV_HOST, &trans, 10000);
        

        if (ret == ESP_OK) 
        {
          Serial.println("Transaction complete. Data received.\r\n");
        } 
        else 
        {
          Serial.printf("Failed to get transaction result: %s\r\n", esp_err_to_name(ret));
        }
    }
    else 
    {
      Serial.printf("Failed to queue transaction: %s\r\n", esp_err_to_name(ret));
    }
*/
  }
}

