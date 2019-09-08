/*
  Printer Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
  Grocking the Json response with <1k of Ram left after the screens have gobbled a load 
  is courtesy of Jsmn (https://zserge.com/jsmn.html)
*/

// This gives useful debug on json processing; but eats a bit of memory..
// only enable as needed.
//#define DEBUG

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

// Following might be useful for a 3.3v arduino with a 16Mhz cristal. Nb; affects serial baud rate..
// power&clock divider lib: https://arduino.stackexchange.com/a/5414
//#include <avr/power.h>
// also see the setup() function; where it needs enabling.

#include <Arduino.h>
#include <U8x8lib.h>
#include "jsmn.h"

// Pinout
#define LED 9          // pwm capable O/P for the led
#define BUTTON 10      // pause button pin

// I2C Left display
#define SDA1 A2
#define SCK1 A3

// I2C Right display
#define SDA2 A4
#define SCK2 A5


// Some important limits and allocations
// - what you set here must stay under available ram 
// - Allow 150 bytes or so free for allocations during processing and screen updates (found by experiment)
// Heater settings are good for a 4 extruder system + bed, everything else is ignored.
// - Each additional heater adds 8 bytes in global arrays, 4 additional tokens, and ~20 characters of extra Json
//
#define JSONSIZE 500    // Json incoming buffer size
#define MAXTOKENS 86    // Maximum number of jsmn tokens we can handle (see jsmn docs, 8 bytes/token)
#define HEATERS 5       // Bed + Up to 4 extruders
#define JSONWINDOW 500; // How many ms we allow for the rest of the object to arrive after the '{' is recieved

// U8x8 Contructors for my displays and wiring
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ SCK1, /* data=*/ SDA1, /* reset=*/ U8X8_PIN_NONE);   // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ SCK2, /* data=*/ SDA2, /* reset=*/ U8X8_PIN_NONE);   // Right OLED
//   Hardware I2C works, but the ATMega 328P only has one HW interface available, so if your displays have address conflicts 
//   (like my cheap ones do) a trick is to use two SW interfaces.
//   Software (bit-bang)is slow, but so is the update frequency from the controller

// During debug I enable this to monitor the free memory during runtime
#ifdef DEBUG 
  #include "MemoryFree.h" // Taken from https://playground.arduino.cc/Code/AvailableMemory/
#endif

// Incoming data

const int jsonSize = JSONSIZE;  // Maximum size of a M408 response we can process.
static char json[jsonSize + 1]; // Json response
static int index;               // length of the response


// Primary Settings (can also be set via Json messages to serial port, see README)
int updateinterval = 1000;     // how many ~1ms loops we spend looking for a response after M408
int maxfail = 6;               // max failed requests before entering comms fail mode (-1 to disable)
bool screensave = true;        // Go into screensave when controller reports PSU off (status 'O')
byte bright = 255;             // Screen brightness (0-255, sets OLED 'contrast',0 is off, not linear)
int pausecontrol = 333;        // Hold-down delay for pause, 0=disabled, max=(updateinterval-100)
byte activityled = 128;        // Activity LED brightness (0 to disable)
char ltext[11] = "SHOWSTATUS"; // Left status line for the idle display (max 10 chars)
char rtext[11] = "          "; // Right status line for the idle display (max 10 chars)

// PrintEye internal 
int noreply = 1;           // count failed requests (default: assume we have already missed some)
int currentbright = 0;     // track changes to brightness
bool screenpower = true;   // OLED power status

// Json derived data:
char printerstatus = '-';  // from m408 status key, initial value '-' is shown as 'connecting'
int toolhead = 0;          // Tool to be monitored (assume E0 by default)
int done = 0;             // Percentage printed

// Heater active, standby and status values for all possible heaters ([0] = bed, [1] = E0, [2] = E1, etc)
int heateractive[HEATERS];
int heaterstandby[HEATERS];
byte heaterstatus[HEATERS];
// Main temp display is derived from Json values, split into the integer value and it's decimal
int heaterinteger[HEATERS];
byte heaterdecimal[HEATERS];

// A time store for the pause button
unsigned long pausetimer = 0;


/*    Setup    */

void setup()
{
  // 3.3v ATMega with 16Mhz crystal? Drop to 8MHz!
  // This is an option to avoid using level converters while still using that 
  // 'spare' 328P+crystal you have in the parts bin..
  // It relies on the fact that the 328P can probably boot to here OK even 
  // though the recommended maximum clock at 3.3v is 12Mhz. This is largely 
  // temperature/load dependent; plenty of people have done it successfully
  // for light loads/cool conditions. But some chips are more on the edge
  // than others (we are Overclocking, in principle) so YMMV. ;-)
  // nb: If you enable this, you also need to halve the serial speed below since 
  //     the Serial port clock is affected by this change.
  // clock_prescale_set(clock_div_2);

  // The button and the LED
  pinMode(LED, OUTPUT);
  pinMode(BUTTON, INPUT_PULLUP);
  analogWrite(LED, activityled); // blip the LED on while setup runs

  // Get the LED's initialised  asap, begin() will also blank them
  LOLED.begin();
  LOLED.setFlipMode(1);   // as needed
  ROLED.begin();
  ROLED.setFlipMode(1);   // as needed

  // Some serial is needed
  Serial.begin(57600); // DUET default is 57600,
  delay(50);

  #ifdef DEBUG
    Serial.println(F("Debug Enabled"));
    Serial.println();
    Serial.println(F("Try: {\"status\":\"I\",\"pe_rate\":5000,\"pe_fails\":-1}"));
    Serial.println();
  #endif
  
  // Set all heater values to off by default.
  for (int a = 0; a < HEATERS; a++)
  {
    heateractive[a] = 0;
    heaterstandby[a] = 0;
    heaterstatus[a] = 0;
    heaterinteger[a] = 0;
    heaterdecimal[a] = 0;
  }

  // Displays
  splashscreen();         // Splash Screen
  //delay(2400);            // For 2.5 seconds   ---- Try with this off.. get processing Json asap.
  analogWrite(LED, 0);    // turn the led off
  //screenclean(); //                            ---- Try with this off.. get processing Json asap.
}


/*    Screen Handlers    */

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

// Flash power off icon, blank the screen and turn on powersave
void screensleep()
{
  goblank();
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.setCursor(6, 1);
  ROLED.setCursor(6, 1);
  LOLED.print(F("N")); // power off icon in this font set
  ROLED.print(F("N")); // power off icon in this font set
  unblank();
  delay(500); // flash the power off icons
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
  delay(500); // flash the icons then clean and reset screen
  goblank();
  unblank();
}


// Startup splashscreen
void splashscreen()
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

  // screen on (even in screensave mode)
  LOLED.setPowerSave(false); 
  ROLED.setPowerSave(false);

  int preservebright = bright;
  if (bright == 0) bright = 1; // display even when the screen is normally blanked
  unblank();
  bright = preservebright;
}

// Set the (non-linear) contrast/brightness level for supported displays
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


/*   Update The Displays   */ 

void updatedisplay()
{
  // Called every time a valid Json response is recieved to update the screen with the 
  // current printer state. (not called during screensave)
  
  // Because redraws are slow and visible, the order of drawing here is deliberate
  // to ensure updates look smooth and animated to the user
  // There is no screenbuffer (memory, again) so we overdraw every active screen element 
  // on each update, and have logic to blank areas when they are inactive.
  // - This all looks very cumbersome in code, cest'la'vie!
  // - We also regularly call the pause button handler

  int bedset = 0;
  int toolset = 0;

  // First update lower status line
  
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);

  LOLED.setCursor(0, 6);
  ROLED.setCursor(0, 6);

  // Max 10 chars for a status string, '
  // SHOWSTATUS' is special, it displays 'sleep' or 'idle' as appropriate.
  if (printerstatus == 'O' )      {if (strcmp_P(ltext, PSTR("SHOWSTATUS")) == 0)
                                     LOLED.print(F(" Sleep    "));
                                   else 
                                     LOLED.print(ltext); 
                                   ROLED.print(rtext);}
  else if (printerstatus == 'I' ) {if (strcmp_P(ltext, PSTR("SHOWSTATUS")) == 0)
                                     LOLED.print(F(" Idle     "));
                                   else 
                                     LOLED.print(ltext); 
                                   ROLED.print(rtext);}
  else if (printerstatus == 'M' )  LOLED.print(F("Simulating"));
  else if (printerstatus == 'B' )  LOLED.print(F(" Busy     "));
  else if (printerstatus == 'P' )  LOLED.print(F(" Printing "));
  else if (printerstatus == 'T' )  LOLED.print(F(" Tool     "));
  else if (printerstatus == 'D' )  LOLED.print(F(" Pausing  "));
  else if (printerstatus == 'A' )  LOLED.print(F(" Paused   "));
  else if (printerstatus == 'R' )  LOLED.print(F(" Resuming "));
  else if (printerstatus == 'S' )  LOLED.print(F(" Stopped  "));
  else if (printerstatus == 'C' )  LOLED.print(F(" Config   "));
  else if (printerstatus == 'F' )  LOLED.print(F(" Updating "));
  else if (printerstatus == 'H' )  LOLED.print(F(" Halted  "));
  else if (printerstatus == '-' )  LOLED.print(F("Connecting")); // never set by the printer, used during init.
  else                             {LOLED.print(printerstatus);
                                   LOLED.print(F("         "));}// Oops; has someone added a new status?
    
  if ((printerstatus == 'P') || (printerstatus == 'A') || 
      (printerstatus == 'D') || (printerstatus == 'R'))
  { // Only display progress during printing states
    ROLED.print(F("  "));
    if ( done < 100 ) ROLED.print(F(" "));
    if ( done < 10 ) ROLED.print(F(" "));
    ROLED.print(done);
    ROLED.print(F("%"));
  }
  else if ((printerstatus != 'I') && (printerstatus != 'O')) 
  { 
    ROLED.print(F("          ")); 
  }

  handlebutton(); // catch the pause button

  if (heaterstatus[0] == 1) bedset = heaterstandby[0];
  else if (heaterstatus[0] == 2) bedset = heateractive[0];
  else if (heaterstatus[0] == 3) bedset = -1;
  else bedset = 0;

  if (bedset == -1 )
  { // fault
    LOLED.setCursor(10, 6);
    LOLED.print(F(" FAULT"));
  }
  else if (bedset == 0 )
  { // blank when off
    LOLED.setCursor(10, 6);
    LOLED.print(F("      "));
  }
  else
  { // Show target temp
    LOLED.setCursor(10, 6);
    LOLED.print(F("  "));
    if ( bedset < 100 ) LOLED.print(F(" "));
    if ( bedset < 10 ) LOLED.print(F(" "));
    LOLED.print(bedset);
    LOLED.print(char(176)); // degrees symbol
  }

  if (heaterstatus[toolhead+1] == 1) toolset = heaterstandby[toolhead+1];
  else if (heaterstatus[toolhead+1] == 2) toolset = heateractive[toolhead+1];
  else if (heaterstatus[toolhead+1] == 3) toolset = -1;
  else toolset = 0;

  if (toolset == -1 )
  { // fault
    ROLED.setCursor(10, 6);
    ROLED.print(F(" FAULT"));
  }
  else if (toolset == 0 )
  { // blank when off
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
  
  handlebutton(); // catch the pause button
  
  // bed and head text
  LOLED.setCursor(10, 0);
  LOLED.print(F("Bed"));

  ROLED.setCursor(11, 0);
  ROLED.print(F("E"));
  ROLED.print(toolhead);
  if (toolhead < 10) ROLED.print(F(" "));
  
  // Bed and Tool status icons
  if ( bedset == -1 )
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print(F("G")); // warning icon
  }
  else if ( bedset == 0 )
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print(F("N")); // standby icon
  }
  else
  {
    LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
    LOLED.setCursor(14, 0);
    LOLED.print(F("N")); // heater icon
  }

  if ( toolset == -1 )
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print(F("G")); // warning icon
  }
  else if ( toolset <= 0 )
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print(F("N")); // standby icon
  }
  else
  {
    ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
    ROLED.setCursor(14, 0);
    ROLED.print(F("T")); // down arrow to line (looks a bit like a hotend)
  }

  handlebutton(); // catch the pause button

  // The main temps (slowest to redraw)
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0, 0);
  if ( heaterinteger[0] < 100 ) LOLED.print(F(" "));
  if ( heaterinteger[0] < 10 ) LOLED.print(F(" "));
  LOLED.print(heaterinteger[0]);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9, 3);
  LOLED.print(F("."));
  LOLED.print(heaterdecimal[0]);
  LOLED.setFont(u8x8_font_8x13B_1x2_f);
  LOLED.print(char(176)); // degrees symbol

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0, 0);
  if ( heaterinteger[toolhead+1] < 100 ) ROLED.print(F(" "));
  if ( heaterinteger[toolhead+1] < 10 ) ROLED.print(F(" "));
  ROLED.print(heaterinteger[toolhead+1]);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9, 3);
  ROLED.print(F("."));
  ROLED.print(heaterdecimal[toolhead+1]);
  ROLED.setFont(u8x8_font_8x13B_1x2_f);
  ROLED.print(char(176)); // degrees symbol
}

/*    Button    */

// Note; this would be better handled by a pin interrupt, however this works and is easier.
// I simply repeatedly call this routine while updating the display or waiting for data
void handlebutton()
{
  // Process the button state and send pause/resume as appropriate
  
  if (pausecontrol == 0) return; // fast exit if pause disabled 
  
  if (digitalRead(BUTTON)) // button is active low.
  {
    // button not pressed, reset pause timer if needed and exit asap
    if (pausetimer != 0) 
    { 
      analogWrite(LED, 0); // led off
      #ifdef DEBUG
        Serial.println(F("Pausetimer reset"));
      #endif
      pausetimer = 0;
    }
    return; 
  }
  
  else if (pausetimer == -1) 
  {
    // command has been sent, ensure LED off and stop processing
    analogWrite(LED,0); // led off
    return; 
  }
  
  else if ((printerstatus == 'P') || (printerstatus == 'A'))
  {
    // Button is pressed while printing or paused
    if (pausetimer == 0) 
    { 
      pausetimer = millis(); // start timer as needed
      analogWrite(LED,255); // led on full
      #ifdef DEBUG 
        Serial.println(F("Pausetimer Started")); 
      #endif
    }

    // When timer expires take action
    if (millis() > (pausetimer + pausecontrol)) 
    {
      // Button held down for timeout; send commands as appropriate;
      if (printerstatus == 'P') sendwithcsum("M25");
      if (printerstatus == 'A') sendwithcsum("M24");
      pausetimer = -1; // -1 means we have sent the command
    }
  }
}


/*    JSON processing    */

// Good resource:
// https://alisdair.mcdiarmid.org/jsmn-example/

bool jsonparser()
{ // parse the Json data in 'char json[0-index]'; set values as appropriate or fail

  // Json parser instance (static)
  static jsmntok_t jtokens[MAXTOKENS];  // Tokens
  static jsmn_parser jparser; // Instance
  jsmn_init(&jparser); // Initialise json parser instance
  

  #ifdef DEBUG 
    Serial.print(F("freeMemory pre = "));
    Serial.println(freeMemory());
    Serial.print(F("Json : "));
    Serial.println(json);
    Serial.print(F("Size : "));
    Serial.println(index);
  #endif

  // blink LED while processing and pause button not pressed
  if (pausetimer == 0) analogWrite(LED, activityled);
  
  // Parse 
  int parsed = jsmn_parse(&jparser, json, index+1, jtokens, MAXTOKENS);

  #ifdef DEBUG
    Serial.print(F("Parser Return = "));
    Serial.println(parsed);
  #endif

  if (parsed < 1 || jtokens[0].type != JSMN_OBJECT) 
  {
    #ifdef DEBUG 
      Serial.println(F("Not a Json Object"));
    #endif
    if (pausetimer == 0) analogWrite(LED, 0);
    return(false);
  }

  if (parsed == 1)
  {
    #ifdef DEBUG 
      Serial.println(F("Empty Json Object"));
    #endif
    if (pausetimer == 0) analogWrite(LED, 0);
    return(false);
  }

  // Now process each token in turn using token parameters to 
  //  determine if we have a key, a value, or a nest of values.
  
  for (int i = 1; i < parsed; i++) 
  {
    // determine token payload size and copy to 'result[]'
    size_t result_len = jtokens[i].end-jtokens[i].start;
    char result[result_len+1];
    memcpy(result,json + jtokens[i].start,result_len);
    result[result_len] = '\0';

    // If it is type 3, and the token size is 1, we have a key.
    if ((jtokens[i].type == 3) && (jtokens[i].size == 1))
    {
      // The corresponding value (or an array object) is in the following token
      if (jtokens[i+1].size == 0) 
      {
        // Single value key

        // Load the corresponding value to a char array
        size_t value_len = jtokens[i+1].end-jtokens[i+1].start;
        char value[value_len+1];
        memcpy(value,json + jtokens[i+1].start,value_len);
        value[value_len] = 0;
  
        // Now process all the single-value keys we expect
        //
        
        if (strcmp_P(result, PSTR("status")) == 0)
        {
          char oldstatus = printerstatus;
          printerstatus = value[0];
          if( ((oldstatus == 'I') || (oldstatus == 'O')) && ((printerstatus != 'I') && (printerstatus != 'O')) )
          {
            // we are leaving Idle mode, clear idletext
            LOLED.clearLine(6);LOLED.clearLine(7);
            ROLED.clearLine(6);ROLED.clearLine(7);
          }
        }
        else if( strcmp_P(result, PSTR("tool")) == 0 )
        {
          toolhead = atoi(value);
          if ((toolhead < 0) || (toolhead >= HEATERS)) toolhead = 0;
        }
        else if( strcmp_P(result, PSTR("fraction_printed")) == 0 )
        {
          done = atof(value) * 100;
        }
        else if( strcmp_P(result, PSTR("pe_rate")) == 0 )
        {
          updateinterval = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_fails")) == 0 )
        {
          maxfail = atoi(value);
          if (maxfail == -1) screenclean(); // cleanup for when this is set while 'waiting for printer'
        }
        else if( strcmp_P(result, PSTR("pe_bright")) == 0 )
        {
          bright = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_saver")) == 0 )
        {
          if( strcmp_P(value, PSTR("true")) == 0 ) screensave = true; 
          else { screensave = false; if (!screenpower) screenwake(); } // force screen on.
        }
        else if( strcmp_P(result, PSTR("pe_pause")) == 0 )
        {
          pausecontrol = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_led")) == 0 )
        {
          activityled = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_lmsg")) == 0 )
        {
          byte s = strlen(value);
          for( byte a = 0; a < 10; a++ ) 
          {
            if( a < s ) ltext[a] = value[a]; else ltext[a] = ' '; // copy or pad as appropriate
          }
          ltext[10]='\0';
        }
        else if (strcmp_P(result, PSTR("pe_rmsg")) == 0)
        {
          byte s = strlen(value);
          for( byte a = 0; a < 10; a++ ) 
          {
            if( a < s ) rtext[a] = value[a]; else rtext[a] = ' '; // copy or pad as appropriate
          }
          rtext[10]='\0';
        }
      }
      else if( jtokens[i+1].size > 0 )
      {
        // Multiple value keys 
        // Assume values are max 5 chars since that is the most we need for a heater setting 
        //  '123.4', a percentage, or the word 'false'. ;-)

        // determine how many values we have, and create values[] array for them
        byte num_values = jtokens[i+1].size;
        if( num_values > HEATERS ) num_values = HEATERS; 
        char values[num_values][6]; // array of reported values, max 5 digits +NULL to a value

        // Step through each value in turn and populate the array
        for( int idx=0; idx < num_values; idx++ )
        {
          // Load the corresponding value into an array
          size_t value_len = jtokens[i+2+idx].end-jtokens[i+2+idx].start;
          if (value_len > 5) value_len = 5;
          memcpy(values[idx],json + jtokens[i+2+idx].start,value_len);
          values[idx][value_len] = '\0';
        } 

        // Now handle the Json keys that correspond to value lists.
        if (strcmp_P(result, PSTR("heaters")) == 0)
        {
          for( int idx=0; idx < num_values; idx++) 
          {
            if(( values[idx] >= -99 ) && ( values[idx] <= 999 )) // reject insane values
            {
              heaterinteger[idx] = atoi(values[idx]);
              heaterdecimal[idx] = (atof(values[idx])-heaterinteger[idx])*10;
            }
          }
        }
        else if (strcmp_P(result, PSTR("active")) == 0)
        {
          for( int idx=0; idx < num_values; idx++) 
          {
            if(( values[idx] >= -99 ) && ( values[idx] <= 999 )) heateractive[idx] = atoi(values[idx]);
          }
        }
        else if (strcmp_P(result, PSTR("standby")) == 0)
        {
          for( int idx=0; idx < num_values; idx++) 
          {
            if(( values[idx] >= -99 ) && ( values[idx] <= 999 )) heaterstandby[idx] = atoi(values[idx]);
          }
        }
        else if (strcmp_P(result, PSTR("hstat")) == 0)
        {
          for( int idx=0; idx < num_values; idx++) 
          {
            heaterstatus[idx] = atoi(values[idx]);
          }
        }
      }
    }
  }
  
  #ifdef DEBUG
    Serial.print(F("freeMemory post = "));
    Serial.println(freeMemory());
  #endif

  // If we get here we have recieved a valid Json response from the printer
  // Clear the screens if 'Waiting for Printer' message is currently being displayed
  if ((noreply >=  maxfail) && (maxfail != -1)) screenclean();

  // kill the activity LED unless pause button pressed
  if (pausetimer == 0) analogWrite(LED, 0); 

  return(true);  // we have processed a valid block, does not assert whether data has been updated
}

// Send to Printer with a checksum
// lifted from: https://duet3d.dozuki.com/Wiki/Gcode#Section_Checking

void sendwithcsum(char cmd[8])
{
  int cs = 0;
  for(int i = 0; cmd[i] != '*' && cmd[i] != NULL; i++)
   cs = cs ^ cmd[i];
  cs &= 0xff;  // Defensive programming...
  Serial.print(cmd);
  Serial.print("*");
  Serial.println(cs);
}


/*    Loop    */

void loop(void)
{
  // The core of this is a nested loop that periodically sends M408 S0 requests 
  // and waits for an answer for a predefined period before repeating the request.
  //
  // All responses are processed asap, if a potential Json start is detected '{' 
  // then a fast loop reads the following data from the serial buffer before it overflows.
  // This finishes when it either detects the terminator '}' or times out.
  // Any apparently viable json packets captured are then passed to the Json parser function.
  //
  // Other incoming characters that are not part of any json object are dropped
  //
  // M408 S0 is the most basic info request, but has all the data we use
  
  unsigned long timeout;
  bool jsonstart;
  
  jsonstart = false;
  do 
  {
    // Send the Magic command to ask for Json data (with checksum).
    sendwithcsum("M408 S0");
    noreply++; // Always assume the request will fail, jsonpaser() resets the count on success
    if (maxfail != -1) {
      // once max number of failed requests is reached, show 'waiting for printer'
      if ( noreply == maxfail ) commwait();
    }
    
    timeout = millis() + updateinterval; // start the clock
    
    while ( millis() < timeout && !jsonstart )
    {
      // check button and look for a '{' on the serial port for a time defined by 'updateinterval'
      handlebutton();
      delay(1);
      if (Serial.read() == '{')
      {
        // flag to exit the nested while loops and process input
        jsonstart = true;
      }
    }
  }
  while (!jsonstart);

  // Now read input until the terminating '}' is encountered (eg success)
  // or fail if we either timeout or hit the maximum length
  // do this with no delay in the loop since we need to prevent the
  // (64bit) serial buffer overflowing @57600 baud.
  
  index = 0;
  int nest = 0; //measure nesting of json braces, nest=1 at top level
  char incoming = '{'; // we know the string started with this
  unsigned long jtimeout = millis() + JSONWINDOW; // timeout while recieving the json data

  while ((incoming != '}') || (nest > 1)) // the closing } will exit the loop
  {
    if (incoming != -1) // -1 means serialRead() did not return anything
    {
      json[index] = incoming;
      index++;
    }
    
    if (incoming == '{') nest++;
    if (incoming == '}') nest--;
    
    if (index > jsonSize) 
    {
      index = 0; 
      #ifdef DEBUG
        Serial.println(F("Object too large"));
      #endif
      break;
    }
    if (millis() > jtimeout)
    {
      index = 0;
      #ifdef DEBUG 
        Serial.println(F("Timeout reading Object"));
      #endif
      break;
    }
    handlebutton();
    incoming = Serial.read();
  }
  
  if (index == 0)
  {
    #ifdef DEBUG
      Serial.println(F("No valid Json to parse"));
    #endif
    return;  // no new input, skip parsing or updating display, go back to requesting
  }
  else
  {
    json[index] = '}'; // terminate Json string properly
    json[index+1] = '\0';
  }

  // We have something between braces {}, probably Json.
  // parse it and reset the fail counter on success, loop again on failure
  if (jsonparser()) noreply = 0; else return;
  
  // handle screensave mode if enabled
  if ( screensave )
  { 
    // Sleep the screen when firmware now reports PSU off but screen is still on
    if (( printerstatus == 'O') && screenpower) screensleep();
 
    // Wake screen when printer regains power and the screen is currently off
    if (( printerstatus != 'O') && !screenpower) screenwake();
  }

  // Update Screen!
  // First update brightness level as needed (returns true if screen is on, false if blank) 
  // Test if we have had a response during this requestcycle
  // Determine whether we are in standby mode
  // Only update the display if needed 
  if (setbrightness() && screenpower && (noreply == 0)) updatedisplay();

  // Snooze until timeout is reached (we usually complete a cycle within the timeout and need to pause here)
  if (millis() < timeout)
  {
    handlebutton(); // catch the pause button
    delay(5); // wait a bit
  }

}
