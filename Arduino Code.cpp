#include <SPI.h>
#include <RFID.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <RTClib.h>    
#include <SoftwareSerial.h>

#define SS_PIN 7       // RFID SDA connected to pin 7
#define RST_PIN 4      // RFID RST connected to pin 4
#define LOCK_PIN 8     // Relay control connected to pin 8
#define RED_LED 6      // Red LED on pin 6
#define GREEN_LED 5    // Green LED on pin 5
#define MOSFET_PIN 2   // MOSFET control on pin 2

// SIM800L Setup
SoftwareSerial sim800l(3, 10); // RX=3, TX=10
String adminPhone = "+639765480751"; // Admin phone number

// RFID Module Setup
RFID rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;       

// Days of the week array stored in PROGMEM to save RAM
const char day0[] PROGMEM = "Sunday";
const char day1[] PROGMEM = "Monday";
const char day2[] PROGMEM = "Tuesday";
const char day3[] PROGMEM = "Wednesday";
const char day4[] PROGMEM = "Thursday";
const char day5[] PROGMEM = "Friday";
const char day6[] PROGMEM = "Saturday";

const char* const daysOfTheWeek[] PROGMEM = {day0, day1, day2, day3, day4, day5, day6};

// Master Access Card Configuration
String masterCardUID = "53622439"; 
String masterCardName = "Master Key";
const unsigned long MASTER_ACCESS_DURATION = 600000; // 10 minutes in milliseconds

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

// Global variables for extended access functionality
unsigned long doorUnlockedUntil = 0;  
String currentOccupant = "";          
bool doorMaintainedOpen = false;      
bool masterAccessActive = false;      // New: Track if master access is active
unsigned long masterUnlockUntil = 0;  // New: Track master access timer
unsigned long lastSMSTime = 0;        // To prevent SMS spam
const unsigned long SMS_COOLDOWN = 30000; // 30 seconds between SMS

const int MAX_USERS = 5; 
AuthorizedUser users[MAX_USERS] = {
  // Mr. Hans can access 24/7 (all days, all hours)
  {"538A1C2F", "  Mr. Hans", 0, 0, 23, 59, {true, true, true, true, true, true, true}}
  // Add more users as needed (max 4 more due to memory constraints)
};

void setup() {
  Serial.begin(9600);
  sim800l.begin(9600);
  SPI.begin();
  Wire.begin();      
  
  // Initialize SIM800L
  initializeSIM800L();
  
  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println(F("RTC Error!"));
    lcd.clear();
    lcd.print(F("RTC Error!"));
    while (1);
  }
  
  // Uncomment and set current time if needed
  // rtc.adjust(DateTime(2025, 5, 23, 14, 30, 0));

  rfid.init();
  lcd.init();
  lcd.backlight();
  
  pinMode(LOCK_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(MOSFET_PIN, OUTPUT);
  
  digitalWrite(LOCK_PIN, HIGH);    
  digitalWrite(RED_LED, LOW);      
  digitalWrite(GREEN_LED, LOW);    
  digitalWrite(MOSFET_PIN, LOW);   
  
  lcd.print(F(" Access Control"));
  lcd.setCursor(0, 1);
  lcd.print(F("  System Ready"));
  delay(2000);
  
  // Setup for Excel logging via PLX-DAQ
  Serial.println(F("CLEARDATA"));
  Serial.println(F("LABEL, Access, Time, Date, Keycard UID, Name, Reason"));
  Serial.println(F("RESETTIMER"));
  
  Serial.println(F("System Ready - 24/7 Access Enabled"));
  Serial.print(F("Master Card UID: "));
  Serial.println(masterCardUID);
  
  // Send startup notification
  sendSMS("Access Control System Started Successfully");
}

void loop() {
  if (rfid.isCard()) {
    rfid.readCardSerial();
    String cardUID = getCardUID();
    
    Serial.print(F("Card: "));
    Serial.println(cardUID);
    
    checkAccess(cardUID);
    rfid.halt();
  }
  
  maintainDoorState();
  checkMasterAccessTimer(); // Check master access timer
}

// Initialize SIM800L module
void initializeSIM800L() {
  Serial.println(F("Initializing SIM800L..."));
  
  // Send AT command to check if module is ready
  sim800l.println("AT");
  delay(1000);
  
  // Set SMS mode to text
  sim800l.println("AT+CMGF=1");
  delay(1000);
  
  // Enable caller ID
  sim800l.println("AT+CLIP=1");
  delay(1000);
  
  Serial.println(F("SIM800L Initialized"));
}

// Function to send SMS
bool sendSMS(String message) {
  // Check cooldown to prevent SMS spam
  if (millis() - lastSMSTime < SMS_COOLDOWN) {
    return false;
  }
  
  Serial.println(F("Sending SMS..."));
  
  // Set SMS recipient
  sim800l.print("AT+CMGS=\"");
  sim800l.print(adminPhone);
  sim800l.println("\"");
  delay(1000);
  
  // Send message content
  sim800l.print(message);
  delay(100);
  
  // Send Ctrl+Z to finish SMS
  sim800l.write(26);
  delay(5000);
  
  lastSMSTime = millis();
  Serial.println(F("SMS Sent"));
  return true;
}

// Function to create formatted SMS message with timestamp
String createSMSMessage(String event, String name, String reason) {
  DateTime now = rtc.now();
  String message = "ACCESS ALERT\n";
  message += event + "\n";
  message += "User: " + name + "\n";
  message += "Reason: " + reason + "\n";
  
  // Format time in 12-hour format
  int displayHour = now.hour();
  String ampm = "AM";
    
  if (displayHour == 0) {
    displayHour = 12;
    ampm = "AM";
  } else if (displayHour == 12) {
    ampm = "PM";
  } else if (displayHour > 12) {
    displayHour -= 12;
    ampm = "PM";
  }

  message += "Time: ";
  message += String(displayHour);
  message += ":";
  if (now.minute() < 10) message += "0";
  message += String(now.minute());
  message += " " + ampm + "\n";
  
  message += "Date: ";
  message += String(now.month());
  message += "/";
  message += String(now.day());
  message += "/";
  message += String(now.year());
  
  return message;
}

// Convert RFID serial number to string
String getCardUID() {
  String uid = "";
  uid.reserve(8); 
  
  for (int i = 0; i < 4; i++) {
    if (rfid.serNum[i] < 0x10) {
      uid += "0";
    }
    uid += String(rfid.serNum[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// Check if current time is within allowed access hours for a user
bool isWithinAllowedTime(AuthorizedUser user) {
  DateTime now = rtc.now();
  int currentDay = now.dayOfTheWeek();
  int currentHour = now.hour();
  int currentMinute = now.minute();
  
  // Check if today is an allowed day
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

// New: Check master access timer
void checkMasterAccessTimer() {
  if (masterAccessActive && millis() > masterUnlockUntil) {
    // Master access time is up, lock the door
    digitalWrite(LOCK_PIN, HIGH);    
    digitalWrite(MOSFET_PIN, LOW);   
    digitalWrite(GREEN_LED, LOW);    
    
    lcd.clear();
    lcd.print(F("  Door Locked"));
    lcd.setCursor(0, 1);
    lcd.print(F("Master timer end"));
    
    // Send SMS notification about door locking
    String smsMessage = "DOOR LOCKED\n";
    smsMessage += "User: " + masterCardName + "\n";
    smsMessage += "Reason: 10-minute timer expired\n";
    DateTime now = rtc.now();
    smsMessage += "Time: " + formatTime(now);
    sendSMS(smsMessage);
    
    masterAccessActive = false;
    
    delay(2000);
    lcd.clear();
    lcd.print(F(" Access Control"));
    lcd.setCursor(0, 1);
    lcd.print(F("  System Ready"));
  }
}

// Check if door should remain open based on schedule
void maintainDoorState() {
  if (!doorMaintainedOpen) {
    return;
  }
  
  if (millis() > doorUnlockedUntil) {
    // Time is up, lock the door
    digitalWrite(LOCK_PIN, HIGH);    
    digitalWrite(MOSFET_PIN, LOW);   
    digitalWrite(GREEN_LED, LOW);    
    
    lcd.clear();
    lcd.print(F("  Door Locked"));
    lcd.setCursor(0, 1);
    lcd.print(F("Schedule ended"));
    
    // Send SMS notification about door locking
    String smsMessage = "DOOR LOCKED\n";
    smsMessage += "User: " + currentOccupant + "\n";
    smsMessage += "Reason: Schedule ended\n";
    DateTime now = rtc.now();
    smsMessage += "Time: " + formatTime(now);
    sendSMS(smsMessage);
    
    doorMaintainedOpen = false;
    currentOccupant = "";
    
    delay(2000);
    lcd.clear();
    lcd.print(F(" Access Control"));
    lcd.setCursor(0, 1);
    lcd.print(F("  System Ready"));
  }
}

// Helper function to format time
String formatTime(DateTime dt) {
  int displayHour = dt.hour();
  String ampm = "AM";
    
  if (displayHour == 0) {
    displayHour = 12;
    ampm = "AM";
  } else if (displayHour == 12) {
    ampm = "PM";
  } else if (displayHour > 12) {
    displayHour -= 12;
    ampm = "PM";
  }

  String timeStr = String(displayHour) + ":";
  if (dt.minute() < 10) timeStr += "0";
  timeStr += String(dt.minute()) + " " + ampm;
  
  return timeStr;
}

// Handle master card access
void grantMasterAccess() {
  lcd.clear();
  lcd.print(F("Master Access!"));
  lcd.setCursor(0, 1);
  lcd.print(F("10 min timer"));
  
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(LOCK_PIN, LOW);     
  digitalWrite(MOSFET_PIN, HIGH);  
  
  // Set master access timer for 10 minutes
  masterUnlockUntil = millis() + MASTER_ACCESS_DURATION;
  masterAccessActive = true;
  
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
  
  // Show remaining time countdown
  lcd.clear();
  lcd.print(F("Door unlocked"));
  lcd.setCursor(0, 1);
  lcd.print(F("Master: 10 min"));
}

// Modified checkAccess function with master card support
void checkAccess(String cardUID) {
  bool authorized = false;
  String userName = "";
  String reason = ""; 
  
  // Check if it's the master card
  if (cardUID == masterCardUID) {
    grantMasterAccess();
    authorized = true;
    userName = masterCardName;
    reason = "Master access granted (10 min)";
    
    // Log to PLX-DAQ
    logAccessAttempt(authorized, userName, reason, cardUID);
    return;
  }
  
  // Check regular users
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i].rfid == "") {
      break;
    }
    
    if (users[i].rfid == cardUID) {
      if (isWithinAllowedTime(users[i])) {
        // Calculate how long to keep the door open
        DateTime now = rtc.now();
        int endTimeInMinutes = users[i].endHour * 60 + users[i].endMinute;
        
        unsigned long currentMinuteOfDay = now.hour() * 60 + now.minute();
        unsigned long minutesRemaining = endTimeInMinutes - currentMinuteOfDay;
        unsigned long millisecondsRemaining = minutesRemaining * 60 * 1000;
        
        doorUnlockedUntil = millis() + millisecondsRemaining;
        doorMaintainedOpen = true;
        currentOccupant = users[i].name;
        
        extendedAccessGrant(users[i].name);
        authorized = true;
        userName = users[i].name;
        reason = "Access granted";
        
        // Send SMS notification for successful access
        String smsMessage = createSMSMessage("ACCESS GRANTED", userName, reason);
        sendSMS(smsMessage);
        
      } else {
        if (doorMaintainedOpen || masterAccessActive) {
          String occupiedBy = doorMaintainedOpen ? currentOccupant : masterCardName;
          showDeniedMessage(F("Room occupied"));
          reason = "Room occupied by " + occupiedBy;
        } else {
          denyAccess(F("Outside schedule"));
          reason = "Outside scheduled hours";
        }
        authorized = false;
        userName = users[i].name;
        
        // Send SMS notification for denied access
        String smsMessage = createSMSMessage("ACCESS DENIED", userName, reason);
        sendSMS(smsMessage);
      }
      break;
    }
  }
  
  // If not found in the list of authorized users
  if (userName == "") {
    if (doorMaintainedOpen || masterAccessActive) {
      String occupiedBy = doorMaintainedOpen ? currentOccupant : masterCardName;
      reason = "Room occupied by " + occupiedBy;
      showDeniedMessage(F("Room occupied"));
    } else {
      denyAccess(F("  Unauthorized"));
      reason = "Unauthorized card";
    }
    authorized = false;
    userName = "Unauthorized";
    
    // Send SMS notification for unauthorized access attempt
    String smsMessage = createSMSMessage("UNAUTHORIZED ACCESS ATTEMPT", "Unknown User", reason);
    smsMessage += "\nCard UID: " + cardUID;
    sendSMS(smsMessage);
  }
  
  // Log to PLX-DAQ
  logAccessAttempt(authorized, userName, reason, cardUID);
}


void logAccessAttempt(bool authorized, String userName, String reason, String cardUID) {
  DateTime now = rtc.now();
  Serial.print(F("DATA,"));
  Serial.print(authorized ? F("Granted") : F("Denied"));
  Serial.print(F(","));

  // Format time in 12-hour format with AM/PM
  int displayHour = now.hour();
  String ampm = F("AM");
    
  if (displayHour == 0) {
    displayHour = 12;
    ampm = F("AM");
  } else if (displayHour == 12) {
    ampm = F("PM");
  } else if (displayHour > 12) {
    displayHour -= 12;
    ampm = F("PM");
  }

  Serial.print(displayHour, DEC);
  Serial.print(F(":"));
  if (now.minute() < 10) Serial.print(F("0"));
  Serial.print(now.minute(), DEC);
  Serial.print(F(":"));
  if (now.second() < 10) Serial.print(F("0"));
  Serial.print(now.second(), DEC);
  Serial.print(F(" "));
  Serial.print(ampm);
  Serial.print(F(","));

  Serial.print(now.year(), DEC);
  Serial.print(F("/"));
  Serial.print(now.month(), DEC);
  Serial.print(F("/"));
  Serial.print(now.day(), DEC);
  Serial.print(F(","));
  Serial.print(cardUID);
  Serial.print(F(","));
  Serial.print(userName);
  Serial.print(F(","));
  Serial.println(reason);
}

void showDeniedMessage(const __FlashStringHelper* reason) {
  lcd.clear();
  lcd.print(F("Access Denied"));
  lcd.setCursor(0, 1);
  lcd.print(reason);  
  
  digitalWrite(RED_LED, HIGH);
  delay(2000);
  digitalWrite(RED_LED, LOW);
  
  if (doorMaintainedOpen && currentOccupant != "") {
    lcd.clear();
    lcd.print(F("Room occupied by:"));
    lcd.setCursor(0, 1);
    lcd.print(currentOccupant);
  } else if (masterAccessActive) {
    lcd.clear();
    lcd.print(F("Room occupied by:"));
    lcd.setCursor(0, 1);
    lcd.print(masterCardName);
  } else {
    lcd.clear();
    lcd.print(F(" Access Control"));
    lcd.setCursor(0, 1);
    lcd.print(F("  System Ready"));
  }
}

void extendedAccessGrant(String name) {
  lcd.clear();
  lcd.print(F("Access Granted!"));
  lcd.setCursor(0, 1);
  lcd.print(name);
  
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(LOCK_PIN, LOW);     
  digitalWrite(MOSFET_PIN, HIGH);  
  
  delay(2000);
  digitalWrite(GREEN_LED, LOW);
  delay(2000);
  
  lcd.clear();
  lcd.print(F("Room occupied by:"));
  lcd.setCursor(0, 1);
  lcd.print(name);
}

void denyAccess(const __FlashStringHelper* reason) {
  lcd.clear();
  lcd.print(F("  Access Denied"));
  lcd.setCursor(0, 1);
  lcd.print(reason);  
  
  digitalWrite(RED_LED, HIGH);
  digitalWrite(LOCK_PIN, HIGH);    
  digitalWrite(MOSFET_PIN, LOW);   
  
  delay(4000);
  digitalWrite(RED_LED, LOW);
  
  lcd.clear();
  lcd.print(F("  Door Locked"));
  delay(2000);
  
  lcd.clear();
  lcd.print(F(" Access Control"));
  lcd.setCursor(0, 1);
  lcd.print(F("  System Ready"));
}
