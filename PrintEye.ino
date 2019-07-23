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
#include <U8x8lib.h>
#include "jsmn.h"

// During debug I enable the following to dump out the free memory during runtime.
// Taken from https://playground.arduino.cc/Code/AvailableMemory/
#include "MemoryFree.h" 

// Pinout
#define LED 13      // DEBUG: led on trinket, non pwm.
//#define LED 9      // pwm capable port
#define BUTTON 10  // pause button(s)
#define DEBOUNCE 30 // debounce+fatfinger delay for button  // Test if parsing succeeded.


// I2C #1
#define SDA1 A2
#define SCK1 A3
// I2C #2
#define SDA2 A4
#define SCK2 A5


// U8x8 Contructor List
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
// Please update the pin numbers according to your setup. Use U8X8_PIN_NONE if the reset pin is not connected
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ SCK1, /* data=*/ SDA1, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ SCK2, /* data=*/ SDA2, /* reset=*/ U8X8_PIN_NONE);   // Right OLED
//
// HW I2C works, but the ATMega 328P only has one HW interface available, so if your displays have address conflicts this
// can only be used for for one display; and the results looks weird and imbalanced. IMHO better to use two SW interfaces
//U8X8_SSD1306_128X64_NONAME_HW_I2C ROLED(/* reset=*/ U8X8_PIN_NONE);
// End of constructor list


// Primary Settings (can also be set via Json messages to serial port, see README)
int updateinterval = 1000;     // how many ~1ms loops we spend looking for a response after M408
int maxfail = 6;               // max failed requests before entering comms fail mode (-1 to disable)
bool powersave = true;         // Go into powersave when controller reports PSU off status 'O'
byte bright = 255;             // Screen brightness (0-255, sets OLED 'contrast',0 is off, not linear)
bool allowpause = true;        // Allow the button to trigger a pause event
byte activityled = 128;        // Activity LED brightness (0 to disable)
char ltext[11] = " Idle     "; // Left status line for the idle display (max 10 chars)
char rtext[11] = "          "; // Right status line for the idle display (max 10 chars)

// PrintEye
int noreply = 1;           // count failed requests (default: assume we have already missed some)
int currentbright = 0;     // track changes to brightness
bool screenpower = true;   // OLED power status

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

  // The button and the LED
  pinMode(LED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);

  // Does not speed up software I2C..
  //LOLED.setBusClock(400000);
  //ROLED.setBusClock(400000);

  // Displays
  LOLED.begin();
  LOLED.setContrast(0); // blank asap, screenbuffer may be full of crud
  LOLED.setFlipMode(1);
  ROLED.begin();
  ROLED.setContrast(0); // blank asap, screenbuffer may be full of crud
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


// Take the screen out of powersave and flash the power on icon

void screenwake()
{
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


// start the screen for the first time (splashscreen..)

void screenstart()
{
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

// Display the 'Waiting for Comms' splash

void commwait()
{
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
  
  bright = preservebright;
}


// Set the non-linear contrast/brightness level (if supported by display)

bool setbrightness()
{ 
  // Only update brightness when level has been changed
  // (setContrast can cause a screen flicker when called).
  
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

  // Max 10 chars for a status string
  if (printerstatus == 'O' )      LOLED.print(F(" Standby  "));
  else if (printerstatus == 'I' ) {LOLED.print(ltext); ROLED.print(rtext);}
  else if (printerstatus == 'P' ) LOLED.print(F(" Printing "));
  else if (printerstatus == 'S' ) LOLED.print(F(" Stopped  "));
  else if (printerstatus == 'C' ) LOLED.print(F(" Config   "));
  else if (printerstatus == 'A' ) LOLED.print(F(" Paused   "));
  else if (printerstatus == 'D' ) LOLED.print(F(" Pausing  "));
  else if (printerstatus == 'R' ) LOLED.print(F(" Resuming "));
  else if (printerstatus == 'B' ) LOLED.print(F(" Busy     "));
  else if (printerstatus == 'F' ) LOLED.print(F(" Updating "));
  else if (printerstatus == '-' ) LOLED.print(F("Connecting"));
  else                            LOLED.print(F("          ")); // show nothing if unknown.
    
  if ((printerstatus == 'P') || 
      (printerstatus == 'A') || 
      (printerstatus == 'D') || 
      (printerstatus == 'R'))
  { // Only display progress when needed
    ROLED.print(done);
    ROLED.print(F("%  "));
  }
  else if (printerstatus != 'I') 
  {
    ROLED.print(F("          "));
  }

  if ((bedset <= 0 ) || ( bedset > 999 ))
  { // blank when out of bounds
    LOLED.setCursor(10, 6);
    LOLED.print(F("      "));
  }
  else
  { // Show target temp
    LOLED.setCursor(10, 6);
    LOLED.print(F("  "));
    if ( bedset < 100 ) LOLED.print(" ");
    if ( bedset < 10 ) LOLED.print(" ");
    LOLED.print(bedset);
    LOLED.print(char(176)); // degrees symbol
  }

  if ((toolset <= 0 ) || ( toolset > 999))
  { // blank when out of bounds
    ROLED.setCursor(10, 6);
    ROLED.print(F("      "));
  }
  else
  { // Show target temp
    ROLED.setCursor(10, 6);
    ROLED.print(F("  "));
    if ( toolset < 100 ) ROLED.print(F(" "));
    if ( toolset < 10 ) ROLED.print(F(" "));
    ROLED.print(toolset);
    ROLED.print(char(176)); // degrees symbol
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

  //LOLED.setFont(u8x8_font_8x13B_1x2_f);
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

  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  //ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
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

  // Incoming data
  const int jsonSize = 512; // Maximum size of a M408 response we can process.
  static char json[jsonSize + 1];

  // Json
  const int maxtokens = 80; // Max number of distinct objects and values in the json (see jsmn docs)
  jsmn_parser jparser;
  static jsmntok_t jtokens[maxtokens];
  jsmn_init(&jparser);

  // We assume the firmware terminates the JSON response with a \n and no padding (eg.Duet/RRf).
  
  int index = Serial.readBytesUntil('\n',json,jsonSize);
  json[index] = '\0'; // Null terminate the string

  // return false if nothing arrived
  if ( index == 0 ) return (false);
  
  // return false if not initiated properly.
  if ( json[0] != '{' ) return (false);

  // return false if not terminated properly.
  if ( json[index-2] != '}' ) return (false);

  // DEBUG
  Serial.print(F("freeMemory pre = "));
  Serial.println(freeMemory());
  //Serial.print(F("Json : "));
  //Serial.println(json);
  Serial.print(F("Size : "));
  Serial.println(index); // (include the null since it is in memory too)

  // blink 
  digitalWrite(LED, activityled);

  // We have something that may be Json; process it for any keys we need.

  Serial.print("Running Processor On: ");
  for (int i=0;i<index;i++) Serial.print(json[i]);
  Serial.println();

  int parsed = jsmn_parse(&jparser, json, index+1, jtokens, maxtokens);

  Serial.print("Parser Return = ");
  Serial.println(parsed);

  
  if (parsed < 1 || jtokens[0].type != JSMN_OBJECT) 
  {
    Serial.println("Not a Json Object");
    return(false);
  }


  
  if (parsed == 1)
  {
    Serial.println("Empty Json Object");
    return(false);
  }

  for (int i = 1; i < parsed; i++) 
  {
    Serial.print(i);
    Serial.print(" : ");
    Serial.print(jtokens[i].type);
    Serial.print(" : ");
    Serial.print(jtokens[i].size);
    Serial.print(" : ");
    size_t result_len = jtokens[i].end-jtokens[i].start;
    char result[result_len+1];
    memcpy(result,json + jtokens[i].start,result_len);
    result[result_len] = 0;
    Serial.println(result);
    delay(10); // slow o/p down, my serial monitor glitches otherwise..
  }

  // Printer status
  //char* setstatus =  responsedoc[F("status")];
  //if (responsedoc.containsKey(F("status"))) 
  //{
  //  if ((printerstatus == 'I') && (setstatus[0] != 'I'))
  //  { // we are leaving Idle mode, clear idletext
  //    LOLED.clearLine(6);LOLED.clearLine(7);
  //    ROLED.clearLine(6);ROLED.clearLine(7);
  //  }
  //  printerstatus = setstatus[0];
  //}

  // Current Tool
  //signed int tool = responsedoc[F("tool")];
  //if ((tool >= 0) && (tool <= 99) && responsedoc.containsKey(F("status"))) toolhead = tool;

  // Actual Heater temps
  //float btemp = responsedoc[F("heaters")][0];
  //if (responsedoc.containsKey(F("heaters"))) 
  //{
  //  bedmain = btemp; // implicit cast to integer
  //  bedunits = (btemp - bedmain) * 10;
  //}
  
  //float etemp = responsedoc[F("heaters")][toolhead+1];
  //if (responsedoc.containsKey(F("heaters"))) 
  //{
  //  toolmain = etemp; // implicit cast to integer
  //  toolunits = (etemp - toolmain) * 10;
  //}

  // Active printer temp
  //float bset = responsedoc[F("active")][0];
  //if (responsedoc.containsKey(F("active"))) 
  //{
  //  bedset = bset; // implicit cast to integer
  //}
  
  //float eset = responsedoc[F("active")][toolhead+1];
  //if (responsedoc.containsKey(F("active"))) 
  //{
  //  toolset = eset; // implicit cast to integer
  //}

  // Print progress (simple %done metric)
  //float printed = responsedoc[F("fraction_printed")];
  //if (responsedoc.containsKey(F("fraction_printed"))) 
  //{
  //  done = printed * 100; // implicit cast to integer 
  //}

  // Set PrintEye Options via Json.
  //float interval = responsedoc[F("printeye_interval")];
  //if (responsedoc.containsKey(F("printeye_interval"))) 
  //{
  //  updateinterval = interval; // implicit cast to integer 
  //}

  //float fails = responsedoc[F("printeye_maxfail")];
  //if (responsedoc.containsKey(F("printeye_maxfail"))) 
  //{
  //  if (fails == -1) screenclean(); // cleanup for when this is set while 'waiting for printer'
  //  maxfail = fails; // implicit cast to integer 
  //}

  //float dim = responsedoc[F("printeye_brightness")];
  //if ((dim >= 0) && (dim <= 255) && responsedoc.containsKey(F("printeye_brightness"))) 
  //{
  //  bright = dim; // implicit cast to integer 
  //}

  //float pwr = responsedoc[F("printeye_powersave")];
  //if (responsedoc.containsKey(F("printeye_powersave")))
  //{
  //  powersave = pwr; // implicit cast to boolean 
  //}

  //char* left = responsedoc[F("printeye_idle_left")];
  //if (responsedoc.containsKey(F("printeye_idle_left")))
  //{
  //  byte s = strlen(left);
  //  for ( byte a = 0; a < 10; a++ ) 
  //  {
  //    if (a < s) ltext[a] = left[a]; else ltext[a] = ' ';
  //  }
  //  ltext[10]='\0'; // ensure terminated to avoid overruns when printing
  //}

  //char* right = responsedoc[F("printeye_idle_right")];
  //if (responsedoc.containsKey(F("printeye_idle_right")))
  //{
  //  byte s = strlen(right);
  //  for ( byte a = 0; a < 10; a++ ) 
  //  {
  //    if (a < s) rtext[a] = right[a]; else rtext[a] = ' ';
  //  }
  //  rtext[10]='\0'; // ensure terminated to avoid overruns when printing
  //}

  //float pausecontrol = responsedoc[F("printeye_allowpause")];
  //if (responsedoc.containsKey(F("printeye_allowpause")))
  //{
  //  allowpause = pausecontrol; // implicit cast to boolean 
  //}

  //float ledcontrol = responsedoc[F("printeye_activityled")];
  //if (responsedoc.containsKey(F("printeye_activityled")))
  //{
  //  activityled = ledcontrol; // implicit cast to byte 
  //}
  
  // DEBUG
  Serial.print(F("freeMemory post = "));
  Serial.println(freeMemory());

  // Finished processing
  digitalWrite(LED, 0);

  // Clear the screens if 'Waiting for Printer' message is currently being displayed
  if ((noreply >=  maxfail) && (maxfail != -1)) screenclean();
  
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
  // M408 S0 is the most basic info request, but has all the data we use
  Serial.println(F("M408 S0"));

  // This is the 'MAIN' loop where we spend most of our time waiting for
  // serial responses having sent the M408 request.
  
  for ( int counter = 0 ; counter < updateinterval ; counter++ )
  {
    // Anything in the serial buffer?
    if ( Serial.available() )
    {
      // We have something; peek at it and decide what to do, does it look like incoming Json?
      if (Serial.peek() == '{')
      { // It might be Json; read and parse it
        if (m408parser()) noreply = 0; // success; reset the fail count
      }
      else
      {
        Serial.read(); // not a Json start character, junk it and wait for another
      }
    }
    else
    {
      // wait a millisecond(ish) and loop
      delay(1);
    }
  }

  // we now either have updated data, or a timeout..

  if ( powersave ) // handle powersave mode, if enabled
  { 
    // Sleep the screen when firmware reports PSU off
    if (( printerstatus == 'O') && screenpower) screensleep();
 
    // Wake screen when printer regains power
    if (( printerstatus != 'O') && !screenpower) screenwake();
  }

  // Update Screen 
  //.. but first update brightness level as needed (returns true 
  // if screen is on, false if blank) 
  // Then test if we have had a response during this requestcycle, 
  // Determine whether we are in standby mode
  // update the display if needed
  if (setbrightness() && screenpower && (noreply == 0)) updatedisplay();

  // we always assume the next request will fail, m408paser() resets the count on success
  noreply++;
  if (maxfail != -1) {
    // once max number of failed requests is reached, show 'waiting for printer'
    if ( noreply == maxfail ) commwait();
  }

  // Start the next request cycle
}
