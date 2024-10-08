#include <CrsfSerial.h>  // https://github.com/CapnBry/CRServoF/
#include "board_defs.h"
#include "hardware/pwm.h"


// BOARD_ID SELECTION HAPPENS IN PLATFORMIO ENVIRONMENT SELECTION

// Blink routine variables and state tracking
#define BLINK_ENABLED                    // comment this line out to disable led blink
#define BLINK_TIME 60000                 // blink routine window (in ms)
#define BLINK_DELAY 500                  // delay in between led state change (in ms)

static bool serialEcho;
static char serialInBuff[64];
static uint8_t serialInBuffLen;

bool led_state = false;                  // track led on/off state
unsigned long ms_curr = 0;               // current time
unsigned long ms_last_link_changed = 0;  // last time crsf link changed
unsigned long ms_last_led_changed = 0;   // last time led changed state in blink routine

CrsfSerial crsf(UART_SELECT, CRSF_BAUDRATE); // pass any HardwareSerial port
uint16_t Servo_Value;
int16_t Duty_Cycle_Value;
uint8_t i;  // for loop incrementor
uint slice_num[Number_of_Channel_Outputs]; 

// Debug code which is used for sending channel data to USB serial monitor
#define DEBUG 0    // 0 turns off serial debug, 1 turns on serial debug
#define Number_of_Debug_Channels 14
//uint16_t channel_data[Number_of_Debug_Channels];
#if DEBUG == 1
#define debug(x) Serial.print(x)
#define debugln(x) Serial.println(x)
#define debug_delay(x) delay(x)
#else
#define debug(x)
#define debugln(x)
#define debug_delay(x)
#endif


// Failsafe declaration and function
void failsafe_output();

void failsafe_output() 
{
  for (i=0; i<Number_of_Channel_Outputs; ++i){
    if (Channel_Config_Setting[i] == Channel_Set_To_Servo | Channel_Config_Setting[i] == Channel_Set_To_DutyCycle)
    {
      pwm_set_gpio_level(Channel_GPIO_Mapping[i], Failsafe_Channel_Value[i]); // update PWM output value with failsafe value
    }
  }
}


void packetChannels()
{
  // Pin locations can be set up in board_defs.h

  // channel_data is used for debug only
  if (DEBUG == 1){    
    for(i=0; i<Number_of_Debug_Channels; ++i){
      debug("Channel");
      debug(i+1);
      debug(" ");
      debug(crsf.getChannel(i+1));
      debugln();
    }
  }


  for (i=0; i<Number_of_Channel_Outputs; ++i){
    if (Channel_Config_Setting[i] == Channel_Set_To_Servo){
      Servo_Value = crsf.getChannel(i+1);
      if(Servo_Value < Servo_Min_us){
        Servo_Value = Servo_Min_us;
      }
      if(Servo_Value > Servo_Max_us){
        Servo_Value = Servo_Max_us;
      }
      pwm_set_gpio_level(Channel_GPIO_Mapping[i], Servo_Value); // update PWM output value
    }
    if (Channel_Config_Setting[i] == Channel_Set_To_DutyCycle){ 
      Duty_Cycle_Value = ((crsf.getChannel(i+1)-1000)/10);
      if (Duty_Cycle_Value > 90){
        Duty_Cycle_Value = 100;
        }
      if (Duty_Cycle_Value < 10){
        Duty_Cycle_Value = 0;
        }
      if (Duty_Cycle_Invert == 1){
        Duty_Cycle_Value = (Duty_Cycle_Value-100)*-1;
        }
      Duty_Cycle_Value = Duty_Cycle_Value * 100; // change from duty cycle percent to microseconds
      pwm_set_gpio_level(Channel_GPIO_Mapping[i], Duty_Cycle_Value); // update PWM output value
      debug("Duty Cycle Channel ");
      debug(i+1);
      debug(" ");
      debug(Duty_Cycle_Value);
      debugln();
    }
  }   
}


void crsfLinkUp() 
{
  ms_last_link_changed = millis();
  ms_last_led_changed = ms_last_link_changed;
  led_state = true;
  led_on();
}


void crsfLinkDown() 
{
  ms_last_link_changed = millis();
  ms_last_led_changed = ms_last_link_changed;
  led_state = false;
  led_off();
  failsafe_output();
}


static void passthroughBegin(uint32_t baud)
{
  if (baud != crsf.getBaud()){
    // Force a reboot command since we want to send the reboot
    // at 420000 then switch to what the user wanted
    const uint8_t rebootpayload[] = { 'b', 'l' };
    crsf.queuePacket(CRSF_ADDRESS_CRSF_RECEIVER, CRSF_FRAMETYPE_COMMAND, &rebootpayload, sizeof(rebootpayload));
  }
  crsf.setPassthroughMode(true, baud);
  
  serialEcho = false;
}


static void crsfOobData(uint8_t b){
  Serial.write(b);
}

/***
 * @brief Processes a text command like we're some sort of CLI or something
 * @return true if CrsfSerial was placed into passthrough mode, false otherwise
*/


static bool handleSerialCommand(char *cmd){
  // Fake a CRSF RX on UART6
  bool prompt = true;
  if (strcmp(cmd, "#") == 0){
    debugln("Fake CLI Mode, type 'exit' or 'help' to do nothing\r\n");
    serialEcho = true;
  }

  else if (strcmp(cmd, "serial") == 0)
      debugln("serial 5 64 0 0 0 0\r\n");

  else if (strcmp(cmd, "get serialrx_provider") == 0)
      debugln("serialrx_provider = CRSF\r\n");

  else if (strcmp(cmd, "get serialrx_inverted") == 0)
      debugln("serialrx_inverted = OFF\r\n");

  else if (strcmp(cmd, "get serialrx_halfduplex") == 0)
      debugln("serialrx_halfduplex = OFF\r\n");

  else if (strncmp(cmd, "serialpassthrough 5 ", 20) == 0){
    debugln(cmd);

    unsigned int baud = atoi(&cmd[20]);
    passthroughBegin(baud);

    return true;
  }

  else
      prompt = false;

  if (prompt)
      debug("# ");

  return false;
}


static void checkSerialInNormal(){
  while (Serial.available()){
    char c = Serial.read();
    if (serialEcho && c != '\n')
        Serial.write(c);

    if (c == '\r' || c == '\n'){
        if (serialInBuffLen != 0){
          Serial.write('\n');
          serialInBuff[serialInBuffLen] = '\0';
          serialInBuffLen = 0;

          bool goToPassthrough = handleSerialCommand(serialInBuff);
          // If need to go to passthrough, get outta here before we dequeue any bytes
          if (goToPassthrough)
              return;
        }
      }
      else{
        serialInBuff[serialInBuffLen++] = c;
        // if the buffer fills without getting a newline, just reset
        if (serialInBuffLen >= sizeof(serialInBuff))
            serialInBuffLen = 0;
      }
  }  /* while Serial */
}


static void checkSerialInPassthrough(){
  static uint32_t lastData = 0;
  static bool LED = false;
  bool gotData = false;
  unsigned int avail;
  while ((avail = Serial.available()) != 0){
    uint8_t buf[16];
    avail = Serial.readBytes((char *)buf, min(sizeof(buf), avail));
    crsf.write(buf, avail);
    LED ? led_on() : led_off();
    LED = !LED;
    gotData = true;
  }
  // If longer than X seconds since last data, switch out of passthrough
  if (gotData || !lastData)
      lastData = millis();

  // Turn off LED 1s after last data
  else if (LED && (millis() - lastData > 1000)){
    LED = false;
    led_off();
  }

  // Short blink LED after timeout
  else if (millis() - lastData > 5000){
    lastData = 0;
    led_on();
    delay(200);
    led_off();
    crsf.setPassthroughMode(false);
  }
}


static void checkSerialIn()
{
  if (crsf.getPassthroughMode())
      checkSerialInPassthrough();
  else
      checkSerialInNormal();
}


#ifdef BLINK_ENABLED
void led_loop() {
  ms_curr = millis();
  // link is down
  if(!crsf.isLinkUp()) {
    // within the blink routine window (BLINK_TIME)
    if(ms_curr < (ms_last_link_changed + BLINK_TIME)) {
      // handle led toggle delay
      if(ms_curr > (ms_last_led_changed + BLINK_DELAY)) {
        ms_last_led_changed = ms_curr;
        led_state ? led_on() : led_off();
        led_state = !led_state;  // toggle led state
      }
    }
    else{
      // ensure the led is off if the blink routine expired and link is down
      led_off();
    }
  }
}
#endif


void setup(){
  Serial.begin(115200);
  UART_SELECT.setTX(CRSF_TX);
  UART_SELECT.setRX(CRSF_RX);
  boardSetup();
  crsfLinkDown();

  // Attach the channels callback
  crsf.onPacketChannels = &packetChannels;
  crsf.onLinkUp = &crsfLinkUp;
  crsf.onLinkDown = &crsfLinkDown;
  crsf.onOobData = &crsfOobData;
  crsf.begin();
  serialEcho = true;


  // initialize 'config' with the default PMW config
  pwm_config config = pwm_get_default_config();

  for(i=0; i<Number_of_Channel_Outputs; ++i){
      // Set GPIO pins to be allocated to the PWM by using the Channel_GPIO_Mapping array
      gpio_set_function(Channel_GPIO_Mapping[i], GPIO_FUNC_PWM);
      
      // Find out which PWM slice is connected to GPIO and store the slice number in an array that is aligned with Channel_GPIO_Mapping array
      slice_num[i] = {pwm_gpio_to_slice_num(Channel_GPIO_Mapping[i])};

      // Modify PWM config timing to 50hz
      if(Channel_Config_Setting[i] == Channel_Set_To_Servo){
        pwm_set_clkdiv(slice_num[i], CPU_Clock_Divider);  // set PWM clock to 1/133 of CPU clock speed for RP2040 or 1/150 for RP2350,  (133,000,000 / 133 = 1,000,000hz) for RP2040 or (150,000,000 / 150 = 1,000,000hz) for RP2350
        pwm_set_wrap(slice_num[i], 20000);    // set PWM period to 20,000 cycles of PWM clock,  (1,000,000 / 20,000 = 50hz)
      }
      // Modify PWM config timing to 100hz
      if(Channel_Config_Setting[i] == Channel_Set_To_DutyCycle){
        pwm_set_clkdiv(slice_num[i], CPU_Clock_Divider);  // set PWM clock to 1/133 of CPU clock speed for RP2040 or 1/150 for RP2350,  (133,000,000 / 133 = 1,000,000hz) for RP2040 or (150,000,000 / 150 = 1,000,000hz) for RP2350
        pwm_set_wrap(slice_num[i], 10000);    // set PWM period to 10,000 cycles of PWM clock,  (1,000,000 / 10,000 = 100hz)
      }

      // initialize the slice
      if(i % 2 == 0){
          pwm_init(slice_num[i], &config, true);
      }
      // Set initial PWM of channel
      //pwm_set_chan_level(slice_num[i], Channel_GPIO_Mapping[i], Failsafe_Channel_Value[i]);
      pwm_set_gpio_level(Channel_GPIO_Mapping[i],Failsafe_Channel_Value[i]);

      // Set the PWM running
      if(i % 2 == 0){
          pwm_set_enabled(slice_num[i], true);
      }
    }
}


void loop()
{
  // Must call CrsfSerial.loop() in loop() to process data
  crsf.loop();
  checkSerialIn();
  #ifdef BLINK_ENABLED
  led_loop();
  #endif
}
