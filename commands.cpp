#include "commands.h"
#include "sender.h"
#include "RingBuffer.h"
#include "hdlc.h"


// Global variables for baud rate
uint32_t defaultBaudRate = 38400;
uint32_t currentBaudRate = 9600;
const uint32_t maxBaudRate = 983000;

// Loopback flag
bool loopbackEnabled = false;

// Send received data to USB Serial port
bool receiveToSerialEnabled = false;

// HDLC mode ?
bool hdlcMode = false;

SPISettings* spiSendSettings = NULL;


#define DATA_DIRECTION MSBFIRST
// Array of structs to map baud rates to SPI settings - for transmit
BaudRateConfig baudRateConfigs[] = {
  {9600, SPISettings(9600, DATA_DIRECTION, SPI_MODE0)},
  {19200, SPISettings(19200, DATA_DIRECTION, SPI_MODE0)},
  {38400, SPISettings(38400, DATA_DIRECTION, SPI_MODE0)},
  {115200, SPISettings(115200, DATA_DIRECTION, SPI_MODE0)},
  {307200, SPISettings(307200, DATA_DIRECTION, SPI_MODE0)},
  {983000, SPISettings(983000, DATA_DIRECTION, SPI_MODE0)}
};

// Function to find the baud rate configuration
BaudRate findBaudRate(uint32_t baudRate) {
	for (int i = 0; i < sizeof(baudRateConfigs) / sizeof(baudRateConfigs[0]); i++) {
		if (baudRateConfigs[i].baudRate == baudRate) {
			return static_cast<BaudRate>(i);  // Return the index as the enum value
		}
	}
	return BAUD_INVALID;  // Return invalid if not found
}

// Function to set the current clock speed
void setClockSpeed(BaudRate selectedBaud) {
	if (selectedBaud >= BAUD_9600 && selectedBaud <= BAUD_983000) {
		spiSendSettings = &baudRateConfigs[selectedBaud].spiSettings;
		Serial.printf("Baud rate set to %d\r\n", baudRateConfigs[selectedBaud].baudRate);
	}
	else {
		Serial.println("Invalid baud rate selection!");
	}
}

// Set default send baud rate
void SetDefaultSendBaudRate()
{
	setClockSpeed(findBaudRate(defaultBaudRate));
}

// Define the available commands
Command commands[] = {
  {"HELP", "List all commands and their parameters", handleHelp},
  {"SEND", "SEND <hex bytes> - Send a sequence of hex bytes", handleSend},
  {"BAUD", "BAUD <value> - Set baud rate. BAUD MAX for highest rate.", handleBaud},
  {"LOOPBACK", "LOOPBACK ON|OFF - Enable or disable loopback mode", handleLoopback},
  {"ECHO", "Echo data to serial port", handleEcho},
  {"DUMP", "Dump received buffer", handleDump},
  {"CLEAR", "Clear received buffer", handleClear},
  {".", "Send default debug data", handleSendDebug},
  {"HDLC", "Echo HDLC packets", handleHDLC},
  {"F","Send HDLC frame",handleSendHDLCFrame}
#if TASK_COMMAND    
  { "TASK", "List running tasks", handleTask }
#endif
};

// Total number of commands
const int numCommands = sizeof(commands) / sizeof(commands[0]);

// Function to handle the HELP command
void handleHelp(const char* args) {
	Serial.println("Available commands:");
	for (int i = 0; i < numCommands; i++) {
		Serial.printf("  %s: %s\r\n", commands[i].name, commands[i].helpText);
	}
}

// Function to handle the SEND command
void handleSend(const char* args) {
	if (args == NULL || strlen(args) == 0) {
		Serial.println("Usage: SEND <hex bytes>");
		return;
	}

	// Convert the hex string to bytes and send
	while (*args) {
		char byteStr[3] = { args[0], args[1], 0 };  // Get two hex chars
		uint8_t byte = strtoul(byteStr, NULL, 16);  // Convert to a byte
		sendBuffer.writeBuffer(byte);  // Send to buffer
		args += 2;  // Move to the next pair of hex digits
	}

	Serial.println("Data sent to buffer.");
}

// Function to handle the BAUD command
void handleBaud(const char* args) {
	if (args == NULL || strlen(args) == 0) {
		Serial.printf("Current baud rate: %d\r\n", currentBaudRate);

		Serial.printf("\r\nValid choices:\r\n");
		for (int i = 0; i < sizeof(baudRateConfigs) / sizeof(baudRateConfigs[0]); i++)
		{
			Serial.printf("  %d\r\n", baudRateConfigs[i].baudRate);
		}
		Serial.print("\r\n");
		return;
	}

	if (strcasecmp(args, "MAX") == 0) {
		currentBaudRate = maxBaudRate;
		setClockSpeed(findBaudRate(currentBaudRate));
		Serial.printf("Baud rate set to MAX (%d)\r\n", currentBaudRate);
		return;
	}

	if (strcasecmp(args, "DEFAULT") == 0) {
		currentBaudRate = defaultBaudRate;
		setClockSpeed(findBaudRate(currentBaudRate));
		Serial.printf("Baud rate set to DEFAULT (%d)\r\n", currentBaudRate);
		return;
	}

	uint32_t baudRate = atoi(args);  // Convert argument to integer
	BaudRate selectedBaud = findBaudRate(baudRate);
	if (selectedBaud != BAUD_INVALID) {
		currentBaudRate = baudRate;
		setClockSpeed(selectedBaud);
		//Serial.printf("Baud rate set to %d\r\n", currentBaudRate);
	}
	else {
		Serial.println("Invalid baud rate.");
	}
}

// Function to handle the LOOPBACK command
void handleLoopback(const char* args) {
	if (args == NULL || strlen(args) == 0) {
		Serial.printf("Loopback is currently %s\r\n", loopbackEnabled ? "ON" : "OFF");
		return;
	}

	// No parameter means show current status
	if (args == NULL || strlen(args) == 0) {
		Serial.printf("Current Loopback mode is: ");
		if (loopbackEnabled)
		{
			Serial.println("ENABLED");
		}
		else
		{
			Serial.println("DISABLED");
		}
		return;
	}


	if (strcasecmp(args, "ON") == 0) {
		loopbackEnabled = true;
		Serial.println("Loopback mode enabled.");
	}
	else if (strcasecmp(args, "OFF") == 0) {
		loopbackEnabled = false;
		Serial.println("Loopback mode disabled.");
	}
	else {
		Serial.println("Invalid parameter. Use ON or OFF.");
	}
}

void handleEcho(const char* args)
{
	receiveToSerialEnabled = !receiveToSerialEnabled;
}


void handleDump(const char* args)
{
	int len = receiveBuffer.Length();

	if (len == 0)
	{
		Serial.println("Nothing to dump");
		return;
	}


	Serial.printf("Dumping %d bytes\r\n", len);

	for (int i = 0; i < len; i++)
	{
		uint8_t data = receiveBuffer.readBuffer();
		Serial.printf("0x%02X ", data);
		if ((i + 1) % 8 == 0)
		{
			Serial.println();
		}
	}

	Serial.println();
}

void handleClear(const char* args)
{
	Serial.println("Cleared receive buffer");
	receiveBuffer.Clear();
}



void handleSendDebug(const char* args)
{
	int cnt = 10;

	if (args == NULL || strlen(args) == 0)
	{
		cnt = 10;
	}
	else
	{
		cnt = atoi(args);  // Convert argument to integer
	}

	Serial.printf("Sending %d bytes\r\n", cnt);

	for (int i = 0; i < cnt; i++)
	{
		while (sendBuffer.isBufferFull())
		{
			vTaskDelay(pdMS_TO_TICKS(10)); // 10ms delay
		}
		sendBuffer.writeBuffer(i & 0xFF);  // Send to buffer    
	}
}


void handleHDLC(const char* args)
{
	hdlcMode = !hdlcMode;
	Serial.printf("HDLC mode is %s.\r\n", hdlcMode ? "ENABLED" : "DISABLED");
}

HDLC hdlcSendFrame;
void handleSendHDLCFrame(const char* args)
{
	hdlcSendFrame.Clear();

	hdlcSendFrame.CreateDummyFrame();

	int len = hdlcSendFrame.Lenght();
	for (int i = 0; i < len; i++)
	{
		uint8_t data = hdlcSendFrame.GetFrameByte(i);
		sendBuffer.writeBuffer(data & 0xFF);  // Send to buffer    
	}
	Serial.printf("Created and sent HDLC frame, %d bytes.\r\n", len);
}



#if TASK_COMMAND
TaskStatus_t taskStatusArray[10]; // Adjust size as needed
uint32_t totalRunTime;
// Buffer to hold the task list
char taskListBuffer[512];

// Function to handle the TASK command
void handleTask(const char* args)
{

#if false
	// Get the system state and task information
	uint32_t numTasks = uxTaskGetSystemState(taskStatusArray, 10, &totalRunTime);

	// Print task information
	Serial.println("Task Name\tRuntime");
	for (uint32_t i = 0; i < numTasks; i++) {
		Serial.print(taskStatusArray[i].pcTaskName);
		Serial.print("\t");
		Serial.println(taskStatusArray[i].ulRunTimeCounter);
	}
#else
	// Call vTaskList to fill the buffer with task information
	vTaskList(taskListBuffer);

	// Print the task list information over serial
	Serial.println("Task Name\tState\tPrio\tStack\tTask#");
	Serial.println(taskListBuffer);

	/*
	Task Name   State   Prio  Stack  Task#
	--------------------------------------
	IDLE        R       0      128    1
	SPI Task    B       1      256    2
	Send Task   B       1      512    3
	Recv Task   B       1      512    4

	*/
#endif
}

#endif

// Function to process incoming commands
void processCommand(char* input) {
	char* command = strtok(input, " ");
	char* args = strtok(NULL, "");  // The rest is the argument string

	for (int i = 0; i < numCommands; i++) {
		// Use strcasecmp() for case-insensitive comparison
		if (strcasecmp(command, commands[i].name) == 0) {
			commands[i].handler(args);  // Call the command handler
			return;
		}
	}


	Serial.printf("Unknown command '%s'. Type HELP for a list of commands.\r\n", command);
}
