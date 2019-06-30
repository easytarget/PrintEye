
/*
  RepRap Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)
  - performs a small, display-only, subset of panelDue functionality, uses same comms channel and port

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
*/

// power&clock divider lib: https://arduino.stackexchange.com/a/5414
#include <avr/power.h>

#include <Arduino.h>
#include <U8x8lib.h>

// debug : To enable add MemoryFree.c & .h from https://playground.arduino.cc/Code/AvailableMemory/ to sketch
#include "MemoryFree.h"

// #include <ArduinoJson.h> 

// U8x8 Contructor List 
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
// Please update the pin numbers according to your setup. Use U8X8_PIN_NONE if the reset pin is not connected
// WORKS! - HW I2C only..  //U8X8_SSD1306_128X64_NONAME_HW_I2C LOLED(/* reset=*/ U8X8_PIN_NONE); 	      
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ A3, /* data=*/ A2, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ A5, /* data=*/ A4, /* reset=*/ U8X8_PIN_NONE);   // Right OLED

// JSON containers
// todo

// End of constructor list

// globals

int bedmain=12;
int bedunits=3;
int toolmain=123;
int toolunits=4;

int toolhead=3;

int noreply = 1;
int mainstate = 0;

int bright = 3; // track display brightness
int lastbright = 0; // unnesscarily setting brightness causes a screen flicker

// Start displays, serial and blank
void setup(void)
{
  // First; lets drop to 8MHz since that is in-spec for a ATMega328P @ 3.3v
  clock_prescale_set(clock_div_2);

  // Some serial is needed
  Serial.begin(115200);
  Serial.println("EyeDrop: START");

  // Displays
  LOLED.begin();
  LOLED.setFlipMode(1);
  ROLED.begin();
  ROLED.setFlipMode(1);
  
  godark();
  splash();
}

void godark(void)
{
  // Blank
  LOLED.setFont(u8x8_font_8x13B_1x2_r);
  ROLED.setFont(u8x8_font_8x13B_1x2_r);
  LOLED.clear();
  ROLED.clear();
}

void splash(void)
{
  godark();
  Serial.println("V0.01 - alpha - owen.carter@gmail.com");

  LOLED.setFont(u8x8_font_8x13B_1x2_r);
  LOLED.setCursor(5,6);
  LOLED.print(" Bed ");
  
  ROLED.setFont(u8x8_font_8x13B_1x2_r);
  ROLED.setCursor(5,6);
  ROLED.print(" Tool ");

  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6,1);
  LOLED.print("F"); // https://github.com/olikraus/u8g2/wiki/fntgrpiconic
  
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setCursor(6 ,1);
  ROLED.print("F"); // https://github.com/olikraus/u8g2/wiki/fntgrpiconic

  delay(1750);

  LOLED.clear();
  ROLED.clear();
}

bool setbrightness() 
{
  // Sets the (non-linear) contrast level to off/low/mid/high 
  int contrast;
  if ( bright != lastbright) 
  {
    // Three brightness levels low,mid,high, ignore all else
    if ( bright == 0 ) { 
      // Blank the screen as well as setting contrast = 0
      godark;
      contrast = 0;
    }
    else if ( bright == 1 ) contrast = 1;   // three
    else if ( bright == 2 ) contrast = 64;  // brightness
    else if ( bright == 3 ) contrast = 255; // levels
    LOLED.setContrast(contrast);
    ROLED.setContrast(contrast);
    lastbright = bright;
  }
  if ( bright > 0 ) return(true); else return(false); 
}

void updatedisplay()
{
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0,0);
  if ( bedmain < 100 ) LOLED.print(" "); // pad decimal
  LOLED.print(bedmain);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9,3);
  LOLED.print(".");
  LOLED.print(bedunits);

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0,0);
  if ( toolmain < 100 ) ROLED.print(" "); // pad decimal
  ROLED.print(toolmain);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9,3);
  ROLED.print(".");
  ROLED.print(toolunits);

  LOLED.setFont(u8x8_font_8x13B_1x2_r);
  LOLED.setCursor(10,0);
  LOLED.print("Bed");
  LOLED.setCursor(1,6);
  if (mainstate == 0 ) LOLED.print("Init");
  if (mainstate == 1 ) LOLED.print("Run");
 
  ROLED.setFont(u8x8_font_8x13B_1x2_r);
  ROLED.setCursor(11,0);
  ROLED.print("E");
  ROLED.print(toolhead);
  ROLED.setCursor(0,6);
  ROLED.print("#: ");
  ROLED.print(noreply);

  // if Bed active
  LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
  LOLED.setCursor(14,0);
  LOLED.print("N");

  // If Tool Active
  ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
  ROLED.setCursor(14,0);
  ROLED.print("T"); // (down arrow to line)

}

void loop(void)
{
  Serial.print("DataRequest: (");
  Serial.print(noreply);
  Serial.print(" unreplied)");
  // debug
  Serial.print(": freeMemory()=");
  Serial.print(freeMemory());
  Serial.print(" : Bed= ");
  Serial.print(bedmain);
  Serial.print(".");
  Serial.print(bedunits);
  Serial.print(" : Tool= ");
  Serial.print(toolmain);
  Serial.print(".");
  Serial.print(toolunits);
  Serial.print(" : Brightness= ");
  Serial.print(bright);

  // update brightness level as needed; returns true if screen is on, false if blank.
  if ( setbrightness() )
  {
    // if we are not blank, update the display with latest readings
    updatedisplay();
  }

  
  // increment counters for demo
  noreply++;
  toolmain += rand() % 16;
  toolunits = rand() % 10;
  if ( toolmain > 310 ) toolmain = 8;
  bedmain += rand() % 4;
  bedunits = rand() % 10;
  if ( bedmain > 110 ) bedmain = 15;

  if ( Serial.available() ) 
  {
    int a = Serial.read();
    switch (a) 
    {
      case '0': bright = 0; break;
      case 'l': bright = 1; break;
      case 'm': bright = 2; break;
      case 'h': bright = 3; break;
    }
    while ( Serial.available() ) Serial.read(); // flush buffer
  }

  delay(1000);

  Serial.println(); // Move to next request

}
