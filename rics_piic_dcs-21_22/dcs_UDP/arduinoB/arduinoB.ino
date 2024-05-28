/////////////////////////////////////////////
#include <Arduino_FreeRTOS.h>
#include <queue.h>
#include <semphr.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

TaskHandle_t taskSystemHandle;
TaskHandle_t taskExecutionHandle;
TaskHandle_t taskStopHandle;
TaskHandle_t taskResumeHandle;

int movestate[4];

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 200, 5);
unsigned int localPort = 8888;      // local port to listen on
char packetBuffer[UDP_TX_PACKET_MAX_SIZE];  //buffer to hold incoming packet,
EthernetUDP Udp;

SemaphoreHandle_t semExe;
SemaphoreHandle_t semStop;
SemaphoreHandle_t semResume;
QueueHandle_t mbxExe;
QueueHandle_t mbxStop;
QueueHandle_t mbxResume;

void taskSystem(void *pvParameters);
void taskExecution(void *pvParameters);
void taskStop(void *pvParameters);
void taskResume(void *pvParameters);

int isActuatorUp(){
  return digitalRead(I1_4);
}

int isActuatorDown(){
  return digitalRead(I1_5);
}

int isPartAtActuator(){
  return !digitalRead(I1_3);
}

int isPartAtSensor(){
  return !digitalRead(I1_2);
}

void moveConveyorAct(){
  digitalWrite(Q0_0, HIGH);
  while(!isPartAtActuator());
  digitalWrite(Q0_0, LOW);
}

void moveConveyorSens(){
  digitalWrite(Q0_1, HIGH);
  while(!isPartAtSensor());
  digitalWrite(Q0_1, LOW);
  Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
  Udp.write("B_END_EXE");
  Udp.endPacket();
  xSemaphoreGive(semExe);
}

void transition_send(){
  digitalWrite(Q0_1, HIGH);
}

void transition_rcv(){
  digitalWrite(Q0_0, HIGH);
  while(!isPartAtSensor());
  Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
  Udp.write("B_RCV");
  Udp.endPacket();
  digitalWrite(Q0_0, LOW);
}

void conveyorStop(){
  digitalWrite(Q0_0, LOW);
  digitalWrite(Q0_1, LOW);
}

void actuatorStop(){
  digitalWrite(Q1_0, LOW);
  digitalWrite(Q1_1, LOW);
}

void partProcessing(){
  digitalWrite(Q1_1, HIGH);
  while(!isActuatorDown());
  digitalWrite(Q1_1, LOW);
  delay(5000);
  digitalWrite(Q1_0, HIGH);
  while(!isActuatorUp());
  digitalWrite(Q1_0, LOW);
}

void execution (){
  while(!isPartAtSensor());
  moveConveyorAct();
  partProcessing();
  moveConveyorSens();
}

//calibration needs to be implemented in java
void calibration(){
  if(!digitalRead(I1_4)){
    digitalWrite(Q1_0, HIGH);
    while(!isActuatorUp());
    digitalWrite(Q1_0, LOW);
  }
}

void setup() {

  pinMode(Q0_0,OUTPUT);
  pinMode(Q0_1,OUTPUT);
  pinMode(Q1_0,OUTPUT);
  pinMode(Q1_1,OUTPUT);
  
  mbxExe = xQueueCreate(1, sizeof(int));
  mbxStop = xQueueCreate(1, sizeof(int));
  mbxResume = xQueueCreate(1, sizeof(int));
  semExe = xSemaphoreCreateMutex();
  xSemaphoreGive(semExe);
  semStop = xSemaphoreCreateMutex();
  xSemaphoreGive(semStop);
  semResume = xSemaphoreCreateMutex();
  xSemaphoreTake(semResume, portMAX_DELAY);

  xTaskCreate(taskSystem, "taskSystem", 128, NULL, 1 /*Priority*/, &taskSystemHandle);
  xTaskCreate(taskExecution, "taskExecution", 128, NULL, 1 /*Priority*/, &taskExecutionHandle);
  xTaskCreate(taskStop, "taskStop", 128, NULL, 2 /*Priority*/, &taskStopHandle);
  xTaskCreate(taskResume, "taskResume", 128, NULL, 2 /*Priority*/, &taskResumeHandle);
  
  calibration();

  Serial.begin(9600);
  while (!Serial) vTaskDelay(1);
  
  Ethernet.begin(mac);
  Udp.begin(localPort);
}

void loop() {}

void taskSystem(void *pvParameters){
  (void) pvParameters;
  
  while(true){
    int packetSize = Udp.parsePacket();
    if (packetSize){
      Udp.read(packetBuffer, UDP_TX_PACKET_MAX_SIZE);  
      char cmd = packetBuffer[0];
      switch(cmd){
        case '1':
          xSemaphoreTake(semExe, portMAX_DELAY);
          xQueueSend(mbxExe,&cmd,portMAX_DELAY);
          break;
        case '2':
          conveyorStop();
          break;
        case '3':
          xSemaphoreTake(semStop, portMAX_DELAY);
          xQueueSend(mbxStop,&cmd,portMAX_DELAY);
          break;
        case '4':
          xSemaphoreTake(semResume, portMAX_DELAY);
          xQueueSend(mbxResume,&cmd,portMAX_DELAY);
          break; 
        case '5':
          transition_send();
          break;
        case '6':
          transition_rcv();
          break;
        case '7':
          Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
          if(isPartAtSensor())
            Udp.write("B_ACK_ON");
          else
            Udp.write("B_ACK_OFF");
          Udp.endPacket();
          break;
      }
    }
  }
}

void taskExecution(void *pvParameters){
  (void) pvParameters;
  char cmd;
  while(true){
    if(xQueueReceive(mbxExe, &cmd, portMAX_DELAY)){
        execution();
    }
  }
}

void taskStop(void *pvParameters){
  (void) pvParameters;
  char cmd;
  while(true){
    if(xQueueReceive(mbxStop, &cmd, portMAX_DELAY)){
      taskENTER_CRITICAL();
      movestate[0]=digitalRead(Q0_0);
      movestate[1]=digitalRead(Q0_1);
      movestate[2]=digitalRead(Q1_0);
      movestate[3]=digitalRead(Q1_1);
      taskEXIT_CRITICAL();
       
      actuatorStop();
      conveyorStop();
      vTaskSuspend(taskExecutionHandle);
      xSemaphoreGive(semResume);
    }
  }
}

void taskResume(void *pvParameters){
  (void) pvParameters;
  char cmd;
  while(true){
    if(xQueueReceive(mbxResume, &cmd, portMAX_DELAY)){
      taskENTER_CRITICAL();
      digitalWrite(Q0_0, movestate[0]);
      digitalWrite(Q0_1, movestate[1]);
      digitalWrite(Q1_0, movestate[2]);
      digitalWrite(Q1_1, movestate[3]);
      taskEXIT_CRITICAL();
      vTaskResume(taskExecutionHandle);
      xSemaphoreGive(semStop);
    }
  }
}
