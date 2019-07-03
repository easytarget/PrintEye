/*
  RepRap Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)
  - performs a small, display-only, subset of panelDue functionality, uses same comms channel and port

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
*/

// FONT reference
// https://github.com/olikraus/u8g2/wiki/fntgrpiconic
// Duet reference
// https://duet3d.dozuki.com/Wiki/Duet_Wiring_Diagrams
// https://duet3d.dozuki.com/Wiki/Gcode#Section_M408_Report_JSON_style_response
// https://reprap.org/forum/read.php?416,830988

// Maybe use for a 3.3v arduino with a 16Mhz cristal, note; affects serial baud rate..
// power&clock divider lib: https://arduino.stackexchange.com/a/5414
//#include <avr/power.h>

#include <Arduino.h>
#include <U8x8lib.h>

// Sigh, It's Jason
//#include <Arduino_JSON.h>

// debug : To enable add MemoryFree.c & .h from https://playground.arduino.cc/Code/AvailableMemory/ to sketch
#include "MemoryFree.h"

// U8x8 Contructor List
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
// Please update the pin numbers according to your setup. Use U8X8_PIN_NONE if the reset pin is not connected
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ A3, /* data=*/ A2, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ A5, /* data=*/ A4, /* reset=*/ U8X8_PIN_NONE);   // Right OLED
// HW I2C works, but the ATMega 328P only has one HW interface available, so if your displays have address conflicts this
// can omly be used for for one display; and the results looks weird and imbalanced. IMHO better to use two SW interfaces
//U8X8_SSD1306_128X64_NONAME_HW_I2C ROLED(/* reset=*/ U8X8_PIN_NONE);
// End of constructor list


// DEBUG data to be used until proper display processing is implemented
int bedmain = 12;
int bedunits = 3;
int toolmain = 123;
int toolunits = 4;

// JSON response data: (-1 if unset/unspecified)
//int bedtemp = 35;   // Current bed temp
int bedset = -1;   // Bed target temp
//int tooltemp = 101; // latest tool temp
int toolset = -1;  // Tool target temp
int toolhead = 0;   // Tool to be monitored

// Master printer status (from JSON)
char printerstatus = "-"; // from m408 status key, '-' means unread
int updateinterval = 1000; // how many ~1ms loops we spend looking for a response after M408
int noreply = 1;           // count failed requests

// PrintEye
int bright = 255; // track requested brightness
int currentbright = 0; // and what we actually have
bool screenpower = true; // OLED on/off 
int maxfail = 4; // max failed requests before comms fail mode (-1 to disable)

//   ____       _
//  / ___|  ___| |_ _   _ _ __
//  \___ \ / _ \ __| | | | '_ \ 
//   ___) |  __/ |_| |_| | |_) |
//  |____/ \___|\__|\__,_| .__/
//                       |_|

void setup(void)
{
  // First; lets drop to 8MHz since that is in-spec for a ATMega328P @ 3.3v
  // clock_prescale_set(clock_div_2);

  // Some serial is needed
  Serial.begin(57600); // DUET default is 57600,
  Serial.setTimeout(500); // give the printer max 500ms to send complete json block

  // Displays
  LOLED.begin();
  LOLED.setContrast(0); // blank asap, screenbuffer often full of crud
  LOLED.setFlipMode(1);
  ROLED.begin();
  ROLED.setContrast(0); // blank asap, screenbuffer often full of crud
  ROLED.setFlipMode(1);
  goblank();

  screenstart(); // Splash Screen
  delay(2500); // For 2.5 seconds
  commwait(); // Now wait for first response
}


//   ____
//  / ___|  ___ _ __ ___  ___ _ __
//  \___ \ / __| '__/ _ \/ _ \ '_ \ 
//   ___) | (__| | |  __/  __/ | | |
//  |____/ \___|_|  \___|\___|_| |_|

void screenclean()
{ // clear both oleds, used in animations
  LOLED.clear();
  ROLED.clear();

}
void goblank()
{ // darken screen then erase contents, used in animations
  LOLED.setContrast(0);
  ROLED.setContrast(0);
  screenclean();
}

void unblank()
{ // undark the screen, used in animations
  LOLED.setContrast(bright);
  ROLED.setContrast(bright);
}

void screensleep(void)
{ // flash power off icon, blank the screen and turn on powersave
  goblank();
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print("N"); // power off icon in this font set
  ROLED.print("N"); // power off icon in this font set
  unblank();
  delay(666); // flash the power off icons
  goblank();
  LOLED.setPowerSave(true);
  ROLED.setPowerSave(true);
  screenpower = false;
}

void screenwake()
{ // Take the screen out of powersave and flash the power on icon
  goblank();
  LOLED.setPowerSave(false);
  ROLED.setPowerSave(false);
  screenpower = true;
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print("O"); // Resume icon in this font set
  ROLED.print("O"); // Resume icon in this font set
  unblank();
  delay(666); // flash the icons then clean and reset screen
  goblank();
  unblank();
  noreply = 0;
}

void screenstart()
{ // start the screen for the first time (splashscreen..)
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print("C"); // Power bolt icon in this font set
  ROLED.print("C"); // Power bolt icon in this font set
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.setCursor(3, 6);
  ROLED.setCursor(3, 6);
  LOLED.print(" PrintEye ");
  ROLED.print(" by Owen ");
  unblank();
}

void commwait()
{
  int preservebright = bright;
  bright = 255;
  goblank();
  
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.setCursor(2, 6);
  ROLED.setCursor(5, 6);
  LOLED.print(" Waiting for ");
  ROLED.print("printer");
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print("F"); // comms icon in this font set
  ROLED.print("F"); // comms icon in this font set

  // screen on (even in powersave mode)
  LOLED.setPowerSave(false); 
  ROLED.setPowerSave(false);
  unblank();
  
  delay(333); // a brief pause while animation is displayed
  // Todo; repeat sending M408 and some sort of animation while waiting..
  Serial.print("CommWait - ");
  Serial.println("M408 P0");
  while ( !Serial.available() ) delay(1); // wait for serial, any serial
  noreply = 0;
  goblank();
  bright = preservebright;
  if (screenpower) 
  { // turn screens off again if in powersave mode
    LOLED.setPowerSave(false); 
    ROLED.setPowerSave(false);
  }
  unblank();
}

bool setbrightness()
{ // Set the (non-linear) contrast (brightness) level (if supported)

  // Only update if the brightnes level has changed because
  // setContrast can cause a screen flicker when called.
  
  if ( bright != currentbright ) {
    // limits
    if ( bright > 255 ) bright = 255;
    if ( bright < 0) bright = 0;
    
    // Set the new contrast value
    LOLED.setContrast(bright);
    ROLED.setContrast(bright);

    // remember what we just did
    currentbright = bright;
  }
  
  // return on/off status
  if ( bright > 0 ) return (true); else return (false);
}


//   _   _           _       _       
//  | | | |_ __   __| | __ _| |_ ___ 
//  | | | | '_ \ / _` |/ _` | __/ _ \
//  | |_| | |_) | (_| | (_| | ||  __/
//   \___/| .__/ \__,_|\__,_|\__\___|
//        |_|                        

void updatedisplay()
{
  // Because redraws are slow and visible, the order of drawing here is deliberate
  // to ensure updates look 'smooth' to the user

  // First update smaller status blocks
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);

  LOLED.setCursor(1, 6);
  if (printerstatus == 'O' ) LOLED.print("Poweroff");
  else if (printerstatus == 'B' ) LOLED.print("Busy    ");
  else if (printerstatus == 'I' ) LOLED.print("Init    "); // idle
  else if (printerstatus == 'P' ) LOLED.print("Printing"); 
  else LOLED.print("!COMERR!");

  ROLED.setCursor(0, 6);
  ROLED.print("#: ");
  ROLED.print(noreply);
  ROLED.print("   ");

  if ((bedset < 0 ) || ( bedset > 999 ))
  { // blank when out of bounds
    LOLED.setCursor(10, 6);
    LOLED.print("      ");
  }
  else
  { // Show target temp
    LOLED.setCursor(10, 6);
    if ( bedset < 100 ) LOLED.print(" ");
    if ( bedset < 10 ) LOLED.print(" ");
    LOLED.print("(");
    LOLED.print(bedset);
    LOLED.print(char(176)); // degrees symbol
    LOLED.print(")");
  }

  if ((toolset < 0 ) || ( toolset > 999))
  { // blank when out of bounds
    ROLED.setCursor(10, 6);
    ROLED.print("      ");
  }
  else
  { // Show target temp
    ROLED.setCursor(10, 6);
    if ( toolset < 100 ) ROLED.print(" ");
    if ( toolset < 10 ) ROLED.print(" ");
    ROLED.print("(");
    ROLED.print(toolset);
    ROLED.print(char(176)); // degrees symbol
    ROLED.print(")");
  }

  // bed and head text
  LOLED.setCursor(10, 0);
  LOLED.print("Bed");

  ROLED.setCursor(11, 0);
  ROLED.print("E");
  ROLED.print(toolhead);

  // Bed and Tool status icons
  if ( bedset == -1 )
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print("N"); // power off icon in this font set
  }
  else
  {
    LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print("N"); // heater icon in this font set
  }

  if ( toolset == -1 )
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print("N"); // power off icon in this font set
  }
  else
  {
    ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print("T"); // down arrow to line in this font set (looks a bit like a hotend..)
  }

  // Finally the main temps (slowest to redraw)
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0, 0);
  if ( bedmain < 100 ) LOLED.print(" ");
  if ( bedmain < 10 ) LOLED.print(" ");
  LOLED.print(bedmain);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9, 3);
  LOLED.print(".");
  LOLED.print(bedunits);
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.print(char(176));

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0, 0);
  if ( toolmain < 100 ) ROLED.print(" ");
  if ( toolmain < 10 ) ROLED.print(" ");
  ROLED.print(toolmain);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9, 3);
  ROLED.print(".");
  ROLED.print(toolunits);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.print(char(176));

}


//       _
//      | |___  ___  _ __
//   _  | / __|/ _ \| '_ \ 
//  | |_| \__ \ (_) | | | |
//   \___/|___/\___/|_| |_|

bool m408parser()
{ // parse a M408 result; or give an error.
  // M408 is FLAT, no recursive sections, so we can read until we see a 
  // closing brace '}' without breaking.
  const int jsonSize = 512;
  char json[jsonSize + 1];

  int index = Serial.readBytesUntil('\n',json,jsonSize);
  json[index] = '\0'; // Null terminate the string

  // return false of nothing arrived
  if ( index == 0 ) return (false);
  
  // return false if not terminated properly.
  if ( json[index-2] != '}' ) return (false);
  
  // DEBUG O/P
  Serial.print("Jason Sez:");
  Serial.println(json);
  Serial.print("Length = ");
  Serial.print(index); // counts from zero..
  Serial.print(" : Progmem = ");
  Serial.println(freeMemory());

  noreply = 0; // success; reset the fail count
  return(true);
}


//   _                      
//  | |    ___   ___  _ __  
//  | |   / _ \ / _ \| '_ \ 
//  | |__| (_) | (_) | |_) |
//  |_____\___/ \___/| .__/ 
//                   |_|    

void loop(void)
{
  // Begin with the data request to the RepRap controller
  // P0 is the most basic info request, but has all the data we use
  Serial.println("M408 P0");

  // This is the 'MAIN' loop where we spend most of our time waiting for
  // serial responses having sent the M408 request.
  for ( int counter = 0 ; counter < updateinterval ; counter++ )
  {
    if ( Serial.available() )
    {
      // we have something; peek at it and decide what to do
      switch (Serial.peek())
      {
        // If it begins with a brace, assume Json
        case '{': if ( m408parser() ) break;

        // DEBUG : do all the below with JSON.
        // Light control, take the next character as command
        case '=': Serial.read(); switch (Serial.read())
          {
            case 'o': bright = 0; break;
            case 'l': bright = 1; break;
            case 'm': bright = 64; break;
            case 'h': bright = 255; break;
            case 'c': bright = Serial.parseInt(); break;
          }
          noreply = 0; 
          break; 

        // DEBUG: options below are useful for testing
        case 'B': Serial.read(); bedset = Serial.parseInt(); noreply = 0; break;
        case 'T': Serial.read(); toolset = Serial.parseInt(); noreply = 0; break;
        //case 'b': Serial.read(); bedtemp = Serial.parseInt(); noreply = 0; break;
        //case 't': Serial.read(); tooltemp = Serial.parseInt(); noreply = 0; break;
        case 'A': Serial.read(); toolhead = Serial.parseInt(); noreply = 0; break;
        case 'P': Serial.read(); printerstatus = Serial.read(); noreply = 0; break;
        case 'F': Serial.read(); maxfail = Serial.parseInt(); noreply = 0; break;
        
        default : Serial.read(); // invalid, clean from buffer
      }
    }
    else
    {
      // wait a millisecond(ish) and loop
      delay(1);
    }
  }

  // we now either have updated data, or a timeout..

  // Sleep the screen as necesscary when firmware reports PSU off
  if (( printerstatus == 'O') && screenpower) screensleep();

  // Wake screen as necesscary when printer has power
  if (( printerstatus != 'O') && !screenpower) screenwake();

  // Update brightness level as needed (returns true if screen is on, false if blank)
  // test if we have had a response, and are powered.
  // If all above is good, update the display.
  if (setbrightness() && screenpower && (noreply == 0)) updatedisplay();

  // always assume the next request will fail, json parser resets the count on success
  noreply++;
  if (maxfail == 0) {
    // once max number of failed requests is reached, go to 'waiting for comms' mode.
    if ( noreply >= maxfail ) commwait();
  }
  
  // DEBUG increment counters for demo
  toolmain += rand() % 16;
  toolunits = rand() % 10;
  if ( toolmain > 310 ) toolmain = 8;
  bedmain += rand() % 4;
  bedunits = rand() % 10;
  if ( bedmain > 110 ) bedmain = 15;

}
