
/*
  based on debounce, ultrasonic Sr04 lib and sweep examples form arduino.cc
https://github.com/Martinsos/arduino-lib-hc-sr04 for distance sensor
*/

// includes comunicación libs
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h> // check if you are using versión 6 
#include <Servo.h>
#include <HCSR04.h>

//***************MODIFICAR PARA SU PROYECTO *********************
//  configuración datos wifi 
// descomentar el define y poner los valores de su red y de su dispositivo
#define WIFI_AP "WIFNAME"
#define WIFI_PASSWORD "WIFIPASS"


//  configuración datos thingsboard
#define NODE_NAME "DEVICE"   //nombre que le pusieron al dispositivo cuando lo crearon
#define NODE_TOKEN "DEVICE TOKEN"   //Token que genera Thingboard para dispositivo cuando lo crearon


//***************NO MODIFICAR *********************
char thingsboardServer[] = "demo.thingsboard.io";

/*definir topicos.
 * telemetry - use to send telemetry
 * request -  use to receive and send requests to server
 * attributes - to receive commands from server to device
 */
char telemetryTopic[] = "v1/devices/me/telemetry";
char requestTopic[] = "v1/devices/me/rpc/request/+";  //RPC - El Servidor usa este topico para enviar rquests, cliente response
char responseTopic[] = "v1/devices/me/rpc/response/+"; // RPC responce
char attributesTopic[] = "v1/devices/me/attributes";  //El Servidor usa este topico para enviar atributos


// declarar cliente Wifi y PubSus
WiFiClient wifiClient;
PubSubClient client(wifiClient);

//Dispenser variables
// constants won't change. They're used here to set pin numbers:
const int dispenserServoPin = D2;
const int buttonPin = D1;    // the number of the pushbutton pin

bool dispenserState = false;         // the current state of the dispenser is closed

// Declare servo
Servo dispenserServo; 
int dispenserOpenPos = 180;
int dispenserClosePos = 0;

UltraSonicDistanceSensor distanceSensor(D4, D5);  // Initialize sensor that uses digital pins 13 and 12.


// Debounce variables (see  https://www.arduino.cc/en/Tutorial/Debounce)
int buttonState;             // the current reading from the input pin
int lastButtonState = LOW;   // the previous reading from the input pin
unsigned long lastDebounceTime = 0;  // the last time the output pin was toggled
unsigned long debounceDelay = 50;    // the debounce time; increase if the output flickers

//-------------------------------------------------------------

//-------------------------------------------------------------
// app logic state of userRead, userAuthenticated and bottleDetected
bool serverDispenseButtonState = false;
bool serverLockMode = false;
bool dispenserCommand = false;
unsigned int dispensingTime = 10000;

//-------------------------------------------------------------
// THB request timer variables
//-------------------------------------------------------------
unsigned long lastSend;
int elapsedTime = 1000; // elapsed time request vs reply
int requestNumber =1;  


// App Logic
void setup() { 
  Serial.begin(9600);

  // init wifi & pubsus callbacks
  Serial.println("init connection");
  
  connectToWiFi();
  delay(10);
  
  client.setServer(thingsboardServer, 1883);
  client.setCallback(on_message);
   
  lastSend = 0; // variable to ctrl delays
      
  //Dispenser init
  pinMode(buttonPin, INPUT);

    // servo setup
  dispenserServo.attach(dispenserServoPin);
}
 
void loop() 
{  
    if ( !client.connected() ) {
      reconnect();
      }

    if (serverLockMode == false) {
        unsigned int startDispensing = millis();
        // check physical button
        do {
            readDispenserButton();
            client.loop();
        } while(dispense() && (millis() - startDispensing < dispensingTime));
    
        // check server button
        startDispensing = millis();
        do {
            dispenserState = serverDispenseButtonState;
            client.loop();
        } while(dispense() && (millis() - startDispensing < dispensingTime));
        serverDispenseButtonState = false; // just in case it exit becuase of timer
        
        // send remaing space on container
        if ( millis() - lastSend > elapsedTime ) { // Update and send only after 1 second
          sendRemainingSpaceOnContainer();
          lastSend = millis();
        }
        
      } else {
        // in case locked mode change in between dispensing operations, lets close de dispenser
        dispenserState = false;
        dispense();
      }
    client.loop();
}




/*
 * Thingsboard methods
 */
void on_message(const char* topic, byte* payload, unsigned int length) 
{
    // print received message
  Serial.println("On message");
  char message[length + 1];
  strncpy ( message, (char*)payload, length);
  message[length] = '\0';
  
  Serial.print("Topic: ");
  Serial.println(topic);
  Serial.print("Message: ");
  Serial.println( message);
  
  String topicStr = topic;
  String topicHead = topicStr.substring(0,strlen("v1/devices/me/rpc/request"));
  // Check topic for attributes o request
  if (strcmp(topic, "v1/devices/me/attributes") ==0) { //es un cambio en atributos compartidos
    Serial.println("----> CAMBIO DE ATRIBUTOS");
    //processAttributeRequestCommand(message);
  } else if (strcmp(topicHead.c_str(), "v1/devices/me/rpc/request") == 0) {
    // request
    Serial.println("----> PROCESS REQUEST FROM SERVER");
    processRequestAndReply(message, topic);
  }


}


// Process request
void processRequestAndReply(char *message, const char* topic)
{
  // Decode JSON request with ArduinoJson 6 https://arduinojson.org/v6/doc/deserialization/
  // Notar que a modo de ejemplo este mensaje se arma utilizando la librería ArduinoJson en lugar de desarmar el string a "mano"

  
  const int capacity = JSON_OBJECT_SIZE(4);
  StaticJsonDocument<capacity> doc;
  DeserializationError err = deserializeJson(doc, message);
  
  if (err) {
    Serial.print(("deserializeJson() failed with code "));
    Serial.println(err.c_str());
    
    return;
  }
    
  String methodName = doc["method"];
  if (methodName.equals("setDispenseValue") &&  serverLockMode == false ) {
    bool response  = doc["params"];
    processDispenseFromServer(response);
  }  else if (methodName.equals("setLockValue")) {
    bool response  = doc["params"];
    processLockFromServer(response);
  }
  
}

void processLockFromServer(bool response)
{
    const int capacity = JSON_OBJECT_SIZE(4);
    StaticJsonDocument<capacity> doc;
   
    serverLockMode = response;
   
   //Update card

    doc["LockMessage"] = serverLockMode ? "Bloqueado"  :"Habilitado" ;

    
    String output = "";
    serializeJson(doc, output);
    
    char attributes[100];
    output.toCharArray( attributes, 100 );
 
    Serial.print("respuesta aributos: ");
    Serial.println(attributes);

    // se envia la repsuesta la cual se despliegan en las tarjetas creadas para el atrubito 
    client.publish(attributesTopic, attributes);
}

void processDispenseFromServer(bool response)
{
   const int capacity = JSON_OBJECT_SIZE(4);
   StaticJsonDocument<capacity> doc;
   
   serverDispenseButtonState = response;
    
   
    //Update card

    doc["dispensingState"] = serverDispenseButtonState ? "Dispensando"  :"Dispensado" ;

    
    String output = "";
    serializeJson(doc, output);
    
    char attributes[100];
    output.toCharArray( attributes, 100 );
 
    Serial.print("respuesta aributos: ");
    Serial.println(attributes);

    // se envia la repsuesta la cual se despliegan en las tarjetas creadas para el atrubito 
    client.publish(attributesTopic, attributes);
}



/* 
 *  Solution Sensor management functions
 *
 */


void sendRemainingSpaceOnContainer()
{
      
   
      const int capacity = JSON_OBJECT_SIZE(4);
      StaticJsonDocument<capacity> doc;

      double distance = distanceSensor.measureDistanceCm();
      
      doc["space"] = distance;
      
      String output = "";
      serializeJson(doc, output);
      
      Serial.print("json to send telemetry:");
      Serial.println(output);
  
      char attributes[100];
      output.toCharArray( attributes, 100 );
      
      if (client.publish(telemetryTopic, attributes ) == true) {
          Serial.println("publish ok telemetry");
        } else {
           Serial.println("publish ERROR");
        }     
    


}

bool dispense()
{
  static bool previousDispensingState = false;


  if (dispenserState != previousDispensingState) {
    if (dispenserState == true) {
      Serial.println("Abre");
      dispenserServo.write(dispenserOpenPos);
    } 
    else {
        Serial.println("Cierra");
        dispenserServo.write(dispenserClosePos);
      }
    previousDispensingState = dispenserState;
  }

  return dispenserState;
}

void readDispenserButton()
{
  // read the state of the switch into a local variable:
  int reading =  digitalRead(buttonPin);

  // check to see if you just pressed the button
  // (i.e. the input went from LOW to HIGH), and you've waited long enough
  // since the last press to ignore any noise:
  // If the switch changed, due to noise or pressing:
  if (reading != lastButtonState) {
    // reset the debouncing timer
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    // whatever the reading is at, it's been there for longer than the debounce
    // delay, so take it as the actual current state:

    // if the button state has changed:
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == HIGH) {
        dispenserState = !dispenserState;
      }
    }
  }

  // save the reading. Next time through the loop, it'll be the lastButtonState:
  lastButtonState = reading;
  

}



//***************NO MODIFICAR - Conexion con Wifi y ThingsBoard *********************
/*
 * THB connection and topics subscription
 */
void reconnect() {
  int statusWifi = WL_IDLE_STATUS;
  
  // Loop until we're reconnected
  while (!client.connected()) {
    statusWifi = WiFi.status();
    connectToWiFi();
    
    Serial.print("Connecting to ThingsBoard node ...");
    // Attempt to connect (clientId, username, password)
    if ( client.connect(NODE_NAME, NODE_TOKEN, NULL) ) {
      Serial.println( "[DONE]" );
      
      // Suscribir al Topico de request
      client.subscribe(requestTopic); 
      client.subscribe(attributesTopic); 
      client.subscribe(responseTopic);     
    } else {
      Serial.print( "[FAILED] [ rc = " );
      Serial.print( client.state() );
      Serial.println( " : retrying in 5 seconds]" );
      // Wait 5 seconds before retrying
      delay( 5000 );
    }
  }
}

 
/*
 * 
 *  wifi connection
 */
 
void connectToWiFi()
{
  Serial.println("Connecting to WiFi ...");
  // attempt to connect to WiFi network

  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to WiFi");
}
