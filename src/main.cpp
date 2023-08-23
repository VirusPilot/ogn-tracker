#include <stdint.h>
#include <string.h>
#include <math.h>

#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Arduino.h>

#include <Wire.h>             // I2C for pressure sensor and charge controller
#include <SPI.h>

#include "main.h"

#ifdef WITH_BT_SPP
#include "BluetoothSerial.h"
#endif

#ifdef WITH_XPOWERS
#include "XPowersLib.h"
#endif

#ifdef WITH_AXP
#include <axp20x.h>
#endif

#include "gps.h"
#include "rf.h"
#include "sens.h"
#include "proc.h"
#include "log.h"

#include "nmea.h"

#include "driver/gpio.h"      // ESP32 GPIO driver
#include "driver/uart.h"      // ESP32 UART driver

#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "esp_spiffs.h"

#include "nvs.h"
#include "nvs_flash.h"

// =======================================================================================================

uint64_t getUniqueMAC(void)                                  // 48-bit unique ID of the ESP32 chip
{ uint8_t MAC[6]; esp_efuse_mac_get_default(MAC);
  uint64_t ID=MAC[0];
  for(int Idx=1; Idx<6; Idx++)
  { ID<<=8; ID|=MAC[Idx]; }
  return ID; }

uint64_t getUniqueID(void) { return getUniqueMAC(); }         // get unique serial ID of the CPU/chip
uint32_t getUniqueAddress(void) { return getUniqueMAC()&0x00FFFFFF; } // get unique OGN address

HardItems Hardware = { 0 };

// =======================================================================================================
// VS to store parameters and other data

static int NVS_Init(void)
{ esp_err_t Err = nvs_flash_init();
  if (Err == ESP_ERR_NVS_NO_FREE_PAGES)
  { nvs_flash_erase();
    Err = nvs_flash_init(); }
  return Err; }

// =======================================================================================================

#ifdef WITH_SPIFFS
int SPIFFS_Register(const char *Path, const char *Label, size_t MaxOpenFiles)
{ esp_vfs_spiffs_conf_t FSconf =
  { base_path: Path,
    partition_label: Label,
    max_files: MaxOpenFiles,
    format_if_mount_failed: true };
  return esp_vfs_spiffs_register(&FSconf); }

int SPIFFS_Info(size_t &Total, size_t &Used, const char *Label)
{ return esp_spiffs_info(Label, &Total, &Used); }
#endif

// =======================================================================================================

uint8_t I2C_Restart(uint8_t Bus)
{ Wire.end(); Wire.begin(); return 0; }

uint8_t I2C_Read (uint8_t Bus, uint8_t Addr, uint8_t Reg, uint8_t *Data, uint8_t Len, uint8_t Wait)
{ Wire.beginTransmission(Addr);
  int Ret=Wire.write(Reg);
  Wire.endTransmission(false);
  Ret=Wire.requestFrom(Addr, Len);
  for(uint8_t Idx=0; Idx<Len; Idx++)
  { Data[Idx]=Wire.read(); }
  // Wire.endTransmission();
  return Ret!=Len; }
 
uint8_t I2C_Write(uint8_t Bus, uint8_t Addr, uint8_t Reg, uint8_t *Data, uint8_t Len, uint8_t Wait)
{ Wire.beginTransmission(Addr);
  int Ret=Wire.write(Reg);
  for(uint8_t Idx=0; Idx<Len; Idx++)
  { Ret=Wire.write(Data[Idx]); if(Ret!=1) break; }
  Wire.endTransmission();
  return Ret!=1; }

// =======================================================================================================

#ifdef WITH_XPOWERS
XPowersLibInterface *PMU = 0;
#endif

#ifdef WITH_AXP
static AXP20X_Class AXP;
#endif

// =======================================================================================================
// ADC to sense battery voltage

static esp_adc_cal_characteristics_t *ADC_characs =
        (esp_adc_cal_characteristics_t *)calloc(1, sizeof(esp_adc_cal_characteristics_t));
static adc1_channel_t ADC_Chan_Batt = ADC1_CHANNEL_7; // ADC channel #7 is GPIO35
static const adc_atten_t ADC_atten = ADC_ATTEN_DB_11;
static const adc_unit_t ADC_unit = ADC_UNIT_1;
#define ADC_Vref 1100

static int ADC_Init(void)
{ // if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_TP) == ESP_OK) // Check TP is burned into eFuse
  // if (esp_adc_cal_check_efuse(ESP_ADC_CAL_VAL_EFUSE_VREF) == ESP_OK) // Check Vref is burned into eFuse
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC_Chan_Batt, ADC_atten);
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_unit, ADC_atten, ADC_WIDTH_BIT_12, ADC_Vref, ADC_characs); // calibrate ADC1
  return 0; }

uint16_t BatterySense(int Samples)
{
#ifdef WITH_XPOWERS
  if(PMU) return PMU->getBattVoltage();
#endif
#ifdef WITH_AXP
  if(Hardware.AXP192||Hardware.AXP202) return AXP.getBattVoltage();
#endif
  uint32_t RawVoltage=0;
  for( int Idx=0; Idx<Samples; Idx++)
  { RawVoltage += adc1_get_raw(ADC_Chan_Batt); }
  RawVoltage = (RawVoltage+Samples/2)/Samples;
  uint16_t Volt = (uint16_t)esp_adc_cal_raw_to_voltage(RawVoltage, ADC_characs)*2;
  // const uint16_t Bias = 80;  // apparently, there is 80mV bias in the battery voltage measurement
  // if(Volt>=Bias) Volt-=Bias;
  return Volt; } // [mV]

// =======================================================================================================

FlashParameters Parameters;  // parameters stored in Flash: address, aircraft type, etc.

// =======================================================================================================
// CONSole UART

#ifdef WITH_BT_SPP
static BluetoothSerial BTserial;
#endif

void CONS_UART_Write(char Byte) // write byte to the console (USB serial port)
{ Serial.write(Byte);
#ifdef WITH_BT_SPP
  BTserial.write(Byte);
#endif
}

int  CONS_UART_Free(void)
{ return Serial.availableForWrite(); }

int  CONS_UART_Read (uint8_t &Byte)
{ int Ret=Serial.read(); if(Ret>=0) { Byte=Ret; return 1; }
#ifdef WITH_BT_SPP
  Ret=BTserial.read(); if(Ret>=0) { Byte=Ret; return 1; }
#endif
  return 0; }

// =======================================================================================================

// move to the specific pin-defnition file
// #define GPS_UART    UART_NUM_1      // UART for GPS
// const int GPS_PinTx  = GPIO_NUM_12;
// const int GPS_PinRx  = GPIO_NUM_34;
// const int GPS_PinPPS = GPIO_NUM_37;

int   GPS_UART_Full         (void)          { size_t Full=0; uart_get_buffered_data_len(GPS_UART, &Full); return Full; }
int   GPS_UART_Read         (uint8_t &Byte) { return uart_read_bytes  (GPS_UART, &Byte, 1, 0); }  // should be buffered and non-blocking
void  GPS_UART_Write        (char     Byte) {        uart_write_bytes (GPS_UART, &Byte, 1);    }  // should be buffered and blocking
void  GPS_UART_Flush        (int MaxWait  ) {        uart_wait_tx_done(GPS_UART, MaxWait);     }
void  GPS_UART_SetBaudrate  (int BaudRate ) {        uart_set_baudrate(GPS_UART, BaudRate);    }

#ifdef GPS_PinPPS
bool GPS_PPS_isOn(void) { return gpio_get_level((gpio_num_t)GPS_PinPPS); }
#endif
#ifdef GPS_PinEna
void GPS_DISABLE(void) { gpio_set_level((gpio_num_t)GPS_PinEna, 0); }
void GPS_ENABLE (void) { gpio_set_level((gpio_num_t)GPS_PinEna, 1); }
#endif

static void GPS_UART_Init(int BaudRate=9600)
{
#ifdef GPS_PinPPS
  gpio_set_direction((gpio_num_t)GPS_PinPPS, GPIO_MODE_INPUT);
#endif
#ifdef GPS_PinEna
  gpio_set_direction((gpio_num_t)GPS_PinEna, GPIO_MODE_OUTPUT);
  GPS_ENABLE();
#endif
  uart_config_t GPS_UART_Config =
  { baud_rate : BaudRate,
    data_bits : UART_DATA_8_BITS,
    parity    : UART_PARITY_DISABLE,
    stop_bits : UART_STOP_BITS_1,
    flow_ctrl : UART_HW_FLOWCTRL_DISABLE,
    rx_flow_ctrl_thresh: 0,
    use_ref_tick: 0
  };
  uart_param_config  (GPS_UART, &GPS_UART_Config);
  uart_set_pin       (GPS_UART, GPS_PinTx, GPS_PinRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  uart_driver_install(GPS_UART, 256, 256, 0, 0, 0);
  uart_set_rx_full_threshold(GPS_UART, 16); }

// =======================================================================================================

void LED_PCB_On   (void) { }                // LED on the PCB for visual indications
void LED_PCB_Off  (void) { }
void LED_PCB_Flash(uint8_t Time) { }

// =======================================================================================================

uint8_t PowerMode = 2;                    // 0=sleep/minimal power, 1=comprimize, 2=full power

SemaphoreHandle_t CONS_Mutex;                // Mut-Ex for the Console
SemaphoreHandle_t I2C_Mutex;                 // Mut-Ex for the I2C

static char Line[128];
static void PrintPOGNS(void);

void setup()
{
  CONS_Mutex = xSemaphoreCreateMutex();      // semaphore for sharing the writing to the console
  I2C_Mutex  = xSemaphoreCreateMutex();      // semaphore for sharing the I2C bus

  NVS_Init();                                // initialize storage in flash like for parameters
#ifdef WITH_SPIFFS
  SPIFFS_Register();                         // initialize the file system in the Flash
#endif

  Parameters.setDefault(getUniqueAddress()); // set default parameter values
  if(Parameters.ReadFromNVS()!=ESP_OK)       // try to get parameters from NVS
  { Parameters.WriteToNVS(); }               // if did not work: try to save (default) parameters to NVS
  if(Parameters.CONbaud<2400 || Parameters.CONbaud>921600 || Parameters.CONbaud%2400)
  { Parameters.CONbaud=115200; Parameters.WriteToNVS(); }

  Serial.begin(Parameters.CONbaud);          // USB Console: baud rate probably does not matter here
  GPS_UART_Init();

  Serial.println("OGN-Tracker");
  // Serial.printf("RFM: CS:%d IRQ:%d RST:%d\n", LORA_CS, LORA_IRQ, LORA_RST);

  Wire.begin(I2C_PinSDA, I2C_PinSCL, (uint32_t)400000); // (SDA, SCL, Frequency) I2C on the correct pins
  Wire.setTimeOut(20);                                  // [ms]

#ifdef WITH_AXP
  if(AXP.begin(Wire, AXP192_SLAVE_ADDRESS)!=AXP_FAIL)
  { Hardware.AXP192=1; Serial.println("AXP192 power/charge chip detected"); }
  else if(AXP.begin(Wire, AXP202_SLAVE_ADDRESS)!=AXP_FAIL)
  { Hardware.AXP202=1; Serial.println("AXP202 power/charge chip detected"); }
  else
  { Serial.println("AXP power/charge chip NOT detected"); }

  if(Hardware.AXP192 || Hardware.AXP202)
  { AXP.adc1Enable(AXP202_VBUS_VOL_ADC1 |
                   AXP202_VBUS_CUR_ADC1 |
                   AXP202_BATT_CUR_ADC1 |
                   AXP202_BATT_VOL_ADC1,
                   true);
    Serial.printf("  USB:  %5.3fV  %5.3fA\n",
              0.001f*AXP.getVbusVoltage(), 0.001f*AXP.getVbusCurrent());
    Serial.printf("  Batt: %5.3fV (%5.3f-%5.3f)A\n",
              0.001f*AXP.getBattVoltage(), 0.001f*AXP.getBattChargeCurrent(), 0.001f*AXP.getBattDischargeCurrent());
  }
#endif
#ifdef WITH_XPOWERS
  if(PMU==0)
  { PMU = new XPowersAXP2101(Wire);
    if(PMU->init())
    { Hardware.AXP210=1; Serial.println("AXP2101 power/charge chip detected"); }
    else
    { delete PMU; PMU=0; Serial.println("AXP2101 power/charge chip NOT detected"); }
  }
  if(PMU==0)
  { PMU = new XPowersAXP192(Wire);
    if(PMU->init())
    { Hardware.AXP192=1; Serial.println("AXP192 power/charge chip detected"); }
    else
    { delete PMU; PMU=0; Serial.println("AXP192 power/charge chip NOT detected"); }
  }
  if(Hardware.AXP192 || Hardware.AXP210)
  { PMU->enableSystemVoltageMeasure();
    PMU->enableVbusVoltageMeasure();
    PMU->enableBattVoltageMeasure();
    // It is necessary to disable the detection function of the TS pin on the board
    // without the battery temperature detection function, otherwise it will cause abnormal charging
    PMU->disableTSPinMeasure();
    Serial.printf("  USB:  %5.3fV\n", 0.001f*PMU->getVbusVoltage());
    Serial.printf("  Batt: %5.3fV\n", 0.001f*PMU->getBattVoltage());
    // Serial.printf("  USB:  %5.3fV  %5.3fA\n",
    //           0.001f*PMU->getVbusVoltage(), 0.001f*PMU->getVbusCurrent());
    // Serial.printf("  Batt: %5.3fV (%5.3f-%5.3f)A\n",
    //           0.001f*PMU->getBattVoltage(), 0.001f*PMU->getBattChargeCurrent(), 0.001f*PMU->getBattDischargeCurrent());
  }
#endif
  if(!Hardware.AXP192 && !Hardware.AXP202 && !Hardware.AXP210)
  { ADC_Init(); }

  int RadioStat=TRX.Init();
  if(RadioStat==0)
  { Hardware.Radio=1; Serial.println("RF chip detected"); }
  else
  { Serial.printf("RF chip not detected: %d\n", RadioStat); }

#ifdef WITH_OLED
  OLED.begin();
  // OLED.setDisplayRotation(U8G2_R2);
  OLED.clearBuffer();
  OLED_DrawLogo(OLED.getU8g2(), 0);
  OLED.sendBuffer();
#endif

#ifdef WITH_BT_SPP
  BTserial.begin(Parameters.BTname);
#endif

  uint8_t Len=Format_String(Line, "$POGNS,SysStart");
  Len+=NMEA_AppendCheckCRNL(Line, Len);
  Line[Len]=0;
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, Line);
  xSemaphoreGive(CONS_Mutex);
  PrintPOGNS();

  xTaskCreate(vTaskLOG    ,  "LOG"  ,  5000, NULL, 0, NULL);  // log data to flash
  xTaskCreate(vTaskGPS    ,  "GPS"  ,  3000, NULL, 1, NULL);  // read data from GPS
#if defined(WITH_BMP180) || defined(WITH_BMP280) || defined(WITH_BME280)
  xTaskCreate(vTaskSENS   ,  "SENS" ,  3000, NULL, 1, NULL);  // read data from pressure sensor
#endif
  xTaskCreate(vTaskPROC   ,  "PROC" ,  3000, NULL, 1, NULL);  // process received packets, prepare packets for transmission
  xTaskCreate(vTaskRF     ,  "RF"   ,  3000, NULL, 1, NULL);  // transmit/receive packets

}

// =======================================================================================================

void PrintTasks(void (*CONS_UART_Write)(char))
{ char Line[32];

  size_t FreeHeap = xPortGetFreeHeapSize();
  Format_String(CONS_UART_Write, "Task            Pr. Stack, ");
  Format_UnsDec(CONS_UART_Write, (uint32_t)FreeHeap, 4, 3);
  Format_String(CONS_UART_Write, "kB free\n");

  UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
  TaskStatus_t *pxTaskStatusArray = (TaskStatus_t *)pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );
  if(pxTaskStatusArray==0) return;
  uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, NULL );
  for(UBaseType_t T=0; T<uxArraySize; T++)
  { TaskStatus_t *Task = pxTaskStatusArray+T;
    uint8_t Len=Format_String(Line, Task->pcTaskName, configMAX_TASK_NAME_LEN, 0);
    // for( ; Len<=configMAX_TASK_NAME_LEN; )
    //   Line[Len++]=' ';
    Len+=Format_UnsDec(Line+Len, Task->uxCurrentPriority, 2); Line[Len++]=' ';
    // Line[Len++]='0'+Task->uxCurrentPriority; Line[Len++]=' ';
    Len+=Format_UnsDec(Line+Len, (uint32_t)(Task->usStackHighWaterMark), 3);
    Line[Len++]='\n'; Line[Len]=0;
    Format_String(CONS_UART_Write, Line);
  }
  vPortFree( pxTaskStatusArray );
}

static NMEA_RxMsg NMEA;
#ifdef WITH_GPS_UBX_PASS
static UBX_RxMsg  UBX;
#endif

static void PrintParameters(void)                              // print parameters stored in Flash
{ Parameters.Print(Line);
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);                   // ask exclusivity on UART1
  Format_String(CONS_UART_Write, Line);
  xSemaphoreGive(CONS_Mutex);                                  // give back UART1 to other tasks
}

static void PrintPOGNS(void)                                   // print parameters in the $POGNS form
{ xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Parameters.WritePOGNS(Line);
  Format_String(CONS_UART_Write, Line);
  Parameters.WritePOGNS_Pilot(Line);
  Format_String(CONS_UART_Write, Line);
  Parameters.WritePOGNS_Acft(Line);
  Format_String(CONS_UART_Write, Line);
  Parameters.WritePOGNS_Comp(Line);
  Format_String(CONS_UART_Write, Line);
#ifdef WITH_AP
  Parameters.WritePOGNS_AP(Line);
  Format_String(CONS_UART_Write, Line);
#endif
#ifdef WITH_STRATUX
  Parameters.WritePOGNS_Stratux(Line);
  Format_String(CONS_UART_Write, Line);
#endif
  xSemaphoreGive(CONS_Mutex);                                          //
  return; }

#ifdef WITH_CONFIG
static void ReadParameters(void)  // read parameters requested by the user in the NMEA sent.
{ if((!NMEA.hasCheck()) || NMEA.isChecked() )
  { PrintParameters();
    if(NMEA.Parms==0) { PrintPOGNS(); return; }                              // if no parameter given
    Parameters.ReadPOGNS(NMEA);
    PrintParameters();
    esp_err_t Err = Parameters.WriteToNVS();                                                  // erase and write the parameters into the Flash
  }
}
#endif

#ifdef WITH_LOG
static void ListLogFile(void)
{ if(NMEA.Parms!=1) return;
#ifdef DEBUG_PRINT
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "ListLogFile() ");
  Format_String(CONS_UART_Write, (const char *)NMEA.ParmPtr(0), 0, 12);
  Format_String(CONS_UART_Write, " ");
  Format_UnsDec(CONS_UART_Write, (uint32_t)NMEA.ParmLen(0));
  Format_String(CONS_UART_Write, "\n");
  xSemaphoreGive(CONS_Mutex);
#endif
  uint32_t FileTime = FlashLog_ReadShortFileTime((const char *)NMEA.ParmPtr(0), NMEA.ParmLen(0));
  if(FileTime==0) return;
#ifdef DEBUG_PRINT
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "ListLogFile() ");
  Format_Hex(CONS_UART_Write, FileTime);
  Format_String(CONS_UART_Write, "\n");
  xSemaphoreGive(CONS_Mutex);
#endif
  FlashLog_ListFile(FileTime);
}
#endif

static void ProcessNMEA(void)     // process a valid NMEA that got to the console
{
#ifdef WITH_CONFIG
  if(NMEA.isPOGNS()) ReadParameters();
#endif
}

static void ProcessCtrlF(void)                                  // list log files to the console
{ xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
#ifdef WITH_SPIFFS
  char FullName[32];
  strcpy(FullName, "/spiffs/");
  struct stat Stat;
  uint32_t Files=0;                                          // count/list files in SPIFFS
  DIR *Dir=opendir(FullName);                                // open SPIFFS top directory
  if(Dir)
  { for( ; ; )                                               // loop over files
    { struct dirent *Ent = readdir(Dir); if(!Ent) break;     // get the next file of the directory
      if(Ent->d_type != DT_REG) continue;                    // if not a regular file (directory, link, ...): then skip
      char *Name = Ent->d_name;
      strcpy(FullName+8, Name);
      if(stat(FullName, &Stat)<0) continue;
      Format_String(CONS_UART_Write, FullName);
      CONS_UART_Write(' ');
      Format_UnsDec(CONS_UART_Write, (uint32_t)Stat.st_size);
      // if(Stat.st_size==0) { unlink(FullName); }           // remove files with zero length
      Format_String(CONS_UART_Write, "\n");
      Files++; }                                             // count the (regular) files
    closedir(Dir); }
  Format_String(CONS_UART_Write, "SPIFFS: ");
  size_t Total, Used;
  if(SPIFFS_Info(Total, Used)==0)                            // get the SPIFFS usage summary
  { Format_UnsDec(CONS_UART_Write, Used/1024);
    Format_String(CONS_UART_Write, "kB used, ");
    Format_UnsDec(CONS_UART_Write, Total/1024);
    Format_String(CONS_UART_Write, "kB total, "); }
  Format_UnsDec(CONS_UART_Write, Files);
  Format_String(CONS_UART_Write, " files\n");
#endif
  xSemaphoreGive(CONS_Mutex); }

static void ProcessCtrlC(void)                                  // print system state to the console
{ xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Parameters.Print(Line);
  Format_String(CONS_UART_Write, Line);
  Format_String(CONS_UART_Write, "GPS: ");
  Format_UnsDec(CONS_UART_Write, GPS_getBaudRate(), 1);
  Format_String(CONS_UART_Write, "bps");
  CONS_UART_Write(',');
  Format_UnsDec(CONS_UART_Write, GPS_PosPeriod, 4, 3);
  CONS_UART_Write('s');
  if(GPS_Status.PPS)         Format_String(CONS_UART_Write, ",PPS");
  if(GPS_Status.NMEA)        Format_String(CONS_UART_Write, ",NMEA");
  if(GPS_Status.UBX)         Format_String(CONS_UART_Write, ",UBX");
  if(GPS_Status.MAV)         Format_String(CONS_UART_Write, ",MAV");
  if(GPS_Status.BaudConfig)  Format_String(CONS_UART_Write, ",BaudOK");
  if(GPS_Status.ModeConfig)  Format_String(CONS_UART_Write, ",ModeOK");
  CONS_UART_Write('\r'); CONS_UART_Write('\n');
  // PrintTasks(CONS_UART_Write);                               // print the FreeRTOS tasks

  Parameters.Write(CONS_UART_Write);                         // write the parameters to the console

  Format_String(CONS_UART_Write, "Batt:");
  Format_UnsDec(CONS_UART_Write, (10*BatteryVoltage+128)>>8, 5, 4);
  Format_String(CONS_UART_Write, "V ");
  Format_SignDec(CONS_UART_Write, (600*BatteryVoltageRate+128)>>8, 3, 1);
  Format_String(CONS_UART_Write, "mV/min\n");

  xSemaphoreGive(CONS_Mutex); }

static void ProcessCtrlL(void)                                    // print system state to the console
{
#ifdef WITH_SPIFFS
  FlashLog_ListFiles();
#endif
}

static int ProcessInput(void)
{ int Count=0;
  for( ; ; )
  { uint8_t Byte; int Err=CONS_UART_Read(Byte); if(Err<=0) break; // get byte from console, if none: exit the loop
    Count++;
#ifndef WITH_GPS_UBX_PASS
    if(Byte==0x03) ProcessCtrlC();                                // if Ctrl-C received: print parameters
    if(Byte==0x06) ProcessCtrlF();                                // if Ctrl-F received: list files
    if(Byte==0x0C) ProcessCtrlL();                                // if Ctrl-L received: list log files
    if(Byte==0x18)                                                // Ctrl-X
    {
#ifdef WITH_SPIFFS
      FlashLog_SaveReq=1;
#endif
      vTaskDelay(1000);
      esp_restart(); }                                            // if Ctrl-X received then restart
#endif
    NMEA.ProcessByte(Byte);                                       // pass the byte through the NMEA processor
    if(NMEA.isComplete())                                         // if complete NMEA:
    {
#ifdef WITH_GPS_NMEA_PASS
      if(NMEA.isChecked())
        NMEA.Send(GPS_UART_Write);
#endif
      ProcessNMEA();                                              // interpret the NMEA
      NMEA.Clear(); }                                             // clear the NMEA processor for the next sentence
#ifdef WITH_GPS_UBX_PASS
    UBX.ProcessByte(Byte);
    if(UBX.isComplete())
    { UBX.Send(GPS_UART_Write);                                   // is there a need for a Mutex on the GPS UART ?
      UBX.Clear(); }
#endif
  }
  return Count; }

void loop()
{ if(ProcessInput()==0) vTaskDelay(1);

}

// =======================================================================================================
