#include <SPI.h>
#include <RFID.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <RTClib.h>    // Add RTC library

// Define Pins - Updated to match wiring
#define SS_PIN 10      // RFID SDA
#define RST_PIN 3      // RFID RST updated from 9 to match wiring
#define LOCK_PIN 4     // Relay control to 4
#define RED_LED 6      // Red LED to 6
#define GREEN_LED 5    // Green LED on pin 5
#define MOSFET_PIN 2   // Added for MOSFET control

// RFID Module Setup
RFID rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;       // Create RTC object

// Days of the week array for readability
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// Authorized Users Data Structure with time restrictions
struct AuthorizedUser {
  String rfid;
  String name;
  int startHour;    // Start hour in 24-hour format
  int startMinute;  // Start minute
  int endHour;      // End hour in 24-hour format
  int endMinute;    // End minute
  bool weekdays[7]; // Access allowed on specific days (Sun=0, Mon=1, ..., Sat=6)
};

// Added global variables for extended access functionality
unsigned long doorUnlockedUntil = 0;  // Timestamp when door should be locked again
String currentOccupant = "";          // Stores the name of the current occupant
bool doorMaintainedOpen = false;      // Flag to track if door is being held open

const int MAX_USERS = 20;
AuthorizedUser users[MAX_USERS] = {
  // Example: Prof. Hans can access from 8:00 to 17:00 on weekdays (Mon-Fri)
  {"538A1C2F", "Prof. Hans", 22, 45, 22, 50, {true, true, true, true, true, true, true}}
  // Add more users as needed with their time restrictions
};

void setup() {
  Serial.begin(9600);
  SPI.begin();
  Wire.begin();      // Initialize I2C communication for RTC
  
  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    lcd.clear();
    lcd.print("RTC Error!");
    while (1);
  }
  
  rfid.init();
  lcd.init();
  lcd.backlight();
  
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(MOSFET_PIN, OUTPUT);  // Added MOSFET pin
  
  digitalWrite(LOCK_PIN, HIGH);    // Relay initially inactive, lock is closed
  digitalWrite(RED_LED, LOW);      // Red LED off
  digitalWrite(GREEN_LED, LOW);    // Green LED off
  digitalWrite(MOSFET_PIN, LOW);   // MOSFET initially off
  
  lcd.print(" Access Control");
  lcd.setCursor(0, 1);
  lcd.print("  System Ready");
  delay(2000);
  
  // Setup for Excel logging via PLX-DAQ
  Serial.println("CLEARDATA");
  Serial.println("LABEL, Access, Time, Date, Keycard UID, Name, Reason");
  Serial.println("RESETTIMER");
  
}

void loop() {
  if (rfid.isCard()) {
    rfid.readCardSerial();
    String cardUID = getCardUID();
    checkAccess(cardUID);
    rfid.halt();
  }
  
  // Continuously check if door state needs to be updated
  maintainDoorState();
}


// Convert RFID serial number to string
String getCardUID() {
  String uid = "";
  for (int i = 0; i < 4; i++) {
    uid.concat(String(rfid.serNum[i] < 0x10 ? "0" : ""));
    uid.concat(String(rfid.serNum[i], HEX));
  }
  uid.toUpperCase();
  return uid;
}

// Check if current time is within allowed access hours for a user
bool isWithinAllowedTime(AuthorizedUser user) {
  DateTime now = rtc.now();
  int currentDay = now.dayOfTheWeek(); // 0 = Sunday, 1 = Monday, etc.
  int currentHour = now.hour();
  int currentMinute = now.minute();
  
  // First check if today is an allowed day
  if (!user.weekdays[currentDay]) {
    return false;
  }
  
  // Convert current time to minutes for easier comparison
  int currentTimeInMinutes = currentHour * 60 + currentMinute;
  int startTimeInMinutes = user.startHour * 60 + user.startMinute;
  int endTimeInMinutes = user.endHour * 60 + user.endMinute;
  
  // Check if current time is within allowed hours
  return (currentTimeInMinutes >= startTimeInMinutes && currentTimeInMinutes <= endTimeInMinutes);
}

// Add function to check if door should remain open based on schedule
void maintainDoorState() {
  // If door isn't currently being maintained open, no need to check
  if (!doorMaintainedOpen) {
    return;
  }
  
  // Check if we should lock the door
  if (millis() > doorUnlockedUntil) {
    // Time is up, lock the door
    digitalWrite(LOCK_PIN, HIGH);    // Deactivate relay to lock the solenoid
    digitalWrite(MOSFET_PIN, LOW);   // Turn off MOSFET
    digitalWrite(GREEN_LED, LOW);    // Turn off Green LED
    
    lcd.clear();
    lcd.print("  Door Locked");
    lcd.setCursor(0, 1);
    lcd.print("Schedule ended");
    
    doorMaintainedOpen = false;
    currentOccupant = "";
    
    delay(2000);
    lcd.clear();
    lcd.print(" Access Control");
    lcd.setCursor(0, 1);
    lcd.print("  System Ready");
  }
}

// Modified checkAccess function to handle extended access
void checkAccess(String cardUID) {
  bool authorized = false;
  String userName = "";
  String reason = ""; // Reason for denying access
  
  for (int i = 0; i < MAX_USERS; i++) {
    // If we've reached an empty entry, no need to continue
    if (users[i].rfid == "") {
      break;
    }
    
    // Check if the card matches
    if (users[i].rfid == cardUID) {
      // Check if user wants to enable extended access
      if (isWithinAllowedTime(users[i])) {
        // Calculate how long to keep the door open
        DateTime now = rtc.now();
        int currentDay = now.dayOfTheWeek();
        int endTimeInMinutes = users[i].endHour * 60 + users[i].endMinute;
        
        // Calculate milliseconds until end of allowed time
        unsigned long currentMinuteOfDay = now.hour() * 60 + now.minute();
        unsigned long minutesRemaining = endTimeInMinutes - currentMinuteOfDay;
        unsigned long millisecondsRemaining = minutesRemaining * 60 * 1000;
        
        // Set the door to remain unlocked until the end of scheduled time
        doorUnlockedUntil = millis() + millisecondsRemaining;
        doorMaintainedOpen = true;
        currentOccupant = users[i].name;
        
        extendedAccessGrant(users[i].name);
        authorized = true;
        userName = users[i].name;
      } else {
        // Valid card but outside allowed hours
        denyAccess("Outside schedule");
        authorized = false;
        userName = users[i].name;
        reason = "Outside scheduled hours";
      }
      break;
    }
  }
  
  // If not found in the list of authorized users
  if (userName == "") {
    // Check if room is occupied first
    if (doorMaintainedOpen && currentOccupant != "") {
      // Modified to display the specific occupancy message
      reason = "Room occupied by " + currentOccupant;
      lcd.clear();
      lcd.print("Room is Occupied");
      lcd.setCursor(0, 1);
      lcd.print("Access Denied");
      
      digitalWrite(RED_LED, HIGH);     // Turn on Red LED
      digitalWrite(LOCK_PIN, HIGH);    // Ensure door is locked
      digitalWrite(MOSFET_PIN, LOW);   // Ensure MOSFET is off
      
      delay(2000);                     // Show message for 2 seconds
      digitalWrite(RED_LED, LOW);      // Turn off Red LED
      
      lcd.clear();
      lcd.print("  Door Locked");
      delay(2000);
      
      lcd.clear();
      lcd.print(" Access Control");
      lcd.setCursor(0, 1);
      lcd.print("  System Ready");
    } else {
      denyAccess("Room is occupied");
      reason = "Room is occupied";
    }
    authorized = false;
    userName = "Unauthorized";
  }
  
// Send data to PLX-DAQ with 12-hour time format
  DateTime now = rtc.now();
  Serial.print("DATA,");
  Serial.print(authorized ? "Granted" : "Denied");
  Serial.print(",");

  // Format time in 12-hour format with AM/PM
  int displayHour = now.hour();
  String ampm = "AM";
    
  if (displayHour == 0) {
    displayHour = 12;  // Midnight
    ampm = "AM";
  } else if (displayHour == 12) {
    ampm = "PM";       // Noon
  } else if (displayHour > 12) {
    displayHour -= 12; // PM hours
    ampm = "PM";
  }

  Serial.print(displayHour, DEC);
  Serial.print(":");
  if (now.minute() < 10) Serial.print('0');
  Serial.print(now.minute(), DEC);
  Serial.print(":");
  if (now.second() < 10) Serial.print('0');
  Serial.print(now.second(), DEC);
  Serial.print(" ");
  Serial.print(ampm);
  Serial.print(",");

  // Continue with date and other fields
  Serial.print(now.year(), DEC);
  Serial.print("/");
  Serial.print(now.month(), DEC);
  Serial.print("/");
  Serial.print(now.day(), DEC);
  Serial.print(",");
  Serial.print(cardUID);
  Serial.print(",");
  Serial.print(userName);
  Serial.print(",");
  Serial.println(reason);
}

// Access Granted Function Display (original function kept intact)
void grantAccess(String name) {
  lcd.print("Access Granted!");
  lcd.setCursor(0, 1);
  lcd.print(name);
  
  digitalWrite(GREEN_LED, HIGH);   // Turn on Green LED
  digitalWrite(LOCK_PIN, LOW);     // Activate relay to unlock the solenoid lock
  digitalWrite(MOSFET_PIN, HIGH);  // Turn on MOSFET to control solenoid
  
  delay(2000);                    // Keep open for 5 seconds (increased from 2 to 5)
  
  digitalWrite(LOCK_PIN, HIGH);    // Deactivate relay to lock the solenoid
  digitalWrite(MOSFET_PIN, LOW);   // Turn off MOSFET
  
  lcd.clear();
  lcd.print("  Door Opened");
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Room is occupied");
  lcd.setCursor(0, 1);
  lcd.print("by ");
  lcd.print(name);
  digitalWrite(GREEN_LED, LOW);    // Turn off Green LED
}

// New function for extended access
void extendedAccessGrant(String name) {
  lcd.clear();
  lcd.print("Access Granted!");  // Add this line
  lcd.setCursor(0, 1);
  lcd.print(name);
  
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(LOCK_PIN, LOW);     // Activate relay to unlock the solenoid lock
  digitalWrite(MOSFET_PIN, HIGH);  // Turn on MOSFET to control solenoid   // Turn on Green LED
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
  
  delay(2000);
  
  lcd.clear();
  lcd.print("Room occupied by:");
  lcd.setCursor(0, 1);
  lcd.print(name);
}

// Access Denied Function Display with reason (original function kept intact)
void denyAccess(String reason) {
  Serial.print("Room is occupied: ");
  Serial.println(reason);
  
  lcd.clear();
  lcd.print(" Room is occupied ");
  lcd.setCursor(0, 1);
  lcd.print(reason);  
  
  digitalWrite(RED_LED, HIGH);     // Turn on Red LED
  
  // Ensure the lock is closed by setting relay to inactive
  digitalWrite(LOCK_PIN, HIGH);    // Deactivate relay, keeping solenoid lock engaged
  digitalWrite(MOSFET_PIN, LOW);   // Ensure MOSFET is off
  Serial.println("Relay: OFF - Solenoid Lock: LOCKED");
  
  delay(2000);                    // Display message for 2 seconds
  digitalWrite(RED_LED, LOW);      // Turn off Red LED
  
  lcd.clear();
  lcd.print("  Door Locked");
  delay(2000);
  
  lcd.clear();
  lcd.print(" Access Control");
  lcd.setCursor(0, 1);
  lcd.print("  System Ready");
}
