// This sketch was originally adapted from: https://github.com/donskytech/esp-projects/tree/master/esp8266-nodemcu-webserver-ajax
// Tutorial here: https://www.hackster.io/donskytech/nodemcu-esp8266-ajax-enabled-web-server-8b0744
// Forked NTP library no longer required.

// This sketch controls an automated backyard system. As is, it is able to cotrol four sprinklers, an automatic pet feeder, an automatic pet drinking bowl, switch on/off an ip cam, and play a tune via a buzzer after feeding.
// It delivers food four times a day, with the water being flushed/refilled each hour at a set time. There is also an 'Food+' button to drop additional food when pressed.
// The whole project is covered in this YouTube video: https://youtu.be/Hz5E83ftKio

// Disclaimer: Use this code at your own risk. I have been using it reliably for some time but there are no guarantees, especially if
// you make changes. Do NOT let this be the only thing in place to keep your pets alive. Please ensure you have a backup in place if
// something fails and a way of knowing that the backup is needed. I accept no responsibilty for any adverse outcomes that stem from
// the use of this code.

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266RTTTLPlus.h>
#include <TimeLib.h>

/********** PLEASE CHANGE THIS *************************/
#ifndef STASSID
#define STASSID "" // Your WIFI SSID here
#define STAPSK  "" // Your WIFI password here
#endif

const char* ssid = STASSID;
const char* password = STAPSK;

const long utcOffsetInSeconds = 39600; // This is to suit Sydney timezone. +11 hours * 60 minutes and 60 seconds. Adjust as necessary for your location.
int meals = 0; // Variable to keep track of how many meals the pets have had throughout the day.
int oldMeals = 0; // Temporary storage variable for when feed is manually topped up.
unsigned long finishMillis = 0; // Time variable used to turn off the feeder after a certain amount of time.
unsigned long finishMillisW = 0; // Time variable used to turn off the drinker after a certain amount of time.

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds, 60000); // Connects to the internet to look up the time.

static char melodyBuffer[500] = "fotc:d=4,o=5,b=200:4b6,8p,g6,8p,4e6,4d6,4p,8p,b6,8p,g6,8p,e6,2b6"; //Flight of the Conchords - "It's Business Time", written in Nokia ringtone format. I used this free software to work out the notes: https://download.cnet.com/Coding-Workshop-Ringtone-Converter/3000-2169_4-10071441.html
static int currentVolume = 10;   // To match default volume hard-coded in HTML

ESP8266WebServer server(80); // Web server is created.

// My pins. Reference this page to see which pins are best to use: https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/
uint8_t output1 = D5; // right sprinkler
uint8_t output2 = D6; // centre sprinkler
uint8_t output3 = D0; // left sprinkler
uint8_t output4 = D1; // vege garden sprinkler
uint8_t output5 = D2; // feeder motor
uint8_t output6 = D7; // water solenoid
uint8_t output7 = D8; // webcam MOSFET
uint8_t vin = A0;     // voltage input
uint8_t buzzer = D3;  // buzzer

// I used active low relays, which means they trigger when the MCU grounds them. If you are using active high relays (5V+ trigger), reverse the HIGH an LOW variables throughout where necessary
bool output1state = HIGH;
bool output2state = HIGH;
bool output3state = HIGH;
bool output4state = HIGH;
bool output5state = HIGH;
bool output6state = HIGH;
bool output7state = HIGH; // I used a P-channel MOSFET. An N-channel MOSFET would probably need the logic reversed.
bool feeding = false; // boolean variable to keep track of whether feeding is in progress
bool drinking = false; // boolean variable to keep track of whether water refill is in progress

// Scheduled meal times, in 24hr format
int breakfast[] = {6,00}; // 6:00 am
int brunch[] = {10,00}; // 10:00 am
int lunch[] = {13,00}; // 1:00 pm
int dinner[] = {16,30}; // 4:30 pm
int feedPumps[] = {4,3,3,4}; // How much food to deliver for breakfast, brunch, lunch and dinner. If you wan to leave out a meal, set its value to zero.
const int feedTime = 1500; // My feeder drops food every 1.5 seconds. Adjust as necessary for yours.
int watering[] = {6,20,5}; // Times for drink refill: Start hour, end hour, minutes past the hour. Will happen every hour on and between those listed. eg 6 am and 8pm. Third number is minutes past the hour to activate water. Eg. 05. Pick a time different to your feeding to avoid clashes.
const int drinkTime = 2000; // 2 seconds is a good amount of time to refill/flush my water system. Adjust as necessary for yours.

time_t boot; // String to store information about when the system was first turned on. String
int bootDay; // Stores day of the week value when first turned on.

void setup() {
  Serial.begin(115200);
  // Pin assignments
  pinMode(output1, OUTPUT);
  pinMode(output2, OUTPUT);
  pinMode(output3, OUTPUT);
  pinMode(output4, OUTPUT);
  pinMode(output5, OUTPUT);
  pinMode(output6, OUTPUT);
  pinMode(output7, OUTPUT);
  pinMode(vin, INPUT);
  pinMode(buzzer, OUTPUT);
  // Set initial output states for relays
  digitalWrite(output1, HIGH);
  digitalWrite(output2, HIGH);
  digitalWrite(output3, HIGH);
  digitalWrite(output4, HIGH);
  digitalWrite(output5, HIGH);
  digitalWrite(output6, HIGH);
  digitalWrite(output7, HIGH);

  // Connect WIFI to local network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    delay(5000);
    ESP.restart();
  }

  // Arduino OTA updating functions taken from example sketch
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

  // NOTE: if updating FS this would be the place to unmount FS using FS.end()
  Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();

  // These lines tell the server what function to run, depending on the web request received. 
  server.on("/", handleRoot);
  server.on("/toggle1", update1);
  server.on("/toggle2", update2);
  server.on("/toggle3", update3);
  server.on("/toggle4", update4);
  server.on("/toggle5", update5);
  server.on("/toggle6", update6);
  server.on("/toggle7", update7);
  server.on("/updateVoltage", updateVoltage);
  server.on("/playFotc", playFotc);
  server.on("/status", updateStatus);
  server.on("/manualFeed", manualFeed);
  server.onNotFound(handleNotFound);
  server.begin(); // Server actually starts
  timeClient.begin(); // Connect to internet and get time
  timeClient.update(); // Update time
  boot = timeClient.getEpochTime(); // Store date which boot up happened
  // set initial time of day for meals
  int bootTime = (timeClient.getHours()*100) + (timeClient.getMinutes()); // Convert boot time to 24hr format integer. Eg. 4:56pm becomes 1656
  int breakfastTime = (breakfast[0]*100) + breakfast[1]; // Convert breakfast time to 24hr format integer.
  int brunchTime = (brunch[0]*100) + brunch[1]; // Convert brunch time to 24hr format integer.
  int lunchTime = (lunch[0]*100) + lunch[1];  // Convert lunch time to 24hr format integer.
  int dinnerTime = (dinner[0]*100) + dinner[1]; // Convert dinner time to 24hr format integer.
  // Compare the boot up time to the scheduled feeding times to determine which meal shoud be next. The meals variable will be set to keep track of this.
  if(bootTime > dinnerTime) { //after dinner, next meal is breakfast, reset to 0 for morning
    meals = 0;
  } else if(bootTime > lunchTime){ // after lunch, next meal is dinner
    meals = 3;
   } else if(bootTime > brunchTime){ // after brunch, next meal is lunch
    meals = 2;  
  } else if (bootTime > breakfastTime){ // after breakfast, next meal is brunch
    meals = 1;
  } else { // early morning, next meal is breakfast
    meals = 0;
  }
}
 
void loop() {
  ArduinoOTA.handle(); // Taken from OTA example sketch.
  server.handleClient(); // Check for and handle incoming web requests.
  if(feeding == true) { // Check if feeding is taking place and stop it if enough time has elapsed.
    if(millis() > finishMillis){
      stopFeed();
    }
  } else { //Check if it is time to start feeding for a scheduled meal.
    if(meals == 0) { //early morning
      if(timeClient.getHours() == breakfast[0] && timeClient.getMinutes() == breakfast[1]) {
        feed();
      }
    }
    if(meals == 1) { // pre brunch
      if(timeClient.getHours() == brunch[0] && timeClient.getMinutes() == brunch[1]) {
        feed();
      }
    }
    if(meals == 2) { // pre lunch
      if(timeClient.getHours() == lunch[0] && timeClient.getMinutes() == lunch[1]) {
        feed();
      }
    }
    if(meals == 3) { // pre dinner
      if(timeClient.getHours() == dinner[0] && timeClient.getMinutes() == dinner[1]) {
        feed();
      }
    }
  }
  if(drinking == true){ // Check if drinking is taking place and stop it if enough time has elapsed.
    if(millis() > finishMillisW){
      digitalWrite(output6, HIGH); // Turn off relay to drinker solenoid.
      drinking = false; // set drinking variable as false for main loop.
    }
  } else { // Check for regular water refill time
    if((timeClient.getHours() >= watering[0]) && (timeClient.getHours() <= watering[1]) && (timeClient.getMinutes() == watering[2]) && (timeClient.getSeconds() == 0)){ // 5 minutes past the hour between 6am and 8pm
      digitalWrite(output6, LOW); // Trigger relay to drinker solenoid
      finishMillisW = millis()+drinkTime; // Set how long water will come out using variables at the top. In my case 2000 ms.
      drinking = true; // set drinking variable as true for main loop to handle.
    }
  }
  e8rtp::loop(); // Command required in loop for the buzzer to be able to play without delaying other functions.
}

void feed(){ // Function to start feeding
  digitalWrite(output5, LOW); // Trigger relay to feeder motor
  if(meals == 0){ // breakfast
    finishMillis = millis()+(feedTime*feedPumps[0]); // Set feeding duration as per variables higher up. Eg. 1500 ms * 4 pumps = 6 seconds of food delivery
  }
  if(meals == 1){ // brunch
    finishMillis = millis()+(feedTime*feedPumps[1]);
  }
  if(meals == 2){ // lunch
    finishMillis = millis()+(feedTime*feedPumps[2]);
  }
  if(meals == 3){ // dinner
    finishMillis = millis()+(feedTime*feedPumps[3]);
  }
  feeding = true; // set feeding variable as true for main loop to handle.
}

void manualFeed(){ // Function to handle if the user presses the 'Food +' button for manual food top up.
  digitalWrite(output5, LOW); // Trigger relay to feeder motor
  if(oldMeals == 9){ // Check if a manual feed is currently underway
    return; // Exit if it is to avoid meals counter being stuck on 9
  }
  oldMeals = meals; // Temporarily store the next scheduled meal in the oldMeals variable.
  meals = 9; // Bogus meals value to 9 to indicate a manual, unscheduled feed.
  finishMillis = millis()+feedTime; // Set how long feed will come out using variables above. In my case 1500 ms.
  feeding = true; // set feeding variable as true for main loop to handle.
}

void stopFeed() { // Function to handle when feeding is over
  digitalWrite(output5, HIGH); // Turn off relay to feeder motor
  feeding = false; // set feeding variable as false for main loop.
  if(meals == 9){ // If this was a manual, unscheduled feed, restore the next scheduled meal.
    meals = oldMeals;
  } else { // Scheduled feed    
    if(meals == 3){ // Advance the meals counter by one, unless it's the end of the day, in which reset it to zero for the next day.
      meals = 0;
    } else {
      meals++;
    }
  }
  playFotc(); // Call function to play tune, to call the pets to the fod.
}

void handleRoot() { // Function to load the base webpage.
  server.send(200, "text/html", prepareHTML());
}

void update1() { // Function to handle output 1 being toggled.
  String output1stateParam = server.arg("output1state"); // Check the server argument.
  if (output1stateParam == "ON"){ // Switch the state from high to low or vice versa.
    output1state =  LOW;
  } else {
    output1state =  HIGH;
  }
  digitalWrite(output1, output1state); // Set the output pin as per the state.
  server.send(200, "text/plain", "Success!"); // Tell the device which made the request it was successful.
}

void update2() {
  String output2stateParam = server.arg("output2state");
  if (output2stateParam == "ON"){
    output2state =  LOW;
  } else {
    output2state =  HIGH;
  }
  digitalWrite(output2, output2state);
  server.send(200, "text/plain", "Success!");
}

void update3() {
  String output3stateParam = server.arg("output3state");
  if (output3stateParam == "ON"){
    output3state =  LOW;
  } else {
    output3state =  HIGH;
  }
  digitalWrite(output3, output3state);
  server.send(200, "text/plain", "Success!");
}

void update4() {
  String output4stateParam = server.arg("output4state");
  if (output4stateParam == "ON"){
    output4state =  LOW;
  } else {
    output4state =  HIGH;
  }
  digitalWrite(output4, output4state);
  server.send(200, "text/plain", "Success!");
}

void update5() {
  String output5stateParam = server.arg("output5state");
  if (output5stateParam == "ON"){
    output5state =  LOW;
  } else {
    output5state =  HIGH;
  }
  digitalWrite(output5, output5state);
  server.send(200, "text/plain", "Success!");
}

void update6() {
  String output6stateParam = server.arg("output6state");
  if (output6stateParam == "ON"){
    output6state =  LOW;
  } else {
    output6state =  HIGH;
  }
  digitalWrite(output6, output6state);
  server.send(200, "text/plain", "Success!");
}

void update7() {
  String output7stateParam = server.arg("output7state");
  if (output7stateParam == "ON"){
    output7state =  LOW;
  } else {
    output7state =  HIGH;
  }
  digitalWrite(output7, output7state);
  server.send(200, "text/plain", "Success!");
}

float voltage() { // Function to handle reading the input voltage to measure battery level. The A0 pin is used, with a 1.2 M Ohm resistor in place. This board already has a voltage divider to read input voltages of more than 1V. This additional resistor in series allows it to read exernal voltages of up to ~14V.
  float reading = analogRead(vin); // Read the A0 pin voltage.
  reading = reading/1023*13.4; // 13.4 is a multiplier variable that you should change to get the correct reading. Compare a multimeter output with what the board is reporting and adjust until accrate.
  return reading;  
}

void updateVoltage() { // Function to handle when the user presses the voltage reading in the webpage. Calls the voltage measuring function above and returns the result to the webpage for it to be updated.
  float v = voltage(); // Measure voltage.
  String message = String(v, 2); // Convert to string.
  message.concat(" V"); // Add V for volts on the end.
  server.send(200, "text/plain", message); // Send back to the webpage.
}

String dotw(int day) { //Function to convert the day of the week (integer 1-7) from the Time library to a human friendly string: Monday, Tuesday, Wednesday, etc.
  switch(day){
    case 1:
      return "Sunday";
      break;
    case 2:
      return "Monday";
      break;
    case 3:
      return "Tuesday";
      break;
    case 4:
      return "Wednesday";
      break;
    case 5:
      return "Thursday";
      break;
    case 6:
      return "Friday";
      break;
    case 7:
      return "Saturday";
      break;
    default:
      return "Unknown";
      break;
  }    
}

String formatDate(int t){ //Function to take an epoch seconds value and convert it into a 24H formatted date string: dotw DD/MM/YYYY HH:MM:SS
  String message = String(""); // Start empty string
  message.concat(dotw(weekday(t))); // Create text day of the week
  message.concat(" ");
  message.concat(day(t)); 
  message.concat("/");
  message.concat(month(t));
  message.concat("/");
  message.concat(year(t));
  message.concat(" ");
  message.concat(hour(t));
  message.concat(":");
  if(minute(t) < 10){
    message.concat("0"); // Pada leading zero if needed
  }
  message.concat(minute(t));
  message.concat(":");
  if(second(t) < 10){
    message.concat("0");
  }
  message.concat(second(t));
  return message;
}

String status() { //Function to prepare a status string for the base of the page. Returns the boot day and time, the current date and time of the request, and the next scheduled meal.
  String message = String("<p>Online since ");
  message.concat(formatDate(boot)); // Format date and time string for when the system was booted
  message.concat(".</p><p>Last updated ");
  time_t t = timeClient.getEpochTime(); // Capture curren time in seconds since epoch
  message.concat(formatDate(t)); // Format date and time string for when the system was booted
  message.concat(".</p><p>");
  message.concat("The next meal is ");
  if(meals == 0){
    message.concat("breakfast at ");
    message.concat(breakfast[0]);
    message.concat(":");
    if(breakfast[1] < 10){
      message.concat("0");
    }
    message.concat(breakfast[1]);
  } else if(meals ==1){
    message.concat("brunch at ");
    message.concat(brunch[0]);
    message.concat(":");
    if(brunch[1] < 10){
      message.concat("0");
    }
    message.concat(brunch[1]);
  } else if(meals ==2){
    message.concat("lunch at ");
    message.concat(lunch[0]);
    message.concat(":");
    if(lunch[1] < 10){
      message.concat("0");
    }
    message.concat(lunch[1]);
  } else if(meals == 3){
    message.concat("dinner at ");
    message.concat(dinner[0]);
    message.concat(":");
    if(dinner[1] < 10){
      message.concat("0");
    }
    message.concat(dinner[1]);
  } else {
    message.concat("error. Current meal is: ");
    message.concat(meals);
  }
  message.concat(" for ");
  message.concat(feedPumps[meals]);
  message.concat(" pumps.</p>");
  return message;
}

void updateStatus() { // Function to handle the status update request at the base of the page.
  String message = status();
  server.send(200, "text/plain", message);
}

void playFotc() { // Function to start playing the jingle on the buzzer after feeding
    e8rtp::setup(buzzer, currentVolume, melodyBuffer);    
    e8rtp::start();
}

void handleNotFound() { // Functio to handle an unknown web request.
  server.send(404, "text/plain", "Not found");
}

String prepareHTML() { // Function to serve the html for the base webpage. Used when ip address is first entered.
  // The html is mostly one long string. It can continue over multiple lines for reading clarity as long at that line ends with \n" , NOT \n";
  // The semi-colon will end the string and break the page. Only use a semi-colon to end the entire string or when you have conditional gcode. Restart the string with html +=
  // BuildMyString.com generated code. Please enjoy your string responsibly.

  String html = "<!DOCTYPE html>\n"
                "<html>\n"
                "  <head>\n"
                "   <meta charset=\"UTF-8\">\n"
                "   <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                "   <title>Goat Anti Bleat System</title>\n"
                // CSS
                "   <style>\n"
                "     /* Copyright 2014 Owen Versteeg; MIT licensed */body,textarea,input,select{background:0;border-radius:0;font:16px sans-serif;margin:0}.smooth{transition:all .2s}.btn,.nav a{text-decoration:none}.container{margin:0;width:auto}label>*{display:inline}form>*{display:block;margin-bottom:10px}.btn{background:#999;border-radius:6px;border:0;color:#fff;cursor:pointer;display:inline-block;margin:2px 0;padding:12px 30px 14px}.btn:hover{background:#888}.btn:active,.btn:focus{background:#777}.btn-a{background:#0ae}.btn-a:hover{background:#09d}.btn-a:active,.btn-a:focus{background:#08b}.btn-b{background:#3c5}.btn-b:hover{background:#2b4}.btn-b:active,.btn-b:focus{background:#2a4}.btn-c{background:#d33}.btn-c:hover{background:#c22}.btn-c:active,.btn-c:focus{background:#b22}.btn-sm{border-radius:4px;padding:10px 14px 11px}.row{margin:1% 0;overflow:auto}.col{float:left}.table,.c12{width:100%}.c11{width:91.66%}.c10{width:83.33%}.c9{width:75%}.c8{width:66.66%}.c7{width:58.33%}.c6{width:50%}.c5{width:41.66%}.c4{width:33.33%}.c3{width:25%}.c2{width:16.66%}.c1{width:8.33%}h1{font-size:3em}.btn,h2{font-size:2em}.ico{font:33px Arial Unicode MS,Lucida Sans Unicode}.addon,.btn-sm,.nav,textarea,input,select{outline:0;font-size:14px}textarea,input,select{padding:8px;border:1px solid #ccc}textarea:focus,input:focus,select:focus{border-color:#5ab}textarea,input[type=text]{-webkit-appearance:none;width:13em}.addon{padding:8px 12px;box-shadow:0 0 0 1px #ccc}.nav,.nav .current,.nav a:hover{background:#000;color:#fff}.nav{height:24px;padding:11px 0 15px}.nav a{color:#aaa;padding-right:1em;position:relative;top:-1px}.nav .pagename{font-size:22px;top:1px}.btn.btn-close{background:#000;float:right;font-size:25px;margin:-54px 7px;display:none}@media(min-width:1310px){.container{margin:auto;width:1270px}}@media(max-width:870px){.row .col{width:100%}}@media(max-width:500px){.btn.btn-close{display:block}.nav{overflow:hidden}.pagename{margin-top:-11px}.nav:active,.nav:focus{height:auto}.nav div:before{background:#000;border-bottom:10px double;border-top:3px solid;content:'';float:right;height:4px;position:relative;right:3px;top:14px;width:20px}.nav a{padding:.5em 0;display:block;width:50%}}.table th,.table td{padding:.5em;text-align:left}.table tbody>:nth-child(2n-1){background:#ddd}.msg{padding:1.5em;background:#def;border-left:5px solid #59d}\n"
                "     .hero {\n"
                "       background: #eee;\n"
                "       width: 100%\n"  
                //"       padding: 20px;\n"
                //"       border-radius: 10px;\n"
                "     }\n"
                "     .hero h1 {\n"
                "       margin-top: 0;\n"
                "       margin-bottom: 0.3em;\n"
                "     }\n"
                "     .c4 {\n"
                "       padding: 10px;\n"
                "       box-sizing: border-box;\n"
                "     }\n"
                "     .c4 h3 {\n"
                "       margin-top: 0;\n"
                "     }\n"
                "     \n"
                "     // Code from https://proto.io/freebies/onoff/\n"
                "     .c4 a {\n"
                "       margin-top: 10px;\n"
                "       display: inline-block;\n"
                "     }\n"
                "     \n"
                "     .onoffswitch {\n"
                "       position: relative; width: 90px;\n"
                "       -webkit-user-select:none; -moz-user-select:none; -ms-user-select: none;\n"
                "     }\n"
                "     .onoffswitch-checkbox {\n"
                "       position: absolute;\n"
                "       opacity: 0;\n"
                "       pointer-events: none;\n"
                "     }\n"
                "     .onoffswitch-label {\n"
                "       display: block; overflow: hidden; cursor: pointer;\n"
                "       border: 2px solid #999999; border-radius: 20px;\n"
                "     }\n"
                "     .onoffswitch-inner {\n"
                "       display: block; width: 200%; margin-left: -100%;\n"
                "       transition: margin 0.3s ease-in 0s;\n"
                "     }\n"
                "     .onoffswitch-inner:before, .onoffswitch-inner:after {\n"
                "       display: block; float: left; width: 50%; height: 30px; padding: 0; line-height: 30px;\n"
                "       font-size: 14px; color: white; font-family: Trebuchet, Arial, sans-serif; font-weight: bold;\n"
                "       box-sizing: border-box;\n"
                "     }\n"
                "     .onoffswitch-inner:before {\n"
                "       content: \"ON\";\n"
                "       padding-left: 10px;\n"
                "       background-color: #34A7C1; color: #FFFFFF;\n"
                "     }\n"
                "     .onoffswitch-inner:after {\n"
                "       content: \"OFF\";\n"
                "       padding-right: 10px;\n"
                "       background-color: #EEEEEE; color: #999999;\n"
                "       text-align: right;\n"
                "     }\n"
                "     .onoffswitch-switch {\n"
                "       display: block; width: 18px; margin: 6px;\n"
                "       background: #FFFFFF;\n"
                "       position: absolute; top: 0; bottom: 0;\n"
                "       right: 56px;\n"
                "       border: 2px solid #999999; border-radius: 20px;\n"
                "       transition: all 0.3s ease-in 0s; \n"
                "     }\n"
                "     .onoffswitch-checkbox:checked + .onoffswitch-label .onoffswitch-inner {\n"
                "       margin-left: 0;\n"
                "     }\n"
                "     .onoffswitch-checkbox:checked + .onoffswitch-label .onoffswitch-switch {\n"
                "       right: 0px; \n"
                "     }\n"
                "     button {\n"
                "       font-size: 25px;\n"
                "       font-weight: bold;\n"
                "       border: 2px solid #999999;\n"
                "       border-radius: 20px;\n"
                "       padding: 1px 15px;\n"
                "       margin-right: 20px;\n"
                "     }\n"                
                "     \n"
                "     .grid-container {\n"
                "       display: grid;\n"
                "       grid-template-columns: 1fr 1fr;\n"
                "       grid-gap: 20px;\n"
                "     }\n"
                "     \n"
                "     @media only screen and (min-width: 400px)  {\n"
                "       .rower {\n"
                "         display: flex;\n"
                "         flex-direction:row;\n"
                "         justify-content: center;\n"
                "       }\n"
                "       iframe{\n"
                "         width: 100%;\n"
                "         height: 600px;\n"
                "       }\n"
                "     }\n"
                "     @media only screen and (max-width: 400px)  {\n"
                "       .rower {\n"
                "         display: flex;\n"
                "         flex-direction:column;\n"
                "         justify-content: center;\n"
                "       }\n"
                "       iframe{\n"
                "         width: 100%;\n"
                "         height: 300px;\n"
                "       }\n"                
                "     }\n"           
                "     .flex-container {\n"
                "       display: flex;\n"
                "       margin: 10px;\n"
                "     }\n"
                "     .flex-child {\n"
                "       flex: 1;\n"
                "       margin: 0 20px 0 0;\n"
                "     }  \n"
                "     .flex-child:first-child {\n"
                "       margin-right: 20px;\n"
                "     } \n"
                "     \n"
                "     .component-label{\n"
                "       float: right;\n"
                "       font-weight: bold;\n"
                "       font-size: 25px;\n"
                "     }\n"
                "     #voltage{\n"
                "       position:fixed;\n"
                "       top:0;\n"
                "       left:0;\n"
                "       background-color:white;\n"
                "       margin: 5px;\n"
                "       padding: 10px;\n"
                "       opacity:0.8;\n"
                "     }\n"
                "     #fotc{\n"
                "       position:fixed;\n"
                "       top:0;\n"
                "       right:0;\n"
                "       background-color:white;\n"
                "       margin: 5px;\n"
                "       padding: 10px;\n"
                "       opacity:0.8;\n"
                "     }\n"
                "     #status{\n"
                "       border: 1px black solid;\n"
                "       padding: 0 10px;\n"                
                "     }\n"
                "   </style>\n"
                " </head>\n"
                " <body>\n"
// Voltage reading display
                "   <div id='voltage'>"; 
                html+= voltage(); // The voltag reading function is called and inserts the value into the html in this DIV.
                html+=" V</div>\n" // Append V for volts and close DIV.
// Manual buzzer tune play button
                "   <div id='fotc'>P</div>"
// Camera iframe. Insert your own camera URL here as the src or comment out the line if you are not using one.
                "   <iframe type='text/html' frameborder='0' src='' allowfullscreen></iframe>\n"
                "   <div class=\"container\">\n"
                "     <div class=\"hero\">\n"
                "     <div class='rower'>\n"
// Switch 3 html. Each switch follows the same format. My switches are listed out of order to match the layout of my back yard.
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">LEFT</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch3\" tabindex=\"0\"";
                                if(output3state == LOW){html+=" checked";}; // Check the state of the switch and make the button checked or not to match.               
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch3\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
// Switch 2 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">CENTRE</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch2\" tabindex=\"0\"";
                                if(output2state == LOW){html+=" checked";};                
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch2\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
// Switch 1 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">RIGHT</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch1\" tabindex=\"0\"";
                                if(output1state == LOW){html+=" checked";};                
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch1\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
                "     </div>\n" // rower
                "     <div class='rower'>\n"
// Manual feed button html 
                "       <div class=\"flex-container\">\n"
                "         <div style=\"margin:auto;\">\n"
                "           <button id=\"manualFeed\">+FOOD</button>\n"
                "         </div>\n"
                "       </div>\n"
// Switch 4 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">VEGE</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch4\" tabindex=\"0\"";
                                if(output4state == LOW){html+=" checked";};
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch4\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
// Switch 7 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">CAM</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch7\" tabindex=\"0\"";
                                if(output7state == LOW){html+=" checked";};
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch7\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
                "     </div>\n" // rower
                "     <div class='rower'>\n"
// Switch 5 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">FEED</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch5\" tabindex=\"0\"";
                                if(output5state == LOW){html+=" checked";};                
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch5\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
// Switch 6 html
                "       <div class=\"flex-container\">\n"
                "         <div class=\"flex-child magenta\">\n"
                "         <span class=\"component-label\">WATER</span>\n"
                "         </div>\n"
                "         <div class=\"flex-child green\">\n"
                "         <div class=\"grid-child green\">\n"
                "           <div style=\"display: inline\">\n"
                "             <div class=\"onoffswitch\">\n"
                "               <input type=\"checkbox\" name=\"onoffswitch\" class=\"onoffswitch-checkbox\" id=\"switch6\" tabindex=\"0\"";
                                if(output6state == LOW){html+=" checked";};                
                                html +=       ">\n"
                "               <label class=\"onoffswitch-label\" for=\"switch6\">\n"
                "                 <span class=\"onoffswitch-inner\"></span>\n"
                "                 <span class=\"onoffswitch-switch\"></span>\n"
                "               </label>\n"
                "             </div>\n"
                "           </div>\n"
                "         </div>\n"
                "         </div>\n"
                "       </div>\n"
//
                "     </div>\n" //rower
                "       </div>\n" //hero
// Status text box html
                "       <div id=\"status\">";
                        html+= status(); // Status string is inserted within this DIV.
                        html+= "</div>\n"
                "   </div>\n" // container 
                "   <script>\n" // This portion is still part of the html string, just javascript.
// Switch 1 js. Each switch is similar. A onclick event is bound to the button/DIV, with the function to run below this.
                "     document.getElementById('switch1').onclick = function() {\n" // Bind the onclick function to the switch element.
                "       // access properties using this keyword\n"
                "       var output1state;\n" // Create a variable to store the state of the switch.
                "       if ( this.checked ) {\n" // Check if the switch is on or off.
                "         output1state = \"ON\";\n" // Set state to ON if checked.
                "       } else {\n"
                "         output1state = \"OFF\";\n" // Or set switch to OFF if not checked.
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n" // Create AJAX request.
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n" // Anything in here will run after the webpage hears back the request was successful from the server.
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle1?output1state=\"+output1state, true);\n" // Create the request to the server, including instructions that this request is for output1 and the new state - ON or OFF.
                "       xhttp.send();\n" // Send the request to the server.
                "     };\n"
// Switch 2 js
                "     document.getElementById('switch2').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output2state;\n"
                "       if ( this.checked ) {\n"
                "         output2state = \"ON\";\n"
                "       } else {\n"
                "         output2state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle2?output2state=\"+output2state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Switch 3 js
                "     document.getElementById('switch3').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output3state;\n"
                "       if ( this.checked ) {\n"
                "         output3state = \"ON\";\n"
                "       } else {\n"
                "         output3state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle3?output3state=\"+output3state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Switch 4 js
                "     document.getElementById('switch4').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output4state;\n"
                "       if ( this.checked ) {\n"
                "         output4state = \"ON\";\n"
                "       } else {\n"
                "         output4state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle4?output4state=\"+output4state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Switch 5 js
                "     document.getElementById('switch5').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output5state;\n"
                "       if ( this.checked ) {\n"
                "         output5state = \"ON\";\n"
                "       } else {\n"
                "         output5state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle5?output5state=\"+output5state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Switch 6 js
                "     document.getElementById('switch6').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output6state;\n"
                "       if ( this.checked ) {\n"
                "         output6state = \"ON\";\n"
                "       } else {\n"
                "         output6state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle6?output6state=\"+output6state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Switch 7 js
                "     document.getElementById('switch7').onclick = function() {\n"
                "       // access properties using this keyword\n"
                "       var output7state;\n"
                "       if ( this.checked ) {\n"
                "         output7state = \"ON\";\n"
                "       } else {\n"
                "         output7state = \"OFF\";\n"
                "       }\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"toggle7?output7state=\"+output7state, true);\n"
                "       xhttp.send();\n"
                "     };\n"
// Voltage update. This one is slightly different from the output switches. See comments below:
                "     document.getElementById('voltage').onclick = function() {\n"
                "       document.getElementById(\"voltage\").innerHTML='-';\n" // When the V DIV is clicked, the value will temporarily be changed to '-' while the webpage waits for the new value to come back.
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           document.getElementById(\"voltage\").innerHTML=this.responseText\n" // The voltage DIV contents are changed to the new value once it is sent back. Eg. '12.13 V'
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"updateVoltage\")\n"
                "       xhttp.send();\n"
                "     };\n"
// FOTC tune. Manually send a command to play the buzzer tune.
                "     document.getElementById('fotc').onclick = function() {\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"playFotc\")\n"
                "       xhttp.send();\n"
                "     };\n"
// Manual feed button
                "     document.getElementById('manualFeed').onclick = function() {\n"
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           \n"
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"manualFeed\")\n"
                "       xhttp.send();\n"
                "     };\n"
// Status box update. Similar to the voltage update.
                "     document.getElementById('status').onclick = function() {\n"
                "       document.getElementById(\"status\").innerHTML='<p>Offline</p>';\n" // Status box test is set to say 'Offline' until the updated status is returned. If it nevers hears back, the message stays as Offline because the system is down.
                "       var xhttp = new XMLHttpRequest();\n"
                "       xhttp.onreadystatechange = function() {\n"
                "         if (this.readyState == 4 && this.status == 200) {\n"
                "           document.getElementById(\"status\").innerHTML=this.responseText\n" // Status box DIV is updated wit the new status string once the server sends it back.
                "         }\n"
                "       };\n"
                "       xhttp.open(\"GET\", \"status\")\n"
                "       xhttp.send();\n"
                "     };\n"
                //
                "   </script>\n" // End of all of the javascript.
                " </body>\n"
                "</html>\n";
  return html; // Return the complete html string to be sewrved as the webpage.
}