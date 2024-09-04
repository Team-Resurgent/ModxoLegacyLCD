#include "xeniumspi.h"
#include "xboxsmbus.h"
#include "Wire.h"
#include <LiquidCrystal.h>

//Some screens like difference contrast values. So I couldn't really set a good default value
//Play around and adjust to suit your screen. If you have flickering issues, it may help to solder
//an extra electrolytic capacitor between the contrast pin (normally labelled 'V0') and GND.
#define DEFAULT_CONTRAST 100 //0-255. Lower is higher contrast.
#define DEFAULT_BACKLIGHT 255 //0-255. Higher is brighter.

//Use the following lines to set the LCD display size; currently only displays of 20/16 columns and 4 rows are supported.
//16 column displays will have issues with the dashboard display as these are all expecting 20 column displays.
#define LCD_ROW 4
#define LCD_COL 20

//HD44780 LCD Setup
const uint8_t rs = 18, en = 8, d4 = 7, d5 = 6, d6 = 5, d7 = 4; //HD44780 compliant LCD display pin numbers
const uint8_t i2c_sda = 2, i2c_scl = 3; //i2c pins
const uint8_t backlightPin = 10, contrastPin = 9; //Pin nubmers for backlight and contrast. Must be PWM enabled
uint8_t cursorPosCol = 0, cursorPosRow = 0; //Track the position of the cursor
uint8_t wrapping = 0, scrolling = 0; //Xenium spi command toggles for the lcd screen.
LiquidCrystal hd44780(rs, en, d4, d5, d6, d7); //Constructor for the LCD.


//SPI Data
int16_t RxQueue[256]; //Input FIFO buffer for raw SPI data from Xenium
uint8_t QueuePos; //Tracks the current position in the FIFO queue that is being processed
uint8_t QueueRxPos; //Tracks the current position in the FIFO queue of the unprocessed input data (raw realtime SPI data)

void receiveEvent(int howMany) 
{
  while (Wire.available() > 0) 
  { 
    uint8_t c = Wire.read(); // receive byte
    RxQueue[QueueRxPos] = c;
    QueueRxPos++;
  }
  Serial.println();
}

void setup() {

  Serial.begin(115200);

  for (uint8_t i = 0; i < 8; i++) {
    delay(1);
    hd44780.createChar(i, &chars[i][0]);
  }

  hd44780.begin(LCD_COL, LCD_ROW);
  hd44780.noCursor();

  memset(RxQueue, -1, 256);

  Wire.begin(0x10);                // join I2C bus with address # 0x10
  Wire.onReceive(receiveEvent); //Random address that is different from existing bus devices.
 
  analogWrite(backlightPin, DEFAULT_BACKLIGHT); //0-255 Higher number is brighter.
  analogWrite(contrastPin, DEFAULT_CONTRAST); //0-255 Lower number is higher contrast

  //Speed up PWM frequency. Gets rid of flickering
  TCCR1B &= 0b11111000;
  TCCR1B |= (1 << CS00); //Change Timer Prescaler for PWM
  //hd44780.setCursor(0, 0); //Note: This command causes problems with i2c slave
}


void loop() {


  //I2C to Parallel Conversion State Machine
  //One completion of processing command, set the buffer data value to -1
  //to indicate processing has been completed.

  if (QueueRxPos != QueuePos) {
    switch (RxQueue[(uint8_t)QueuePos]) {
      case -1:
        //No action required.
        break;

      case XeniumCursorHome:
        cursorPosRow = 0;
        cursorPosCol = 0;
        hd44780.setCursor(cursorPosCol, cursorPosRow);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumHideDisplay:
        hd44780.noDisplay();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumShowDisplay:
        hd44780.display();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumHideCursor:
        hd44780.noCursor();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumShowUnderLineCursor:
      case XeniumShowBlockCursor:
      case XeniumShowInvertedCursor:
        hd44780.cursor();
        hd44780.blink();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumBackspace:
        if (cursorPosCol > 0) {
          cursorPosCol--;
          hd44780.setCursor(cursorPosCol, cursorPosRow);
          hd44780.print(" ");
          hd44780.setCursor(cursorPosCol, cursorPosRow);
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumLineFeed: //Move Cursor down one row, but keep column
        if (cursorPosRow < LCD_ROW - 1) {
          cursorPosRow++;
          hd44780.setCursor(cursorPosCol, cursorPosRow);
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumDeleteInPlace: //Delete the character at the current cursor position
        hd44780.print(" ");
        hd44780.setCursor(cursorPosCol, cursorPosRow);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumFormFeed: //Formfeed just clears the screen and resets the cursor.
        hd44780.clear();
        cursorPosRow = 0;
        cursorPosCol = 0;
        hd44780.setCursor(cursorPosCol, cursorPosRow);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumCarriageReturn: //Carriage returns moves the cursor to the start of the current line
        cursorPosCol = 0;
        hd44780.setCursor(cursorPosCol, cursorPosRow);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumSetCursorPosition: //Sets the row and column of cursor. The following two bytes are the row and column.
        if (RxQueue[(uint8_t)(QueuePos + 2)] != -1) {
          uint8_t col = RxQueue[(uint8_t)(QueuePos + 1)]; //Column
          uint8_t row = RxQueue[(uint8_t)(QueuePos + 2)]; //Row
          if (col < LCD_COL && row < LCD_ROW) {
            hd44780.setCursor(col, row);
            cursorPosCol = col, cursorPosRow = row;
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 2)]);
        }
        break;

      case XeniumSetBacklight:
        //The following byte after the backlight command is the brightness value
        //Value is 0-100 for the backlight brightness. 0=OFF, 100=ON
        //AVR PWM Output is 0-255. We multiply by 2.55 to match AVR PWM range.
        if (RxQueue[(uint8_t)(QueuePos + 1)] != -1) { //ensure the command is complete
          uint8_t brightness = RxQueue[(uint8_t)(QueuePos + 1)];
          if (brightness >= 0 && brightness <= 100) {
            analogWrite(backlightPin, (uint8_t)(brightness * 2.55f)); //0-255 for AVR PWM
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
        }
        break;

      case XeniumSetContrast:
        //The following byte after the contrast command is the contrast value
        //Value is 0-100 0=Very Light, 100=Very Dark
        //AVR PWM Output is 0-255. We multiply by 2.55 to match AVR PWM range.
        if (RxQueue[(uint8_t)(QueuePos + 1)] != -1) { //ensure the command is complete
          uint8_t contrastValue = 100 - RxQueue[(uint8_t)(QueuePos + 1)]; //needs to convert to 100-0 instead of 0-100.
          if (contrastValue >= 0 && contrastValue <= 100) {
            analogWrite(contrastPin, (uint8_t)(contrastValue * 2.55f));
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
        }
        break;

      case XeniumReboot:
        cursorPosRow = 0;
        cursorPosCol = 0;
        hd44780.begin(LCD_COL, LCD_ROW);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumCursorMove:
        //The following 2 bytes after the initial command is the direction to move the cursor
        //offset+1 is always 27, offset+2 is 65,66,67,68 for Up,Down,Right,Left
        if (RxQueue[(uint8_t)(QueuePos + 1)] == 27 &&
            RxQueue[(uint8_t)(QueuePos + 2)] != -1) {

          switch (RxQueue[(uint8_t)(QueuePos + 2)]) {
            case 65: //UP
              if (cursorPosRow > 0) cursorPosRow--;
              break;
            case 66: //DOWN
              if (cursorPosRow < (LCD_ROW - 1) ) cursorPosRow++;
              break;
            case 67: //RIGHT
              if (cursorPosCol < (LCD_COL - 1)) cursorPosCol++;
              break;
            case 68: //LEFT
              if (cursorPosCol > 0) cursorPosCol--;
              break;
            default:
              //Error: Invalid cursor direction
              break;
          }
          Serial.print("\n");
          hd44780.setCursor(cursorPosCol, cursorPosRow);
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 2)]);
        }
        break;

      //Scrolling and wrapping commands are handled here.
      //The flags are toggled, but are not implemented properly yet
      //My testing seems to indicates it's not really needed.
      case XeniumWrapOff:
        wrapping = 0;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumWrapOn:
        wrapping = 1;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumScrollOff:
        scrolling = 0;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumScrollOn:
        scrolling = 1;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case  32 ... 255: //Just an ASCII character
        Serial.print((char)RxQueue[(uint8_t)QueuePos]);
        if (cursorPosCol < LCD_COL) {
          hd44780.setCursor(cursorPosCol, cursorPosRow);
          hd44780.write((char)RxQueue[(uint8_t)QueuePos]);
          cursorPosCol++;
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      //Not implemented. Dont seem to be used anyway.
      case XeniumLargeNumber:
      case XeniumDrawBarGraph:
      case XeniumModuleConfig:
      case XeniumCustomCharacter:
      default:
        //hd44780.setCursor(0, 0);
        //Serial.print("Not implemented ");
        //Serial.print(RxQueue[(uint8_t)QueuePos], DEC);
        //Serial.print("\r\n");
        //hd44780.setCursor(cursorPosCol, cursorPosRow);
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
    }

    if (RxQueue[(uint8_t)QueuePos] == -1) {
      QueuePos = (uint8_t)(QueuePos + 1);
    }

  }

}

void completeCommand(int16_t* c) {
  *c = -1;
}
