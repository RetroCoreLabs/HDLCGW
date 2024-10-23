// https://github.com/orgua/iLib/blob/main/src/atmel_hdlc.h
#define DEBUG_DETAIL 0

#include <string>

// datasheet: http://tools.ietf.org/html/rfc1662
#include "hdlc.h"

/// HDLC /////////////////////////////////////////////////////////
/* a protocol for serial communication featuring frames, checksum and auto-escaping of characters.
    

    send:
        Serial.begin(57600);        // open serialport
        hdlc.frameOpen();       // framestart
        hdlc.frameWrite(byte data); //repeat as often as you want
        hdlc.frameClose();      // frame-ending (&checksum) and flush

    receive:
        uint8_t incoming_msg[80];
        if (uint8_t msg_size = hdlc.poll())
        {
            for (uint8_t ivar=0; ivar < size_msg; ++ivar )
            {
                incoming_msg[ivar] = hdlc.getMsg(ivar);
            };
        };
        hdlc.clearMsg();

*/




String byteToBinaryString(uint8_t byte) {
    String binaryString = "";
    for (int i = 7; i >= 0; i--) {
        binaryString += (byte & (1 << i)) ? '1' : '0';  // Append '1' or '0' based on the bit
    }
    return binaryString;
}


// Constructor
HDLC::HDLC(void) 
{
  Clear();
}

void HDLC::Clear(void)
{
  protocol = PROTOCOL_BOP;
  protocol_flag = HDLC_FLAG;  
  crc16=0;
  msg_now = 0;
  msg_received = 0;

  bitCount = 0;
  frameByte = 0;
  msg_ptr = 0;
  frameCount = 0;
  currentState = HDLC_STATE_IDLE;

  for (int i = 0; i < HDLC_INPUT_BUFFER_SIZE;i++)
  {
    msg[i] = 0;
  }
}


void HDLC::EnableCCP(uint8_t synChar)
{
  protocol_flag = synChar;
  protocol = PROTOCOL_CCP;
}

void HDLC::ReceiveByte(const uint8_t data)
{
  for (int i=0;i<8;i++)
  {
      if ((data & 1<<i)!=0)
      {
        ReceiveBit(1);
      }
      else
      {
        ReceiveBit(0);
      }
  }
}


// Received a bit thats ONE
void HDLC::ReceiveBit(uint8_t bit)
{  
  //Serial.printf("[%s]ReceiveBit[%d] %d \r\n",byteToBinaryString(frameByte), currentState,bit);

  switch (currentState)
  {
    case HDLC_STATE_IDLE:

      // Trying to find a HDLC flag in the bitstream..
      frameByte <<= 1;  
      frameByte |= bit;
      frameByte &= 0xFF;

      if (frameByte == protocol_flag)
      {        
#if DEBUG_DETAIL
        Serial.printf("found first flag: ");
        Serial.printf("0x%02X\r\n ", frameByte);
#endif        
        AddToFrame(frameByte); // add HDLC flag to frame        
        bitCount = 0;
      }
      break;

    case HDLC_STATE_RECEIVING_DATA:
      if ((bit==0) && (oneCounter ==5))
      {
        // ignore stuffed bit
        oneCounter = 0;
        return;
      }

            // Trying to find a HDLC flag in the bitstream..
      frameByte <<= 1;  
      frameByte |= bit;
      frameByte &= 0xFF;
      bitCount++;


      if (bit==1)
      {
        oneCounter++;
      }
      else
      {
        oneCounter = 0;
      }

      
      if (bitCount == 8)
      {
        AddToFrame(frameByte);
      
        frameByte = 0;
        bitCount = 0;
      }
      break;

    case HDLC_STATE_FRAME_COMPLETE:
      // Handle completed frame
#if DEBUG_DETAIL                
        Serial.printf("Frame complete. Found last flag: ");
        Serial.printf("0x%02X\r\n ", frameByte);

#endif  
      break;

  }
  
}


// Add 8 bites to the HDLC frame
void HDLC::AddToFrame(uint8_t data)
{
  if (currentState == HDLC_STATE_FRAME_COMPLETE) return;

  switch (currentState)
  {
    case HDLC_STATE_IDLE:
      if (data == protocol_flag)
      {
        if (protocol == PROTOCOL_CCP)
        {
          currentState  = HDLC_WAITING_FOR_SYN2;
        }
        else
        {
          currentState = HDLC_STATE_RECEIVING_DATA;
        }
      }
      break;

    case HDLC_STATE_RECEIVING_DATA:
      if (data == protocol_flag)
      {
        currentState = HDLC_STATE_FRAME_COMPLETE;
      }
      else
      {
        if (msg_ptr <HDLC_MAX_FRAME_LENGTH)
        {
          msg[msg_ptr++] = data;
          crc16 = crcIteration(crc16, data);
        }
      }
    break;

    default:
      break;
  }
}

  int HDLC::Lenght()
  {
      return msg_ptr;
  }

    uint8_t HDLC::GetFrameByte(int pos)
    {
      if (pos < msg_ptr)
      {
        return msg[pos];
      }
    }

    HDLCReceiveState HDLC::GetState()
    {
      return currentState;
    }

void HDLC::CreateDummyFrame()
{
  Clear();
  AddToFrame(HDLC_FLAG);
  
  AddToFrame(0x55); // Address
  AddToFrame(0x00); // Control

  for (int i=0;i<20;i++)
  {
    AddToFrame('A'+i);
  }

  // ADD FCS
  uint16_t crc_copy = ~crc16;

  AddToFrame((crc_copy >> 0) & 0xFF);
  AddToFrame((crc_copy >> 8) & 0xFF);

  AddToFrame(HDLC_FLAG);
}

void HDLC::CreateDummyCCPFrame(uint8_t flag)
{
  Clear();
  AddToFrame(flag);
  
  AddToFrame(0x55); // Address
  AddToFrame(0x00); // Control

  for (int i=0;i<20;i++)
  {
    AddToFrame('A'+i);
  }

  // ADD FCS
  uint16_t crc_copy = ~crc16;

  AddToFrame((crc_copy >> 0) & 0xFF);
  AddToFrame((crc_copy >> 8) & 0xFF);

  AddToFrame(flag);
}


uint16_t HDLC::crcIteration(const uint16_t crc, const uint8_t _b)
{
    return (crc >> 8) ^ hdlc_fcstab[(crc ^ _b) & 0xff];
}

void HDLC::frameOpen()
{
    Serial.write(HDLC_FLAG);
    crc16 = HDLC_CRCINIT;
}

void HDLC::frameWrite(const uint8_t _b)
{
    crc16 = crcIteration(crc16, _b);

    uint8_t bw;

    if (_b == HDLC_FLAG)
    {
        Serial.write(HDLC_ESCAPE);
        bw = HDLC_FLAG_ESC;
    }
    else if (_b == HDLC_ESCAPE)
    {
        Serial.write(HDLC_ESCAPE);
        bw = HDLC_ESCAPE_ESC;
    }
    else
    {
        bw = _b;
    }

    Serial.write(bw);
}

uint16_t HDLC::frameClose()
{
    uint16_t _crc = ~crc16;

    frameWrite((_crc >> 0) & 0xFF);
    frameWrite((_crc >> 8) & 0xFF);

    Serial.write(HDLC_FLAG);
    Serial.flush();
}

uint8_t HDLC::poll()
{
    if (msg_received < 3)
    {
        while (Serial.available())
        {
            uint8_t _b = uint8_t(Serial.read());

            if (msg_received == 0)
            {
                if (_b == HDLC_FLAG) msg_received = 1;
                if (!Serial.available()) delay(1);
                continue;
            }
            if ((msg_received == 2) && (_b == HDLC_FLAG))
            {
                msg_received = 3;
                break;
            }
            if (msg_received == 1)
            {
                if (_b == HDLC_FLAG) continue;
                else msg_received = 2;
            }

            if (msg_received == 2)
            {
                if (_b == HDLC_ESCAPE)
                {
                    uint8_t wstop = 0;
                    while (!Serial.available())
                    {
                        delay(1);
                        if (wstop++ > 10) break;
                    }
                    _b = uint8_t(Serial.read());
                    if (_b == HDLC_FLAG_ESC) _b = HDLC_FLAG;
                    if (_b == HDLC_ESCAPE_ESC) _b = HDLC_ESCAPE;
                }
                msg[msg_now] = _b;
                msg_now++;
            }
            if (!Serial.available()) delay(1);
        }
    }

    if (msg_received == 3)
    {
        msg_now -= 2;

        uint16_t crcT = HDLC_CRCINIT;
        for (uint8_t ivar = 0; ivar < msg_now; ++ivar)
        {
            crcT = crcIteration(crcT, msg[ivar]);
        }
        crcT = ~crcT;
        uint16_t crcM = msg[msg_now] | (msg[msg_now + 1] << 8);

        if (crcM != crcT)
        {
            Clear();
            return 0;
        }

        msg_received = 4;
        return msg_now;
    }

    if (msg_received > 2) return msg_now;
    else return 0;
}

uint8_t HDLC::getMsg(const uint8_t position)
{
    return (position > (HDLC_INPUT_BUFFER_SIZE - 1)) ? 0 : msg[position];
}
