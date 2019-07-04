## Little Eyes For a printer..
# Temperature display for RepRap firmware (eg Duet)

![Prototype](./images/printeye-prototype.jpg)

* Only displays very basic info: Status, tool and bed activity + temperature, pct printed (when printing)
 * This is it.. the displays are less then an 2cm in size and I will not overload them with info
* Sends M408 requests for basic data and then proceses the Json reply
 * Uses the auxillary UART port on 32bit controllers (eg Duet)
* Also responds to some 'config' Json
 * timeout, update speed, brightness, and how to respect PSU control
* Sleep mode when controller reports status 'O' (PSU off, configurable)

## Requirements 
* ArduinoJSON (via IDE library manager or directly from <URL!!>

## Control
The ArduinoJSON library is used, which provides some robustness in processing key/value pairs (use of quotes etc; the Json must still be structually correct and terminated)
* {"printeye_interval":<integer>"}
 * Set the approximate interval in Ms that PrintEye spends waiting for a M408 response before retrying
 * '-1' to prevent entering 'waiting for Printer' state
* {"printeye_maxfail":}
* {"":}
* {"":}

## Caveats:
* S/W I2C is slow. Alternative (one HW + One SW) looks weird and unbalanced.
 * A I2C multiplexer would solve this, or using a chim (Mega256?) with dual hardware I2C
 * I have tried to compensate for the slow redrawing by sequencing the order of updating screen elements; eg making the updates look more like animations.
* The Json parser uses quite a bit of ram and cannot survive a sudden increase in size of M408 S0 responses (eg from a firmware update), currently they max out at 400 characters or so, and I allow a maximum of 450 bytes for Data +overhead.
* You will need level shifters for interfacing to a Duet UART (PanelDue) port if you run this at 16Mhz/5v, alternatively use a 12Mhz/3.3v combo, or experiment with the underclock option discussed in the setup() section of the sketch. Display updates will be even slower for this, and you might need to add a 3v3 regulator, or tap the controllers 3v3 line for power.

## Enhancements: 
* Set 'idletext' (32 chars, spans both displays) to be displayed in idel mode via Json (memory??, 33 bytes to store!)
* can we multiplex the HW I2C bus (SCK) with IO pins and a couple of signal diodes to address one display or the other, or both for setup, clearing etc.
* WiFi status/strength? not in the Json message, but handy. Network and IP address in idle mode?
* Find a lower memory footprint Json parser
