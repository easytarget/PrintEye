/*
  Printer Eyes; a simple twin-panel display for reprap firmware with a spare serial port (eg. Duet)

  Nice display possibilities courtesy of:
  Universal 8bit Graphics Library (https://github.com/olikraus/u8g2/)
  Grocking the Json response with <1k of ram left after the screens have gobbled a load 
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
// - Allow 160 bytes or more free for allocations during processing and screen updates (found by experiment)
// Heater settings are good for a 4 extruder system + bed, everything else is ignored.
// - Each additional heater adds 8 bytes in global arrays, 4 additional tokens, 
//   and ~20 characters of extra Json
//
#define JSONSIZE 508    // Json incoming buffer size
#define MAXTOKENS 87    // Maximum number of jsmn tokens we can handle (see jsmn docs, 8 bytes/token)
#define HEATERS 5       // Bed + Up to 4 extruders
#define JSONWINDOW 500; // How many ms allowed for the rest of the object to arrive after '{' is recieved

// U8x8 Contructors for my displays and wiring
// The complete list is available here: https://github.com/olikraus/u8g2/wiki/u8x8setupcpp
 // Left OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C LOLED(/* clock=*/ SCK1, /* data=*/ SDA1, /* reset=*/ U8X8_PIN_NONE);
 // Right OLED
U8X8_SSD1306_128X64_NONAME_SW_I2C ROLED(/* clock=*/ SCK2, /* data=*/ SDA2, /* reset=*/ U8X8_PIN_NONE);

//  Hardware I2C works, but the ATMega 328P only has one HW I2C interface available, 
//  and if your displays have address conflicts (like my cheap ones do) a trick is to 
//  use two Software interfaces.
//  Software (bit-bang)is much slower, but otherwise just as good.

// During debug I enable this to monitor the free memory during runtime
#ifdef DEBUG 
  #include "MemoryFree.h" // Taken from https://playground.arduino.cc/Code/AvailableMemory/
#endif

// Incoming data

const int jsonSize = JSONSIZE;  // Maximum size of a M408 response we can process.
static char json[jsonSize + 1]; // Json response
static int index;               // length of the response


// Primary Settings (can also be set via Json messages to serial port, see README)
unsigned int updateinterval = 1000; // how many ~1ms loops we spend looking for a response after M408
byte maxfail = 6;                   // max failed requests before entering comms fail mode (-1 to disable)
bool screensave = true;             // Go into screensave when controller reports PSU off (status 'O')
byte bright = 128;                  // Screen brightness (0-255, sets non-linear OLED 'contrast',0 is off)
unsigned int buttoncontrol = 333;   // Hold-down delay for button, 0=disabled, max=(updateinterval-100)
byte buttonconfig = 22;             // Action config for button (see README).
byte activityled = 80;              // Activity LED brightness (0 to disable)
char ltext[11] = "SHOWSTATUS";      // Status line for the off/idle/busy display (left 10 chars)
char rtext[11] = "          ";      // Status line for the off/idle display (right 10 chars)

// Json derived data:
char printerstatus = '-'; // from m408 status key, initial value '-' is shown as 'connecting'
byte toolhead = 0;        // Tool to be monitored (assume E0 by default)
byte done = 0;            // Percentage printed

// Heater active, standby and status values for all possible heaters ([0] = bed, [1] = E0, [2] = E1, etc)
int heateractive[HEATERS];
int heaterstandby[HEATERS];
byte heaterstatus[HEATERS];
// Main temp display is derived from Json values, split into the integer value and it's decimal
int heaterinteger[HEATERS];
byte heaterdecimal[HEATERS];

// PrintEye internal 
byte noreply = 1;             // count failed requests (default: assume we have already missed one)
byte currentbright = 0;       // track changes to brightness
bool screenpower = true;      // OLED power status
unsigned long pausetimer = 0; // A time store for the pause button
bool octopaused = false;      // Track whether we have sent Octoprint a pausecommand
bool interstatial = false;    // Are we displaying a splashscreen?


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

  // Get the OLED's initialised asap, begin() will blank them
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
  for (byte a = 0; a < HEATERS; a++)
  {
    heateractive[a] = 0;
    heaterstandby[a] = 0;
    heaterstatus[a] = 0;
    heaterinteger[a] = 0;
    heaterdecimal[a] = 0;
  }
  analogWrite(LED, 0);    // turn the led off
}


/*    Screen Handlers    */

void screenclean()
{ // clear both oleds, used in animations
  goblank();
  unblank();
}

void goblank()
{ // darken screen then erase contents, used in animations
  LOLED.setContrast(0);
  ROLED.setContrast(0);
  LOLED.clear();
  ROLED.clear();
}

void unblank()
{ // restore the screen brightness, used in animations
  LOLED.setContrast(bright);
  ROLED.setContrast(bright);
}

// Flash power off icon, blank the screen and enter powersave
void screensleep()
{
  goblank();
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.drawGlyph(6,1,'N'); // Power off icon in this font set
  ROLED.drawGlyph(6,1,'N');
  unblank();
  delay(333); // flash the power off icons
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
  LOLED.drawGlyph(6,1,'O'); // Resume icon in this font set
  ROLED.drawGlyph(6,1,'O');
  unblank();
  delay(333); // make sure icons are seen
  screenclean();
}

// Display the 'Waiting for Comms' splash
void commwait()
{
  goblank();
  LOLED.setPowerSave(false); // in case screen is off (powersave)
  ROLED.setPowerSave(false);
  LOLED.setFont(u8x8_font_8x13_1x2_f);
  ROLED.setFont(u8x8_font_8x13_1x2_f);
  LOLED.setCursor(3, 6);
  ROLED.setCursor(5, 6);
  LOLED.print(F("Waiting for"));
  ROLED.print(F("Printer"));
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.drawGlyph(6,1,'F'); // Signal icon in this font set
  ROLED.drawGlyph(6,1,'F');
  interstatial=true;
  unblank();
}

// Set the (non-linear) contrast/brightness level for supported displays
bool setbrightness()
{ 
  // Only update brightness when level has been changed because
  // setContrast() can cause a screen flicker when called.
  if (bright != currentbright) 
  {
    LOLED.setContrast(bright); // Set the new contrast value
    ROLED.setContrast(bright);
    currentbright = bright;    // remember what we just did
  }
  // return on/off status
  if ((bright > 0) && screenpower) return (true); else return (false);
}

// Print current progress (padded) on right display and blank rest of line
void printprogress()
{
  ROLED.print(F("  "));
  if (done < 100) ROLED.print(' ');
  if (done < 10) ROLED.print(' ');
  ROLED.print(done);
  ROLED.print(F("%    "));
}

// Print 10 space chars on r/h display, saves a bunch of progmem since I do this a lot.
void blankright()
{
  ROLED.print(F("          "));
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

  // If we currently have an interstatial screen clear it.
  if (interstatial)
  {
    screenclean();
    interstatial = false;
  }

  // First update lower status line
  
  LOLED.setFont(u8x8_font_8x13_1x2_f);
  ROLED.setFont(u8x8_font_8x13_1x2_f);

  LOLED.setCursor(0, 6);
  ROLED.setCursor(0, 6);

  
  if (printerstatus == 'O' )
  {
    if (strcmp_P(ltext, PSTR("SHOWSTATUS")) == 0) // 'SHOWSTATUS' displays actual state
    {
      LOLED.print(F(" Sleep    "));
      blankright();
    }
    else 
    {
      LOLED.print(ltext); 
      ROLED.print(rtext);
    }
  }
  else if (printerstatus == 'I' )
  {
    if (strcmp_P(ltext, PSTR("SHOWSTATUS")) == 0) // 'SHOWSTATUS' displays actual state
    {
      LOLED.print(F(" Idle     "));
      blankright();
    }
    else
    {
      LOLED.print(ltext); 
      ROLED.print(rtext);
    }
  }
  else if (printerstatus == 'B' )
  {
    LOLED.print(F(" Busy     ")); // In busy mode put..
    if (strcmp_P(ltext, PSTR("SHOWSTATUS")) == 0) blankright();// if not default..
    else ROLED.print(ltext);      // left text on right display.
  }
  else if (printerstatus == 'M' )
  {
    LOLED.print(F("Simulating"));
    printprogress();
  }
  else if (printerstatus == 'P' )
  {
    LOLED.print(F(" Printing "));
    printprogress();
  }
  else if (printerstatus == 'T' )
  {
    LOLED.print(F(" Tool     "));
    blankright();
  }
  else if (printerstatus == 'D' )
  {
    LOLED.print(F(" Pausing  "));
    printprogress();
  }
  else if (printerstatus == 'A' )
  {
    LOLED.print(F(" Paused   "));
    printprogress();
  }
  else if (printerstatus == 'R' )
  {
    LOLED.print(F(" Resuming "));
    printprogress();
  }
  else if (printerstatus == 'S' )
  {
    LOLED.print(F(" Stopped  "));
    blankright();
  }
  else if (printerstatus == 'C' )
  {
    LOLED.print(F(" Config   "));
    blankright();
  }
  else if (printerstatus == 'F' )
  {
    LOLED.print(F(" Updating "));
    blankright();
  }
  else if (printerstatus == 'H' )
  {
    LOLED.print(F(" Halted  "));
    blankright();
  }
  else if (printerstatus == '-' )
  {
    // Dummy initial status, might be displayed if maxfail reset to zero before any M408 response.
    LOLED.print(F("  Connecting to")); 
    ROLED.print(F(" Printer")); 
    return; // No data has been recieved yet, so dont update anything else
  }
  else
  {
    LOLED.print(' '); // pad
    LOLED.print(printerstatus);  // Oops; has someone added a new status?
    LOLED.print(F("        "));  // show it and blank rest
    blankright();
  }
  
  handlebutton(); // catch the pause button

  // Set temp according to the status of the heater
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
    if ( bedset < 100 ) LOLED.print(' ');
    if ( bedset < 10 ) LOLED.print(' ');
    LOLED.print(bedset);
    LOLED.print(char(176)); // degrees symbol
  }

  // Set temp according to the status of the heater
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
    if ( toolset < 100 ) ROLED.print(' ');
    if ( toolset < 10 ) ROLED.print(' ');
    ROLED.print(toolset);
    ROLED.print(char(176)); // degrees symbol
  }
  
  handlebutton(); // catch the pause button
  
  // bed and head text
  LOLED.setCursor(10, 0);
  LOLED.print(F("Bed"));

  ROLED.setCursor(11, 0);
  ROLED.print('E');
  ROLED.print(toolhead);
  if (toolhead < 10) ROLED.print(' ');
  
  // Bed and Tool status icons
  if ( bedset == -1 ) // fault
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.drawGlyph(14,0,'G'); // Warning icon in this font set
  }
  else if ( bedset == 0 ) // off
  {
    LOLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    LOLED.drawGlyph(14,0,'N'); // Standby icon in this font set
  }
  else
  { 
    LOLED.setFont(u8x8_font_open_iconic_thing_2x2);
    LOLED.drawGlyph(14,0,'N'); // Heater icon in this font set
  }

  if ( toolset == -1 ) // fault
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.drawGlyph(14,0,'G'); // Warning icon in this font set
  }
  else if ( toolset <= 0 ) // off
  {
    ROLED.setFont(u8x8_font_open_iconic_embedded_2x2);
    ROLED.drawGlyph(14,0,'N'); // Standby icon in this font set
  }
  else
  {
    ROLED.setFont(u8x8_font_open_iconic_arrow_2x2);
    ROLED.drawGlyph(14,0,'T'); // Down arrow to line (looks a bit like a hotend)
  }

  handlebutton(); // catch the pause button

  // The main temps (slowest to redraw)
  LOLED.setFont(u8x8_font_inr33_3x6_n);
  LOLED.setCursor(0, 0);
  if ( heaterinteger[0] < 100 ) LOLED.print(' ');
  if ( heaterinteger[0] < 10 ) LOLED.print(' ');
  LOLED.print(heaterinteger[0]);

  LOLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  LOLED.setCursor(9, 3);
  LOLED.print('.');
  LOLED.print(heaterdecimal[0]);
  LOLED.setFont(u8x8_font_8x13_1x2_f);
  LOLED.print(char(176)); // degrees symbol

  ROLED.setFont(u8x8_font_inr33_3x6_n);
  ROLED.setCursor(0, 0);
  if ( heaterinteger[toolhead+1] < 100 ) ROLED.print(' ');
  if ( heaterinteger[toolhead+1] < 10 ) ROLED.print(' ');
  ROLED.print(heaterinteger[toolhead+1]);

  ROLED.setFont(u8x8_font_px437wyse700b_2x2_n);
  ROLED.setCursor(9, 3);
  ROLED.print('.');
  ROLED.print(heaterdecimal[toolhead+1]);
  ROLED.setFont(u8x8_font_8x13_1x2_f);
  ROLED.print(char(176)); // degrees symbol
}

/* Send to Printer with a checksum */

void sendwithcsum(char cmd[])   // cmd[] is assumed to be in PROGMEM...
{
  int cs = 0;
  for(int i = 0; pgm_read_byte_near(cmd+i) != '*' && pgm_read_byte_near(cmd+i) != NULL; i++) {
   Serial.write(pgm_read_byte_near(cmd+i));
   cs = cs ^ pgm_read_byte_near(cmd+i);
  }
  cs &= 0xff;  // Only the lower bits
  Serial.print("*");
  Serial.println(cs);
}

/*    Button    */

// Note; Button detection would be better handled by a pin interrupt, but regular polling 
// as implemented here also works, and is easier. 
// This routine is repeatedly called while updating the display or waiting for data.
void handlebutton()
{
  // Process the button state and send pause/resume as appropriate
  
  if (buttoncontrol == 0) return; // fast exit if disabled 
  
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
  else // Look to see if we need to start timer
  if (pausetimer == 0) 
  { 
    pausetimer = millis(); // start timer as needed
    #ifdef DEBUG 
      Serial.println(F("Pausetimer Started")); 
    #endif
  }

  // Button is pressed and active, turn LED on.
  analogWrite(LED,255); // led on full

  // When timer expires take action
  if (millis() > (pausetimer + buttoncontrol)) 
  {
    switch(buttonconfig) {
    // Send commands as appropriate depending on current status and action config
    case 1:
      rrfpauseresume();
      break;
    case 2:
      rrfpauseresume();
      octopauseresume();
      break;
    case 11:
      rrfpauseresume();
      if (strchr_P(PSTR("IO"),printerstatus)) rrfemergencystop();
      break;
    case 22:
      rrfpauseresume();
      if (octopauseresume()) break;
      if (strchr_P(PSTR("IO"),printerstatus)) rrfemergencystop();
      break;
    case 33: 
      if (rrfpauseresume()) break;
      rrfemergencystop();
      break;
    case 44: 
      if (rrfpauseresume()) break;
      if (octopauseresume()) break;
      rrfemergencystop();
      break;
    case 99: 
      rrfemergencystop(); // always do a stop
      break;
    default:
      break; // do nothing..
    }
    pausetimer = -1;    // -1 = command sent(halts the cycle till the button is released)
  }
}

// Functions to assist the button actions

bool rrfpauseresume()
{  // send a RRF/Duet pause or resume as appropriate
  if (printerstatus == 'A') 
  {
    sendwithcsum(PSTR("M24"));
    return(true);
  }
  else if (printerstatus == 'P')
  {
    sendwithcsum(PSTR("M25"));
    return(true);
  }
  return(false);
}

void rrfemergencystop()
{  // send M112 to RRF/Duet to trigger an emergency stop
  sendwithcsum(PSTR("M112"));
  for (int a=0; a<6; a++) { // briefly, but furiously, flash led
    analogWrite(LED,255); 
    delay(25);
    analogWrite(LED,0);
    delay(25);
  }  
  goblank();
  LOLED.setPowerSave(false); // in case screen is currently in powersave
  ROLED.setPowerSave(false);
  LOLED.setFont(u8x8_font_8x13_1x2_f);
  ROLED.setFont(u8x8_font_8x13_1x2_f);
  LOLED.setCursor(0, 6);
  ROLED.setCursor(0, 6);
  LOLED.print(F("    EMERGENCY   ")); 
  ROLED.print(F("      STOP      "));
  LOLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  ROLED.setFont(u8x8_font_open_iconic_embedded_4x4);
  LOLED.drawGlyph(6,1,'G'); // Warning icon in this font set
  ROLED.drawGlyph(6,1,'G');
  unblank();
  interstatial=true;
  delay(1500); // Give the printer time to die...
  noreply = 0; // reset reply counter and start looking for a response
}

bool octopauseresume()
{ // cycle between a pause/resume octoprint action command. 
  // Pause when busy and unpaused; resume if paused and idle.
  if (printerstatus == 'B' && !octopaused)
  { 
    // Send octoprint pause command when busy 
    sendwithcsum(PSTR("M118 P1 S\"//action:pause\""));
    octopaused = true;
    return(true);
  }
  if (printerstatus == 'I' && octopaused)
  { // Send octoprint resume command from idle state when paused
    sendwithcsum(PSTR("M118 P1 S\"//action:resume\"")); 
    octopaused = false;
    return(true);
  }
  return(false);
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
          printerstatus = value[0];
        }
        else if( strcmp_P(result, PSTR("tool")) == 0 )
        {
          toolhead = atoi(value);
          if ((toolhead < 0) || (toolhead+1 >= HEATERS)) toolhead = 0;
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
          byte f = atoi(value); // cleanup any existing 'waiting for printer' display when this changes
          if (f != maxfail) screenclean(); 
          maxfail = f;
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
        else if( strcmp_P(result, PSTR("pe_bdelay")) == 0 )
        {
          buttoncontrol = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_bcfg")) == 0 )
        {
          buttonconfig = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_led")) == 0 )
        {
          activityled = atoi(value);
        }
        else if( strcmp_P(result, PSTR("pe_imsg")) == 0 )
        {
          byte s = strlen(value);
          for( byte a = 0; a < 10; a++ ) 
          {
            if( a < s ) ltext[a] = value[a]; else ltext[a] = ' '; // copy/pad as appropriate
            if( a+10 < s ) rtext[a] = value[a+10]; else rtext[a] = ' '; // copy/pad as appropriate
          }
          ltext[10]='\0';
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
            if(( atoi(values[idx]) >= -99 ) && ( atoi(values[idx]) <= 999 )) // reject insane values
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
            if(( atoi(values[idx]) >= -99 ) && ( atoi(values[idx]) <= 999 )) 
              heateractive[idx] = atoi(values[idx]);
          }
        }
        else if (strcmp_P(result, PSTR("standby")) == 0)
        {
          for( int idx=0; idx < num_values; idx++) 
          {
            if(( atoi(values[idx]) >= -99 ) && ( atoi(values[idx]) <= 999 )) 
              heaterstandby[idx] = atoi(values[idx]);
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
  if ((noreply >=  maxfail) && (maxfail != 0)) screenclean();

  // kill the activity LED unless pause button pressed
  if (pausetimer == 0) analogWrite(LED, 0); 

  return(true);  // we have processed a valid block of data
}


/*    Loop    */
static unsigned long timeout = 0;
static bool jsonstart;

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
  // - It is sent with a checksum; https://duet3d.dozuki.com/Wiki/Gcode#Section_Checking

  jsonstart = false;
  do 
  {
    if ( millis() > timeout ) {
      // Send the Magic command to ask for Json data (with checksum).
      sendwithcsum(PSTR("M408 S0"));
      timeout = millis() + updateinterval; // and start the clock
    }
  
    // once max number of failed requests is reached, show 'waiting for printer'
    if ((noreply ==  maxfail) && (maxfail != 0)) commwait();

    while ( millis() <= timeout && !jsonstart )
    { // look for a '{' on the serial port for a time defined by 'updateinterval'
      // first, check button 
      handlebutton();
      // delay for a ms
      delay(1);
      // set flag if '{' on the serial port
      if (Serial.read() == '{')
      {
        // flag to exit the nested while loops and process input
        jsonstart = true;
      }
    }
    noreply++; // Always assume the request failed, jsonpaser() resets the count on success
  }
  while (!jsonstart);

  // Now read input until the terminating '}' is encountered (eg success)
  // or fail if we either timeout or hit the maximum length
  // do this with no delay in the loop since we need to prevent the
  // (64bit) serial buffer overflowing @57600 baud.
  
  index = 0;
  byte nest = 0; //measure nesting of json braces, nest=1 at top level
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
  // Parse it and reset the fail counter on success, loop again on failure
  if (jsonparser()) noreply = 0; else return;
  
  // handle screensave mode if enabled
  if ( screensave )
  { 
    // Sleep the screen when firmware now reports PSU off but screen is still on
    if (( printerstatus == 'O') && screenpower) screensleep();
 
    // Wake screen when printer regains power and the screen is currently off
    if (( printerstatus != 'O') && !screenpower) screenwake();
  }

  /*    Update Screen!  */
  
  // First check & update brightness level as needed (returns true if screen is on, false if blank) 
  // also Test if we have had a response during this requestcycle
  // Only update the display if both true
  if (setbrightness() && (noreply == 0)) updatedisplay();

  // If we are in powersave mode and have an interstatial (waiting for printer, emergency stop), 
  // restore the powersave state
  if (interstatial && !screenpower)
  {
    goblank();
    LOLED.setPowerSave(true);
    ROLED.setPowerSave(true);
    interstatial = false;
  }

  
  // Now loop back to waiting for Json and sending requests 
}
