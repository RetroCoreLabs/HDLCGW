#include "console.h"
#include <Arduino.h>
#include "commands.h"
#include "sender.h"
#include "receiver.h"
#include "receiverI2S.h"
#include "receiverIRQ.h"
#include "hdlc.h"

String promptChar = "# ";       // Configurable command prompt, now a string

// Define the LED pin, which is GPIO 15 on the Lolin S2 Mini
// https://done.land/components/microcontroller/families/esp/esp32/s2/s2mini#built-in-led
#define LED_PIN 15
// Variable to track the LED state
bool ledState = HIGH;

// Function to toggle the LED
void toggleLED() {
	ledState = !ledState; // Toggle the state
	digitalWrite(LED_PIN, ledState); // Set the new state to the LED pin
}

HDLC hdlcReceiveFrame;

void handleReceiveBufffer()
{
	int cnt = 0;

	if (receiveToSerialEnabled || loopbackEnabled || hdlcMode)
	{
		while (!receiveBuffer.isBufferEmpty())
		{
			cnt++;
			uint8_t data = receiveBuffer.readBuffer();
			if (loopbackEnabled)
			{
				sendBuffer.writeBuffer(data);  // Send received data to sender      
			}


			if (hdlcMode)
			{
				hdlcReceiveFrame.ReceiveByte(data);

        //Serial.printf("HDLC byte received 0x%02X. Len=%d, State=%d\r\n", data, hdlcReceiveFrame.Lenght(), hdlcReceiveFrame.GetState());

				if (hdlcReceiveFrame.GetState() == HDLC_STATE_FRAME_COMPLETE)
				{					
					
					int len = hdlcReceiveFrame.Lenght();
					if (len > 0)
					{
						Serial.printf("HDLC frame received, len=%d\r\n", len);
					}

					if ((receiveToSerialEnabled)&& (len>0))
					{
						for (int i = 0; i < len; i++)
						{
							uint8_t data = hdlcReceiveFrame.GetFrameByte(i);
							Serial.printf("0x%02X ", data);

							if ((i + 1) % 8 == 0)
							{
								Serial.println();
							}

						}
						Serial.println();
					}


					if (len > 0)
					{
						Serial.printf("HDLC frame size = %d bytes.\r\n", len);
					}
          			hdlcReceiveFrame.Clear(); // Ready for next
				}				
			}


			// Echo back bytes, but not if HDLC mode ie enabled, then we want to echo back HDLC frames
			if ((receiveToSerialEnabled) && (!hdlcMode))
			{
				if (data != 0)
				{
					Serial.printf("Received data: 0x%02X\r\n", data);
					Serial.flush();
				}
			}

			//vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
			if (cnt > 10)
			{
				return;
			}
		}
	}
}



void handleEchoHDLC()
{
	if (hdlcMode)
	{

	}
}

#define MAX_LEN 1024  // Maximum input length
char inputBuffer[MAX_LEN + 1];  // Input buffer, with an extra byte for null-termination
int inputPos = 0;               // Position in the buffer



// Function to handle serial input, including backspace and prompt display
void handleSerialInput() {
	while (Serial.available()) {
		char c = Serial.read();  // Read a character
		toggleLED();

		// Handle backspace
		if ((c == '\b') || (c == 0x7f)) {
			if (inputPos > 0) {
				inputPos--;                 // Move back in the buffer
				inputBuffer[inputPos] = '\0';
				Serial.write("\b#\b");      // Erase the last character in the terminal
			}
		}
		// Handle CR or LF (Enter)
		else if (c == '\n' || c == '\r')
		{

			{
				Serial.println("\r\n");

				if (inputPos > 0)
				{
					// Null-terminate the string and process it
					inputBuffer[inputPos] = '\0';
					processCommand(inputBuffer);  // Process the command
					inputPos = 0;                 // Reset buffer for the next command
				}
				showPrompt();  // Show the prompt after processing the command
			}
		}
		// Handle regular character input
		else
		{
			// Add character to buffer if within size limits
			if (inputPos < MAX_LEN) {
				inputBuffer[inputPos++] = c;  // Store the character
				Serial.write(c);              // Echo the character back
			}
			else {
				// Buffer overflow, send error and reset buffer
				Serial.println("\nError: input exceeds maximum length of 1024 characters.");
				inputPos = 0;  // Clear the buffer
				showPrompt();  // Show the prompt again
			}
		}
	}
}




// Function to update the prompt dynamically based on receiveBuffer and sendBuffer lengths
void updatePrompt() {
	int receiveLength = receiveBuffer.Length();  // Access receiveBuffer length
	int sendLength = sendBuffer.Length();        // Access sendBuffer length

	String p = "#";

	if (hdlcMode)
	{
		p = "@";
	}

	if (receiveToSerialEnabled)
	{
		p = p + ">";
	}

	p = p + " ";


	if ((receiveLength > 0) || (sendLength > 0))
	{
		// Format the prompt as [rxbufsize:txbufsize]#
		promptChar = "[" + String(receiveLength) + ":" + String(sendLength) + "]" + p;
	}
	else

	{
		promptChar = p;
	}
}


// Function to display the command prompt
void showPrompt() {
	updatePrompt();  // Update the prompt dynamically
	Serial.print(promptChar);
}


void initializeConsole()
{
	// Initialize the LED pin as an output
	pinMode(LED_PIN, OUTPUT);
	// Start with the LED off
	digitalWrite(LED_PIN, ledState);

}

void consoleTask(void* pvParameters)
{
	initializeConsole();

	Serial.printf("Free heap size: %d\r\n", esp_get_free_heap_size());
	Serial.flush();


  	// Enable CCP Bisync
  	//hdlcReceiveFrame.EnableCCP(0x16);
  
	while (true)
	{
		// Read serial commands from serial port
		handleSerialInput();

		// Handle received data
		handleReceiveBufffer();


		// Handle echo HDLC packets received
		handleEchoHDLC();

		// Give some CPU back
		vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
	}
}