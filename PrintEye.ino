/*
  Printer Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)
  - performs a small, display-only, subset of panelDue functionality, uses same comms channel and port

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
*/

// FONT reference
// https://github.com/olikraus/u8g2/wiki/fntgrpiconic
// Duet reference
// https://duet3d.dozuki.com/Wiki/Duet_Wiring_Diagrams
// Gcode reference
// https://duet3d.dozuki.com/Wiki/Gcode#Section_M408_Report_JSON_style_response
// https://duet3d.dozuki.com/Wiki/Gcode#Section_Replies_from_the_RepRap_machine_to_the_host_computer
// https://duet3d.dozuki.com/Wiki/Gcode#Section_M575_Set_serial_comms_parameters
// https://duet3d.dozuki.com/Wiki/Gcode#Section_M118_Send_Message_to_Specific_Target
// https://reprap.org/forum/read.php?416,830988

// Maybe use for a 3.3v arduino with a 16Mhz cristal, note; affects serial baud rate..
// power&clock divider lib: https://arduino.stackexchange.com/a/5414
//#include <avr/power.h>

#include <Arduino.h>

// Ohshit, It's Jason.
#include <ArduinoJson.h>

// I allow ~450 chars for the incoming Json stream, currently I see max.400 chars in the M408
// Json response, this may change in the future but be very wary of increasing the buffer sizes;
// increasing these increases lowers stack/heap space and increases the chance of a memory error 
// while the response is being parsed.
StaticJsonDocument<450> responsedoc; // <== increase this at your peril.
                                     // You must also increase 'jsonSize' in m408parser()

// If you increase the above you can enable this tool to dump out the free memory during runtime.
// Some example dumps are in the 
//From https://playground.arduino.cc/Code/AvailableMemory/
//#include "tools/MemoryFree.h"

#include <U8x8lib.h>
// U8x8 Contructor List
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
// Please update the pin numbers according to your setup. Use U8X8_PIN_NONE if the reset pin is not connected
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ A3, /* data=*/ A2, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ A5, /* data=*/ A4, /* reset=*/ U8X8_PIN_NONE);   // Right OLED
// HW I2C works, but the ATMega 328P only has one HW interface available, so if your displays have address conflicts this
// can omly be used for for one display; and the results looks weird and imbalanced. IMHO better to use two SW interfaces
//U8X8_SSD1306_128X64_NONAME_HW_I2C ROLED(/* reset=*/ U8X8_PIN_NONE);
// End of constructor list


// Primary Settings          (can also be set via Json messages to serial port, see README)
int updateinterval = 1000; // how many ~1ms loops we spend looking for a response after M408
int maxfail = 4;           // max failed requests before entering comms fail mode (-1 to disable)
bool powersave = true;     // Go into powersave when controller reports status 'O' (PSU off)
int bright = 255;          // Screen brightness (0-255, sets OLED 'contrast', not very linear imho.
char ltext[17] = " Idle           "; // Left status line for the idle display
char rtext[17] = "                "; // Right status line for the idle display

// PrintEye
int noreply = 1;           // count failed requests (start by assuming we have missed some replies)
int currentbright = 0;     // track changes to brightness
bool screenpower = true;   // OLED on/off 

// Json response data:
char printerstatus = '-';  // from m408 status key, default '-' is shown as 'connecting'
int bedset = 0;           // Bed target temp
int toolset = 0;          // Tool target temp
int toolhead = 0;          // Tool to be monitored (assume E0 by default)
int done = 0;             // Percentage printed

// Main temp display is derived from Json values, split into the integer value and it's decimal
int bedmain = 0;
int bedunits = 0;
int toolmain = 0;
int toolunits = 0;


//   ____       _
//  / ___|  ___| |_ _   _ _ __
//  \___ \ / _ \ __| | | | '_ \ 
//   ___) |  __/ |_| |_| | |_) |
//  |____/ \___|\__|\__,_| .__/
//                       |_|

void setup()
{
  // 3.3v ATMega with 16Mhz crystal: 
  // This is an option to avoid using level converters while still using that 
  // 'spare' 328P+crystal you have in the parts bin..
  // It relies on the fact that the 328P can probably boot to here OK even 
  // though the recommended maximum clock at 3.3v is 12Mhz. This is largely 
  // temperature/load dependent; plenty of people have done it successfully
  // for light loads/cool conditions. But some chips are more on the edge
  // than others (we are Overclocking, in principle) so YMMV. ;-)
  //
  // Drop to 8MHz since that is in-spec for a ATMega328P @ 3.3v
  // clock_prescale_set(clock_div_2);
  // nb: If you enable this, you also need to halve the serial speed below since 
  //     the Serial port clock is affected by this change.

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

void screensleep()
{ // flash power off icon, blank the screen and turn on powersave
  goblank();
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print(F("N")); // power off icon in this font set
  ROLED.print(F("N")); // power off icon in this font set
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
  LOLED.print(F("O")); // Resume icon in this font set
  ROLED.print(F("O")); // Resume icon in this font set
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
  LOLED.print(F("C")); // Power bolt icon in this font set
  ROLED.print(F("C")); // Power bolt icon in this font set
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.setCursor(3, 6);
  ROLED.setCursor(3, 6);
  LOLED.print(F(" PrintEye "));
  ROLED.print(F(" by Owen "));
  unblank();
}

void commwait()
{ // Display the 'Waiting for Comms' splash
  int preservebright = bright;
  if (bright == 0) bright = 1; // show 'waiting' even if 'blank'
  goblank();
  
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.setCursor(2, 6);
  ROLED.setCursor(5, 6);
  LOLED.print(F(" Waiting for "));
  ROLED.print(F("Printer"));
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print(F("F")); // comms icon in this font set
  ROLED.print(F("F")); // comms icon in this font set

  // screen on (even in powersave mode)
  LOLED.setPowerSave(false); 
  ROLED.setPowerSave(false);
  unblank();
  
  delay(333); // a brief pause while animation is displayed
  bright = preservebright;
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

  // First update lower status line
  
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);

  LOLED.setCursor(0, 6);
  ROLED.setCursor(0, 6);
  
  if (printerstatus == 'O' )      LOLED.print(F(" PSU Off "));
  else if (printerstatus == 'I' ) { LOLED.print(ltext); ROLED.print(rtext); }
  else if (printerstatus == 'P' ) LOLED.print(F(" Printing"));
  else if (printerstatus == 'S' ) LOLED.print(F(" Stopped "));
  else if (printerstatus == 'C' ) LOLED.print(F(" Config  "));
  else if (printerstatus == 'A' ) LOLED.print(F(" Paused  "));
  else if (printerstatus == 'D' ) LOLED.print(F(" Pausing "));
  else if (printerstatus == 'R' ) LOLED.print(F(" Resuming"));
  else if (printerstatus == 'B' ) LOLED.print(F(" Busy    "));
  else if (printerstatus == 'F' ) LOLED.print(F(" Updating"));
  else if (printerstatus == '-' ) LOLED.print(F("Connecting"));
  else                            LOLED.print(F("          ")); // show nothing if unknown.

  if ( printerstatus != 'I' )
  { // dont overwrite the status line when in Idle mode
    
    if ((printerstatus == 'P') || (printerstatus == 'A') || 
        (printerstatus == 'D') || (printerstatus == 'R'))
    { // Only display progress when needed
      ROLED.print(done);
      ROLED.print(F("%  "));
    }
    else
    {
      ROLED.print(F("      "));
    }
  
    if ((bedset <= 0 ) || ( bedset > 999 ))
    { // blank when out of bounds
      LOLED.setCursor(10, 6);
      LOLED.print(F("      "));
    }
    else
    { // Show target temp
      LOLED.setCursor(10, 6);
      if ( bedset < 100 ) LOLED.print(" ");
      if ( bedset < 10 ) LOLED.print(" ");
      LOLED.print(F("("));
      LOLED.print(bedset);
      LOLED.print(char(176)); // degrees symbol
      LOLED.print(F(")"));
    }
  
    if ((toolset <= 0 ) || ( toolset > 999))
    { // blank when out of bounds
      ROLED.setCursor(10, 6);
      ROLED.print(F("      "));
    }
    else
    { // Show target temp
      ROLED.setCursor(10, 6);
      if ( toolset < 100 ) ROLED.print(F(" "));
      if ( toolset < 10 ) ROLED.print(F(" "));
      ROLED.print(F("("));
      ROLED.print(toolset);
      ROLED.print(char(176)); // degrees symbol
      ROLED.print(F(")"));
    }
  }

  // bed and head text
  LOLED.setCursor(10, 0);
  LOLED.print(F("Bed"));

  ROLED.setCursor(11, 0);
  ROLED.print(F("E"));
  ROLED.print(toolhead);
  if (toolhead < 10) ROLED.print(F(" "));
  
  // Bed and Tool status icons
  if ( bedset <= 0 )
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print(F("N")); // power off icon in this font set
  }
  else
  {
    LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print(F("N")); // heater icon in this font set
  }

  if ( toolset <= 0 )
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print(F("N")); // power off icon in this font set
  }
  else
  {
    ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print(F("T")); // down arrow to line in this font set (looks a bit like a hotend..)
  }

  // Finally the main temps (slowest to redraw)
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0, 0);
  if ( bedmain < 100 ) LOLED.print(F(" "));
  if ( bedmain < 10 ) LOLED.print(F(" "));
  LOLED.print(bedmain);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9, 3);
  LOLED.print(F("."));
  LOLED.print(bedunits);
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.print(char(176));

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0, 0);
  if ( toolmain < 100 ) ROLED.print(F(" "));
  if ( toolmain < 10 ) ROLED.print(F(" "));
  ROLED.print(toolmain);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9, 3);
  ROLED.print(F("."));
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
{ // parse a M408 result; or set stuff, or fail.

  const int jsonSize = 450; // <== Before you increase this read the important notes about 
                            // Json buffers, StaticJsonDocument, and memory at the head of this file
  static char json[jsonSize + 1];

  // nb: we assume the firmware properly terminates the JSON response with a \n here.
  int index = Serial.readBytesUntil('\n',json,jsonSize);
  json[index] = '\0'; // Null terminate the string

  // return false of nothing arrived
  if ( index == 0 ) return (false);
  
  // return false if not terminated properly.
  if ( json[index-2] != '}' ) return (false);

  // DEBUG
  //Serial.print(F("freeMemory pre = "));
  //Serial.println(freeMemory());
  //Serial.print(F("Json : "));
  //Serial.println(json);
  //Serial.print(F("Size : "));
  //Serial.println(index); // (include the null since it is in memory too)

  DeserializationError error = deserializeJson(responsedoc, json);
  // Test if parsing succeeded.
  if (error) {
    // DEBUG
    //Serial.print(F("deserializeJson() failed: "));
    //Serial.println(error.c_str());
    return(false);
  }

  // Printer status
  char* setstatus =  responsedoc[F("status")];
  if (responsedoc.containsKey(F("status"))) 
  {
    if ((printerstatus == 'I') && (setstatus[0] != 'I'))
    { // we are leaving Idle mode, clear idletext
      //LOLED.clearLine(6); Importing the clearLine function used a lot more program memory
      //ROLED.clearLine(6); than just clearing the line with a functions we already use.
      LOLED.setFont(u8x8_font_8x13B_1x2_f);
      ROLED.setFont(u8x8_font_8x13B_1x2_f);
      LOLED.setCursor(0, 6);
      ROLED.setCursor(0, 6);
      LOLED.print(F("                "));
      ROLED.print(F("                "));
    }
    printerstatus = setstatus[0];
  }

  // Current Tool
  signed int tool = responsedoc[F("tool")];
  if ((tool >= 0) && (tool <= 99) && responsedoc.containsKey(F("status"))) toolhead = tool;

  // Actual Heater temps
  float btemp = responsedoc[F("heaters")][0];
  if (responsedoc.containsKey(F("heaters"))) 
  {
    bedmain = btemp; // implicit cast to integer
    bedunits = (btemp - bedmain) * 10;
  }
  
  float etemp = responsedoc[F("heaters")][toolhead+1];
  if (responsedoc.containsKey(F("heaters"))) 
  {
    toolmain = etemp; // implicit cast to integer
    toolunits = (etemp - toolmain) * 10;
  }

  // Active printer temp
  float bset = responsedoc[F("active")][0];
  if (responsedoc.containsKey(F("active"))) 
  {
    bedset = bset; // implicit cast to integer
  }
  
  float eset = responsedoc[F("active")][toolhead+1];
  if (responsedoc.containsKey(F("active"))) 
  {
    toolset = eset; // implicit cast to integer
  }

  // Print progress (simple %done metric)
  float printed = responsedoc[F("fraction_printed")];
  if (responsedoc.containsKey(F("fraction_printed"))) 
  {
    done = printed * 100; // implicit cast to integer 
  }

  // Set PrintEye Options via Json.
  float interval = responsedoc[F("printeye_interval")];
  if (responsedoc.containsKey(F("printeye_interval"))) 
  {
    updateinterval = interval; // implicit cast to integer 
  }

  float fails = responsedoc[F("printeye_maxfail")];
  if (responsedoc.containsKey(F("printeye_maxfail"))) 
  {
    if (fails == -1) screenclean(); // cleanup for when this is set while 'waiting for printer'
    maxfail = fails; // implicit cast to integer 
  }

  float dim = responsedoc[F("printeye_brightness")];
  if ((dim >= 0) && (dim <= 255) && responsedoc.containsKey(F("printeye_brightness"))) 
  {
    bright = dim; // implicit cast to integer 
  }

  float pwr = responsedoc[F("printeye_powersave")];
  if (responsedoc.containsKey(F("printeye_powersave")))
  {
    powersave = pwr; // implicit cast to boolean 
  }

  char* newtext = responsedoc[F("printeye_idletext")];
  if (responsedoc.containsKey(F("printeye_idletext")))
  {
    for ( byte a = 0; a < 16; a++ )
    {
      ltext[a] = newtext[a];
      rtext[a] = newtext[a+16];
    }
  }

 // DEBUG
  //Serial.print(F("freeMemory post = "));
  //Serial.println(freeMemory());

  // Clear the screens if waiting for printer message is displayed
  if ((noreply >=  maxfail) && (maxfail != -1)) screenclean();
  
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
  Serial.println(F("M408 P0"));

  // This is the 'MAIN' loop where we spend most of our time waiting for
  // serial responses having sent the M408 request.
  for ( int counter = 0 ; counter < updateinterval ; counter++ )
  {
    if ( Serial.available() )
    {
      // We have something; peek at it and decide what to do, does it look like a Json line?
      if (Serial.peek() == '{')
      { // It might be Json; parse it
        if ( !m408parser() ) Serial.read(); // not a valid char, clean from buffer
      }
    }
    else
    {
      // wait a millisecond(ish) and loop
      delay(1);
    }
  }

  // we now either have updated data, or a timeout..

  if ( powersave ) // handle 'standby' mode 
  { 
    // Sleep the screen as necesscary when firmware reports PSU off
    if (( printerstatus == 'O') && screenpower) screensleep();
 
    // Wake screen as necesscary when printer has power
    if (( printerstatus != 'O') && !screenpower) screenwake();
  }

  // Update Screen! .. but first update brightness level as needed (returns true 
  // if screen is on, false if blank) and then test if we have had a response
  // during this cycle, and wether PrintEye is in standby mode
  // If all above is good, update the display.
  if (setbrightness() && screenpower && (noreply == 0)) updatedisplay();

  // always assume the next request will fail, json parser resets the count on success
  noreply++;
  if (maxfail != -1) {
    // once max number of failed requests is reached, show 'waiting for printer'
    if ( noreply == maxfail ) commwait();
  }

  // And start the next request cycle
}
