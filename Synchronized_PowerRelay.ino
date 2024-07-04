/**
 * Synchronized Power Relay
 * An Arduino microcontroller code for activating a relay based on detected power current
 * Used for synchronizing the power of a UV Curing station with secondary UV LED's
 * 
 * Copyright (c) 2024 Shinobu (Shinobu.bu@gmail.com)
 * 
 * Version 1.0
 * 
 * 
 */

#include <stdarg.h>
#include <stdlib.h>
#include <EEPROM.h> // Include EEPROM library in your Arduino IDE (Sketch-> Include Library)
#include <BfButton.h> // Include ButtonFever library in your Arduino IDE (Sketch-> Include Library)
#include "SerialCommand.h"

#define HAL_PIN A0
#define ADJUSTMENT_PIN_SW 2
#define ADJUSTMENT_PIN_DT 3
#define ADJUSTMENT_PIN_CLK 4
BfButton btn(BfButton::STANDALONE_DIGITAL, ADJUSTMENT_PIN_SW , true, LOW);



#define RELAY_PIN 5

// For writing into the EEPROM
#define TYPE_INT 4
#define TYPE_LONG 8
#define TYPE_FLOAT 4
#define TYPE_BOOL 1

#define MEM_activation 0   
#define MEM_relayoffset 8

#define MAX_A 5 // If using 30A version this is the max value change to 20 for 20A or 5 for 5B

#define AVG 10 // average samples of amp reading

SerialCommand sCmd;
// These code snippets was found posted on an Amazon review page lol Thank Richard M H
int threshold = 0;
int relayoffset = 0; // activating the relay causes voltage drop account for this in the readings
int runtime_offset = 0;
int relay_on = false;
int button = 0;
int oldthreshold = 0;
int halvalue = 0;

int calibration_duration = 3000; // 3 seconds
long calibration_data_min = 0;// lowest signal peak
long calibration_data_max = 0;// highest signal peak
unsigned long calibration_time = 0;
bool relayCalibrated = false;
int relay_calibration_time = 0;
long relay_calibration_min = 0;
long relay_calibration_max = 0;


bool streamGraph = false;
bool debug = false;
float in, out;

void(* resetFunc) (void) = 0; //declare reset function @ address 0
int margin = 0;


void cmdHelp(const char *command)
{
  Serial.println("Commands:");
  Serial.println("graph()");
  Serial.println("debug()");
}

void cmdDebug(const char *command)
{
  if (debug)
  {
    debug = false;
  } else 
  {
    debug = true;
  }
}


void cmdStreamGraph(const char *command)
{     
  if (streamGraph)
  {
    streamGraph = false;
  } else
  {        
    streamGraph = true;  
    Serial.println("");         
    Serial.print("Current"); 
    Serial.print(","); 
    Serial.println("Activation");   
  }
}

// Load settings from EEPROM
void cmdLoadFromEEPROM(const char *command)
{
  long memActivation;
  long memRelayOffset;
  EEPROM.get(MEM_activation, memActivation);   
  if ( !isUnusedEEPROM( MEM_activation , TYPE_LONG ) )
  {        
    threshold = memActivation;    
  } 
  EEPROM.get(MEM_relayoffset, memRelayOffset);
  if ( !isUnusedEEPROM( MEM_relayoffset, TYPE_LONG ) )
  {
    relayoffset = memRelayOffset;
  }
}

// Save Settings to EEPROM
void cmdSaveToEEPROM(const char *command)
{
  // Save everything to EEPROM , but only update if different  
  long memActivation;     
  long memRelayOffset;
  EEPROM.get(MEM_activation, memActivation);  
  if (memActivation != threshold)
  {
    EEPROM.put(MEM_activation,threshold);
  }
  EEPROM.get(MEM_relayoffset, memRelayOffset);
  if (memRelayOffset != relayoffset)
  {
    EEPROM.put(MEM_relayoffset , relayoffset);
  }
}

// Helper function for erasing EEPROM values from address
void cmdEraseEEPROM(int address, int bytes)
{
  int i = 0;
  for (i = 0; i < bytes; i++)
  {    
    EEPROM.update(address+i,255);
  }
}

void clearEEPROM()
{
  cmdEraseEEPROM( MEM_activation ,TYPE_LONG);          
}

bool isUnusedEEPROM(int address, int bytes)
{
  int i = 0;  
  for( i = 0 ; i < bytes; i++)
  {
    byte bitt = EEPROM.read(address+i);
    if (bitt != 0xFF)
    {
      return false;
    }
  }
  return true;
}

void setup() {
  // setup decoder threshold
  pinMode(ADJUSTMENT_PIN_SW,INPUT);
  pinMode(ADJUSTMENT_PIN_CLK,INPUT);
  pinMode(ADJUSTMENT_PIN_DT,INPUT);
  
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(HAL_PIN, INPUT);
  Serial.begin(9600);
  cmdLoadFromEEPROM(NULL);
  //function buttonInt will run on a falling edge on pin ADJUSTMENT_PIN_SW
  //attachInterrupt(0,buttonInt,FALLING);  
  //attachInterrupt(0,buttonReleaseInt,RISING);
  btn.onPress(pressHandler)
  .onDoublePress(pressHandler)
  .onPressFor(pressHandler, 1000); // 1 second timeout for hold
  
  // function thresholdInt will run on a falling edge on pin ADJUSTMENT_PIN_CLK  
  attachInterrupt(1,thresholdInt,FALLING);
  
  sCmd.addCommand("graph", cmdStreamGraph);
  sCmd.addCommand("debug", cmdDebug);  
  sCmd.addCommand("help",cmdHelp);
  

  if (debug)
  {
    Serial.print(" Current Saved threshold value ");
    Serial.println(threshold);
  }
}

void pressHandler( BfButton *btn, BfButton::press_pattern_t pattern)
{  
  switch (pattern) {
    case BfButton::SINGLE_PRESS:
      if (debug) {
        Serial.println("Saving to EEPROM");        
      }
      cmdSaveToEEPROM(NULL);
       
      break;
     case BfButton::DOUBLE_PRESS:
      if (debug) {
        Serial.println("Calibrating to current power value");              
      }      
      //delay(1000); // let things rest before starting;
      calibration_time = millis();
      relayCalibrated = false;
      relay_calibration_time = millis();
      calibration_data_min = 0; // reset to 0
      calibration_data_max = 0; // reset to 0     
      relay_calibration_min = 0;
      relay_calibration_max = 0; 
      runtime_offset = 0; // prevent contamination of calibration procedures
      // find lowest peak of power usage
      break;
     case BfButton::LONG_PRESS:
      if (debug) {
        Serial.println("Resetting to Factory settings...");
      }
      clearEEPROM();        
      resetFunc(); 
      break;
  }
}

void thresholdInt(){
  //we got here so we know the threshold changed, pin 4 will tell us which direction.
  int dtv = digitalRead(ADJUSTMENT_PIN_DT);
  int clkv = digitalRead(ADJUSTMENT_PIN_CLK);
  if(clkv){
    threshold++;
    if (threshold > 512)
    {
      threshold = 512;
    }
  }
  else
  {
    threshold--;
    if (threshold < 0)
    {
      threshold = 0;
    }
  }  
  delay(200); // a cheap trick to avoid glitchy knobs
}

void blink()
{
  for (int i=0; i < 4; i++)
  {
    digitalWrite(RELAY_PIN, HIGH);
    delay(100);
    digitalWrite(RELAY_PIN, LOW);
    delay(100);
  }
}

void loop(){
  if (Serial.available() > 0)
  {
    sCmd.readSerial();
  }
  int avg = 0;
  for (int i=0; i < AVG ; i++)
  {
    avg += analogRead(HAL_PIN);
  }  
  halvalue = abs(((avg / AVG) - 512) - runtime_offset);  
    
  btn.read(); // Read button states and execute any events regarding its press states
  if (calibration_time > 0)
  {    
    if (relayCalibrated == false)
    {
      int remaining = (relay_calibration_time + calibration_duration) - millis();
      digitalWrite(RELAY_PIN, HIGH);
      // find relay voltage offset first.
      
      if (relay_calibration_min > halvalue || relay_calibration_min == 0)
      {
        relay_calibration_min = halvalue;
      }
      if (relay_calibration_max < halvalue || relay_calibration_max == 0)
      {
        relay_calibration_max = halvalue;
      }
      if ( remaining <= 0)
      {
        relayoffset = relay_calibration_min;
        relayCalibrated = true;
        calibration_time = millis(); // set for next calibration round
        digitalWrite(RELAY_PIN, LOW);
        if(debug)
        {
          Serial.print("Finished Relay voltage drop test Value found:");
          Serial.println(relayoffset);
        }
        delay(1000);
      } 
    } else {
      int remaining = (calibration_time + calibration_duration)  - millis();
      digitalWrite(RELAY_PIN, LOW);
      // Callibration has been initiated
      if (calibration_data_min > halvalue || calibration_data_min == 0)
      {
        calibration_data_min = halvalue;
      }
      if (calibration_data_max < halvalue || calibration_data_max == 0)
      {
        calibration_data_max = halvalue;
      }
      
      if (remaining <= 0 )
      {
        int fluctation_band = calibration_data_max - calibration_data_min;
        // callibration completed
        threshold = calibration_data_min;// - fluctation_band;
        relayoffset = relayoffset - calibration_data_min; // offset is now relative to calibration data.
        relay_calibration_min = relay_calibration_min - calibration_data_min;
        relay_calibration_max = relay_calibration_max - calibration_data_min;
        if (debug)
        {        
          Serial.print("Callibration completed, target value: ");
          Serial.print( threshold );       
          Serial.print(" min: ");
          Serial.print(calibration_data_min);
          Serial.print(" max: ");
          Serial.print(calibration_data_max);
          Serial.print(" fluctation_band: ");
          Serial.print(fluctation_band);
          Serial.print(" relayoffset: ");
          Serial.print(relayoffset);
          Serial.print(" relay_min: ");
          Serial.print(relay_calibration_min);
          Serial.print(" relay_max: ");
          Serial.println(relay_calibration_max);                     
        }      
        calibration_data_min = 0;
        calibration_time = 0;    
        blink();
      }  
    }
  }
  //check if the value of threshold has changed
  if(threshold != oldthreshold)
  {
    //sync the variables so we can detect another change later
    oldthreshold = threshold;    
    if (debug)
    {            
      Serial.print("threshold value: ");      
      Serial.println(threshold);   
        
    } 
  }


  if (streamGraph)
  {
    Serial.print( halvalue );
    Serial.print(",");
    Serial.println( threshold );
  }
  
  if (calibration_time == 0)
  {
    
    if (halvalue >= threshold)
    {
      digitalWrite(RELAY_PIN, HIGH); // activate ( Normally Off )      
      runtime_offset = relayoffset;
      digitalWrite(LED_BUILTIN,HIGH);
    } else {
      digitalWrite(RELAY_PIN, LOW); // deactivate            
      runtime_offset = 0;
      digitalWrite(LED_BUILTIN,LOW);
    }
  
    //add a 10ms delay so the chip is not too busy and it still responds to the programmer
    delay(10);
  } else {
      //digitalWrite(RELAY_PIN, LOW); // deactivate      
      digitalWrite(LED_BUILTIN,LOW);          
      delay(10);
  }
  
}
