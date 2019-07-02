/*
  RepRap Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)
  - performs a small, display-only, subset of panelDue functionality, uses same comms channel and port

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
*/

// FONT reference
// https://github.com/olikraus/u8g2/wiki/fntgrpiconic


//#define debug

// power&clock divider lib: https://arduino.stackexchange.com/a/5414
//#include <avr/power.h>

#include <Arduino.h>
#include <U8x8lib.h>

// debug : To enable add MemoryFree.c & .h from https://playground.arduino.cc/Code/AvailableMemory/ to sketch
#include "MemoryFree.h"

// #include <ArduinoJson.h> 

// U8x8 Contructor List 
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
// Please update the pin numbers according to your setup. Use U8X8_PIN_NONE if the reset pin is not connected
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ A3, /* data=*/ A2, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ A5, /* data=*/ A4, /* reset=*/ U8X8_PIN_NONE);   // Right OLED
// HW I2C works, but the ATMega 328P only has one HW interface available, so if your displays have address conflicts this 
// can omly be used for for one display; and the results looks weird and imbalanced. IMHO better to use two SW interfaces
//U8X8_SSD1306_128X64_NONAME_HW_I2C ROLED(/* reset=*/ U8X8_PIN_NONE);         


// JSON containers
// todo

// End of constructor list

// globals

// Debug data to be used until proper display processing is implemented
int bedmain=12;  
int bedunits=3;
int toolmain=123;
int toolunits=4;

// JSON response data: (-1 if unset/unspecified)
int bedtemp = 35;   // Current bed temp 
int bedset=-1;     // Bed target temp
int tooltemp = 101; // latest tool temp
int toolset=-1;    // Tool target temp
int toolhead=0;     // Tool to be monitored

// Master printer status (from JSON)
int printerstate = 0;

int validresponse = false; // true when the last recieved data was valid
int noreply = 1;           // count failed requests

int bright = 3; // track display brightness
int lastbright = 3; // and changes to that
int contrast = 255; // Derived from the brightness level.

// Start displays and serial comms

void setup(void)
{
  // First; lets drop to 8MHz since that is in-spec for a ATMega328P @ 3.3v
  // clock_prescale_set(clock_div_2);

  // Some serial is needed
  Serial.begin(115200);
  #ifdef DEBUG
    Serial.println("EyeDrop: START");
  #endif

  // Displays
  LOLED.begin();
  LOLED.setContrast(0); // blank asap, screenbuffer often full of crud
  LOLED.setFlipMode(1);
  ROLED.begin();
  ROLED.setContrast(0); // blank asap, screenbuffer often full of crud
  ROLED.setFlipMode(1);
  
  screenstart(); // wake + clear display at init
  delay(250);
  commwait();
}

void screenclean()
{
  LOLED.clear();
  ROLED.clear();

}
void goblank()
{
  LOLED.setContrast(0);
  ROLED.setContrast(0); 
  screenclean();
}

void unblank()
{
  LOLED.setContrast(contrast);
  ROLED.setContrast(contrast); 
}

void screensleep(void)
{ // flash power off icon then blank the screen and turn on powersave
  goblank();
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6,1);
  ROLED.setCursor(6,1);
  LOLED.print("N"); // power off icon in this font set
  ROLED.print("N"); // power off icon in this font set
  unblank();
  delay(666);
  goblank();
  LOLED.setPowerSave(true);
  ROLED.setPowerSave(true);
}

void screenwake()
{ // Take the screen out of powersave and flash the power on icon
  goblank(); 
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6,1);
  ROLED.setCursor(6,1);
  LOLED.print("O"); // Resume icon in this font set
  ROLED.print("O"); // Resume icon in this font set
  LOLED.setPowerSave(false);
  ROLED.setPowerSave(false);
  unblank();
  delay(666); // flash the icons then clean and reset screen
  goblank();
  unblank();
  
}

void screenstart()
{ // start the screen for the first time (splashscreen..)
  #ifdef DEBUG
    Serial.println("V0.01 - alpha - owen.carter@gmail.com");
  #endif

  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6,1);
  ROLED.setCursor(6,1);
  LOLED.print("C"); // Power bolt icon in this font set
  ROLED.print("C"); // Power bolt icon in this font set
  LOLED.setFont(u8x8_font_8x13B_1x2_r);
  ROLED.setFont(u8x8_font_8x13B_1x2_r);
  LOLED.setCursor(3,6);
  ROLED.setCursor(3,6);
  LOLED.print(" PrinterEye ");
  ROLED.print(" by Owen ");
  unblank();
  delay(2000); // flash the startup icons
  goblank();
  unblank();
}

void commwait()
{
  Serial.println("M355 P1");

  goblank();
  LOLED.setFont(u8x8_font_8x13B_1x2_r);
  ROLED.setFont(u8x8_font_8x13B_1x2_r);
  LOLED.setCursor(2,6);
  ROLED.setCursor(5,6);
  LOLED.print(" Waiting for ");
  ROLED.print("printer");
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6,1);
  ROLED.setCursor(6,1);
  LOLED.print("F"); // comms icon in this font set
  ROLED.print("F"); // comms icon in this font set
  unblank();
  // Todo; some sort of animation while waiting..
  while ( !Serial.available() ) delay(100); // wait for serial, any serial
  goblank();
  unblank();
}

// Set the (non-linear) contrast level to off/low/mid/high (if supported)
bool setbrightness() 
{
  // TODO: remove blanking + return values from this section, only do brightness. handle blanking in main status loop

  // Only update if the brightnes level has changed, setting contrast causes a screen flicker whenever called.
  if ( bright != lastbright) {
    
    // 3 brightness levels low,mid,high, ignore all else
    if ( bright == 1 ) contrast = 1;
    else if ( bright == 2 ) contrast = 64;
    else if ( bright == 3 ) contrast = 255;

    // Sleep the screen if blank
    if ( bright == 0 ) {
      screensleep();
    } else {
      LOLED.setContrast(contrast);
      ROLED.setContrast(contrast);
    }
    
    // Wake screen if necesscary
    if ( lastbright == 0 ) screenwake();

    // remember what we just did
    lastbright = bright;
  }
  // return on/off status
  if ( bright > 0 ) return(true); else return(false); 
}

void updatedisplay()
{
  // Because redraws are slow and visible, the order of drawing here is deliberate
  // to ensure updates look 'smooth' to the user 

  // First update smaller status blocks
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);

  LOLED.setCursor(1,6);
  if (printerstate == 0 ) LOLED.print("Init    ");
  else if (printerstate == 1 ) LOLED.print("Boot    ");
  else if (printerstate == 2 ) LOLED.print("        "); // idle
  else if (printerstate == 3 ) LOLED.print("Printing"); // idle
  else LOLED.print("!COMERR!");
 
  ROLED.setCursor(0,6);
  ROLED.print("#: ");
  ROLED.print(noreply);
  ROLED.print("   ");

  if ((bedset < 0 ) || ( bedset > 999 ))
  { // blank when out of bounds
    LOLED.setCursor(10,6);
    LOLED.print("      ");
  }
  else
  { // Show target temp
    LOLED.setCursor(10,6);
    if ( bedset < 100 ) LOLED.print(" ");
    if ( bedset < 10 ) LOLED.print(" "); 
    LOLED.print("(");
    LOLED.print(bedset);
    LOLED.print(char(176)); // degrees symbol
    LOLED.print(")");
  }

  if ((toolset < 0 ) || ( toolset > 999))
  { // blank when out of bounds
    ROLED.setCursor(10,6);
    ROLED.print("      ");
  }
  else
  { // Show target temp
    ROLED.setCursor(10,6);
    if ( toolset < 100 ) ROLED.print(" ");
    if ( toolset < 10 ) ROLED.print(" "); 
    ROLED.print("(");
    ROLED.print(toolset);
    ROLED.print(char(176)); // degrees symbol
    ROLED.print(")");
  }

  // bed and head text
  LOLED.setCursor(10,0);
  LOLED.print("Bed");
  
  ROLED.setCursor(11,0);
  ROLED.print("E");
  ROLED.print(toolhead); 

  // Bed and Tool status icons
  if ( bedset == -1 )
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.setCursor(14,0);
    LOLED.print("N"); // power off icon in this font set
  }
  else
  {
    LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
    LOLED.setCursor(14,0);
    LOLED.print("N"); // heater icon in this font set
  }
  
  if ( toolset == -1 )
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.setCursor(14,0);
    ROLED.print("N"); // power off icon in this font set
  }
  else
  {
    ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
    ROLED.setCursor(14,0);
    ROLED.print("T"); // down arrow to line in this font set (looks a bit like a hotend..)
  }

  // Finally the main temps (slowest to redraw)
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0,0);
  if ( bedmain < 100 ) LOLED.print(" ");
  if ( bedmain < 10 ) LOLED.print(" ");
  LOLED.print(bedmain);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9,3);
  LOLED.print(".");
  LOLED.print(bedunits);
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.print(char(176));

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0,0);
  if ( toolmain < 100 ) ROLED.print(" ");
  if ( toolmain < 10 ) ROLED.print(" ");
  ROLED.print(toolmain);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9,3);
  ROLED.print(".");
  ROLED.print(toolunits);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.print(char(176));

}

int updateinterval = 500; // here 4 testing

void loop(void)
{
  // int updateinterval = 500; // Sleep time, 500ms when on, 2s when off

  if ( updateinterval == 500 ) // TODO: use printerstate to determine this
  {
    Serial.println("M355 P1");
  }
  else 
  {
    Serial.println("M355 P0");
  }
 

  #ifdef DEBUG
    Serial.print("DataRequest sent: (");
    Serial.print(noreply);
    Serial.print(" unreplied)");
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
    Serial.println();
  #endif

  // update brightness level as needed; returns true if screen is on, false if blank.
  if ( setbrightness() )
  {
    // if we are not blank, update the display with latest readings
    updatedisplay();
    updateinterval = 500; //  todo: set this from printer status, not here
  }
  else
  {
    updateinterval = 2000; //  todo: set this from printer status , not here 
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
      case '0': bright = 0; noreply = 0; break;
      case 'l': bright = 1; noreply = 0; break;
      case 'm': bright = 2; noreply = 0; break;
      case 'h': bright = 3; noreply = 0; break;
      case 'B': bedset = Serial.parseInt(); break;
      case 'T': toolset = Serial.parseInt(); break;
      case 'b': bedtemp = Serial.parseInt(); break;
      case 't': tooltemp = Serial.parseInt(); break;
      case 'P': printerstate = Serial.parseInt(); break;
    }
    while ( Serial.available() ) Serial.read(); // flush buffer
  }

  delay(updateinterval);

}
