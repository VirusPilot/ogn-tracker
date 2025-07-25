#include <stdint.h>

#include "hal.h"                      // Hardware Abstraction Layer

#include "proc.h"                     // PROC task: decode/correct received packets
#include "ctrl.h"                     // CTRL task:
#include "log.h"                      // LOG task: packet logging

#include "ogn.h"                      // OGN packet structures, encoding/decoding/etc.

// #include "rf.h"                       // RF task: transmission and reception of radio packets
#include "ogn-radio.h"                // RF task: transmission and reception of radio packets
#include "gps.h"                      // GPS task: get own time and position, set the GPS baudrate and navigation mode

#include "fifo.h"

#ifdef WITH_FLASHLOG                  // log own track to unused Flash pages (STM32 only)
#include "flashlog.h"
#endif
#ifdef WITH_SDLOG
#include "sdlog.h"
#endif
#ifdef WITH_APRS
#include "aprs.h"
#endif

#ifdef WITH_SOUND
#include "sound.h"
#endif

#ifdef WITH_GDL90
#include "gdl90.h"
GDL90_HEARTBEAT GDL_HEARTBEAT;
GDL90_REPORT GDL_REPORT;
#endif

#ifdef WITH_LOOKOUT                   // traffic awareness and warnings
#include "lookout.h"
LookOut<32> Look;
#ifdef WITH_SOUND
const char *Dir[16] = { "N", "NNE", "NE", "NEE", "E", "SEE", "SE", "SSE", "S", "SSW", "SW", "SWW", "W", "NWW", "NW", "NNW" };
const char *RelDir[8] = { "A", "AR", "R", "BR", "B", "BL", "L", "AL" };

void Sound_TrafficWarn(const LookOut_Target *Tgt)
{ if(!Tgt) return;
  uint8_t WarnLevel = Tgt->WarnLevel;
  // uint16_t DistMargin = Tgt->DistMargin; // [0.5m]
  uint16_t TimeMargin = Tgt->TimeMargin; // [0.5s]
  uint16_t HorDist = Tgt->HorDist;       // [0.5]
  uint16_t Bearing = Tgt->getBearing();  //
  int16_t RelBearing = Look.getRelBearing(Tgt);
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "Traffic: ");
  CONS_UART_Write('#');
  CONS_UART_Write('0'+WarnLevel);
  CONS_UART_Write(' ');
  // Format_Hex(CONS_UART_Write, Bearing);
  // CONS_UART_Write(' ');
  uint16_t DirIdx = (Bearing+0x800)>>12; DirIdx&=0x0F;
  Format_String(CONS_UART_Write, Dir[DirIdx]);
  CONS_UART_Write(' ');
  uint16_t RelDirIdx = (RelBearing+0x1000)>>13; RelDirIdx&=0x07;
  Format_String(CONS_UART_Write, RelDir[RelDirIdx]);
  CONS_UART_Write(' ');
  Format_UnsDec(CONS_UART_Write, (uint16_t)(HorDist/2));
  Format_String(CONS_UART_Write, "m ");
  Format_UnsDec(CONS_UART_Write, (uint16_t)(TimeMargin/2));
  Format_String(CONS_UART_Write, "s\n");
  xSemaphoreGive(CONS_Mutex);
  // SoundMsg("Traffic");
}
#endif
#endif

// static uint16_t PrevBattVolt = 0;     // [mV]
static Delay<uint16_t, 32> BatteryVoltagePipe;
uint32_t BatteryVoltage = 0;          // [1/256 mV] low-pass filtered battery voltage
 int32_t BatteryVoltageRate = 0;      // [1/256 mV/sec] low-pass filtered battery voltage rise/drop rate

static char           Line[160];      // for printing out to the console, etc.

static LDPC_Decoder     Decoder;      // decoder and error corrector for the OGN Gallager/LDPC code

// FlightMonitor Flight;

// #define DEBUG_PRINT

// =======================================================================================================================================

#ifdef WITH_LOG

// log a received packet
static int FlashLog(OGN_RxPacket<OGN_Packet> *Packet, uint32_t Time)
{ OGN_LogPacket<OGN_Packet> *LogPacket = FlashLog_FIFO.getWrite(); if(LogPacket==0) return -1; // allocate new packet in the LOG_FIFO
  LogPacket->Packet = Packet->Packet;                                                          // copy the packet
  LogPacket->Flags=0x80;                                                                       // set Rx flag
  LogPacket->setTime(Time);
  LogPacket->setCheck();
  FlashLog_FIFO.Write();                                                                       // finalize the write
  return 1; }

// log own packet
static int FlashLog(OGN_TxPacket<OGN_Packet> *Packet, uint32_t Time)
{ OGN_LogPacket<OGN_Packet> *LogPacket = FlashLog_FIFO.getWrite(); if(LogPacket==0) return -1;
  LogPacket->Packet = Packet->Packet;
  LogPacket->Flags=0x00;                                                                       // clear Rx flag
  // LogPacket->SNR = ;
  LogPacket->setTime(Time);
  LogPacket->setCheck();
  FlashLog_FIFO.Write();
  return 1; }

#endif // WITH_LOG

// ---------------------------------------------------------------------------------------------------------------------------------------

Relay_PrioQueue<OGN_RxPacket<OGN_Packet>, RelayQueueSize> OGN_RelayQueue;       // received OGN packets and candidates to be relayed
Relay_PrioQueue<ADSL_RxPacket, RelayQueueSize>           ADSL_RelayQueue;       // received ADSL packets and candidates to be relayed

#ifdef DEBUG_PRINT
static void PrintRelayQueue(uint8_t Idx)                    // for debug
{ uint8_t Len=0;
  // Len+=Format_String(Line+Len, "");
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  // Format_String(CONS_UART_Write, Line, Len);
  Line[Len++]='['; Len+=Format_Hex(Line+Len, Idx); Line[Len++]=']'; Line[Len++]=' ';
  Len+=OGN_RelayQueue.Print(Line+Len);
  Format_String(CONS_UART_Write, Line);
  xSemaphoreGive(CONS_Mutex); }
#endif

static bool GetRelayPacket(OGN_TxPacket<OGN_Packet> *Packet)      // prepare a packet to be relayed
{ if(OGN_RelayQueue.Sum==0) return 0;                     // if no packets in the relay queue
  XorShift32(Random.RX);                                  // produce a new random number
  uint8_t Idx=OGN_RelayQueue.getRand(Random.RX);          // get weight-random packet from the relay queue
  if(OGN_RelayQueue.Packet[Idx].Rank==0) return 0;        // should not happen ...
  memcpy(Packet->Packet.Byte(), OGN_RelayQueue[Idx]->Byte(), OGN_Packet::Bytes); // copy the packet
  Packet->Packet.Header.Relay=1;                          // increment the relay count (in fact we only do single relay)
  // Packet->Packet.calcAddrParity();
  if(!Packet->Packet.Header.Encrypted) Packet->Packet.Whiten(); // whiten but only for non-encrypted packets
  Packet->calcFEC();                                      // Calc. the FEC code => packet ready for transmission
  // PrintRelayQueue(Idx);  // for debug
  OGN_RelayQueue.decrRank(Idx);                           // reduce the rank of the packet selected for relay
  return 1; }

static bool GetRelayPacket(ADSL_Packet *Packet)           // prepare a packet to be relayed
{ if(ADSL_RelayQueue.Sum==0) return 0;                    // if no packets in the relay queue
  XorShift32(Random.RX);                                  // produce a new random number
  uint8_t Idx=ADSL_RelayQueue.getRand(Random.RX);         // get weight-random packet from the relay queue
  if(ADSL_RelayQueue.Packet[Idx].Rank==0) return 0;       // should not happen ...
  *Packet = ADSL_RelayQueue[Idx]->Packet;
  Packet->setRelay();
  Packet->Scramble();
  Packet->setCRC();
  ADSL_RelayQueue.decrRank(Idx);                           // reduce the rank of the packet selected for relay
  return 1; }

static void CleanRelayQueue(uint32_t Time, uint32_t Delay=12) // remove "old" packets from the relay queue
{ Time-=Delay;
  uint8_t Sec = Time%60;
  OGN_RelayQueue.cleanTime(Sec);                         // remove packets 20(default) seconds into the past
  uint8_t qSec = Sec%15;
  ADSL_RelayQueue.cleanTime(qSec<<2); }

// ---------------------------------------------------------------------------------------------------------------------------------------

static uint16_t InfoParmIdx = 0;            // the round-robin index to info records in info packets

static int ReadInfo(OGN1_Packet &Packet)
{ Packet.clrInfo();
  uint8_t ParmIdx;
  for( ParmIdx=InfoParmIdx; ; )
  { const char *Parm = Parameters.InfoParmValue(ParmIdx);
    if(Parm)
    { // printf("Parm[%d]=%s\n", ParmIdx, Parm);
      if(Parm[0])
      { int Add=Packet.addInfo(Parm, ParmIdx); if(Add==0) break; }
    }
    ParmIdx++; if(ParmIdx>=Parameters.InfoParmNum) ParmIdx=0;
    if(ParmIdx==InfoParmIdx) break;
  }
  InfoParmIdx = ParmIdx;
  Packet.setInfoCheck();
  return Packet.Info.DataChars; }                                      // zero => no info parameters were stored

// ---------------------------------------------------------------------------------------------------------------------------------------

template <class Type>
 static uint8_t Limit(Type X, Type Low, Type Upp)
{ if(X<Low) return Low;
  if(X>Upp) return Upp;
  return X; }

#ifdef GPS_PinPPS
static int getTelemSatPPS(ADSL_Packet &Packet)
{ Packet.Init(0x42);
  Packet.setAddress    (Parameters.Address);
  Packet.setAddrTypeOGN(Parameters.AddrType);
  Packet.setRelay(0);
  Packet.Telemetry.Header.TelemType=0x3;                            // 3 = GPS telemetry
  Packet.SatSNR.Header.GNSStype=1;                                  // 1 = GPS PPS monitor
  if(PPS_Intr_Count==0) return 0;
  uint32_t msTime = xTaskGetTickCount();                            // [ms] current sys-time
  uint32_t PPSage = msTime-PPS_Intr_msTime;                         // [ms] how old the last PPS is
  if(PPSage>20000) return 0;
  uint32_t UTC = GPS_TimeSync.UTC;
  uint32_t UTCage = msTime-GPS_TimeSync.sysTime;                    // [ms] time since last ref. UTC
  PPSage -= UTCage;                                                 //
  PPSage += 500;
  Packet.SatPPS.Data.UTC = UTC - PPSage/1000;                       // [sec] the UTC time of the last PPS interrupt
  Packet.SatPPS.Data.ClockTime = msTime-PPS_usPrecTime;             //
  Packet.SatPPS.Data.ClockTimeRMS = Limit(IntSqrt(PPS_usTimeRMS<<4), (uint32_t)0, (uint32_t)255);
  Packet.SatPPS.Data.RefClock = 16;                                 // [MHz]
  Packet.SatPPS.Data.PPScount = Limit(PPS_Intr_Count, (uint32_t)0, (uint32_t)240);      // [sec]
  int32_t FreqError = -PPS_usPeriodErr;
  FreqError = (FreqError+8)>>4;                                     // [ppm]
  Packet.SatPPS.Data.PPSerror = Limit(FreqError, (int32_t)-127, (int32_t)+127);
  Packet.SatPPS.Data.PPSresid = Limit(IntSqrt(PPS_usPeriodRMS<<4), (uint32_t)0, (uint32_t)255);
  return 1; }
#else
static int getTelemSatPPS(ADSL_Packet &Packet) { return 0; }
#endif

static void getTelemSatSNR(ADSL_Packet &Packet)
{ Packet.Init(0x42);
  Packet.setAddress    (Parameters.Address);
  Packet.setAddrTypeOGN(Parameters.AddrType);
  Packet.setRelay(0);
  Packet.Telemetry.Header.TelemType=0x3;                            // 3 = GPS telemetry
  Packet.SatSNR.Header.GNSStype=0;                                  // 0 = GPS satellite SNR
  for(uint8_t Sys=0; Sys<5; Sys++)
  { Packet.SatSNR.Data.SatSNR[Sys]=GPS_SatMon.getSysStatus(Sys); }
  // Serial.printf("SatSNR: %04X %04X %04X %04X %04X\n",
  //     Packet.SatSNR.Data.SatSNR[0], Packet.SatSNR.Data.SatSNR[1], Packet.SatSNR.Data.SatSNR[2], Packet.SatSNR.Data.SatSNR[3], Packet.SatSNR.Data.SatSNR[4]);
  Packet.SatSNR.Data.Inbalance = 0;
  Packet.SatSNR.Data.PDOP = GPS_SatMon.PDOP;
  Packet.SatSNR.Data.HDOP = GPS_SatMon.HDOP;
  Packet.SatSNR.Data.VDOP = GPS_SatMon.VDOP; }

static void getTelemStatus(ADSL_Packet &Packet, const GPS_Position *GPS)
{ Packet.Init(0x42);
  Packet.setAddress    (Parameters.Address);
  Packet.setAddrTypeOGN(Parameters.AddrType);
  Packet.setRelay(0);
  Packet.Telemetry.Header.TelemType=0x0;                            // 0 => device status
  if(GPS) GPS->EncodeTelemetry(Packet);
#ifdef WITH_SX1276
  if(Packet.Telemetry.Baro.Temperature==(-128)) Packet.Telemetry.Baro.Temperature=Radio_ChipTemperature*2;
#endif
  uint8_t SNR = (GPS_SatSNR+2)/4;                                   // encode number of satellites and SNR in the Status packet
  if(SNR>10) { SNR-=10; if(SNR>31) SNR=31; }
        else { SNR=0; }
  Packet.Telemetry.GPS.SNR=SNR;
  uint16_t BattVolt = BatterySense();                               // [mV] measure battery voltage
  Packet.Telemetry.Battery.Voltage  = EncodeUR2V8(BattVolt/4);
  int BattCap = ((int)BattVolt-3300)/16;                            // approx. formula
  Packet.Telemetry.Battery.Capacity = Limit(BattCap, 0, 63);
  Packet.Telemetry.Radio.RxNoise = Limit(120+(int)floorf(Radio_BkgRSSI+0.5), 0, 63);
  Packet.Telemetry.Radio.RxRate  = EncodeUR2V4(floorf(Radio_PktRate*4+0.5f));
  Packet.Telemetry.Radio.TxPower = Limit(Parameters.TxPower-10, 0, 15); }

static void ReadStatus(OGN_Packet &Packet)
{
// #ifdef WITH_JACEK
/*
  xSemaphoreTake(ADC1_Mutex, portMAX_DELAY);
  uint16_t MCU_Vtemp  = ADC_Read_MCU_Vtemp();                                // T = 25+(V25-Vtemp)/Avg_Slope; V25=1.43+/-0.1V, Avg_Slope=4.3+/-0.3mV/degC
  uint16_t MCU_Vref   = ADC_Read_MCU_Vref();                                 // VDD = 1.2*4096/Vref
           MCU_Vtemp += ADC_Read_MCU_Vtemp();                                // measure again and average
           MCU_Vref  += ADC_Read_MCU_Vref();
#ifdef WITH_BATT_SENSE
  uint16_t Vbatt       = ADC_Read_Vbatt();                                   // measure voltage on PB1
           Vbatt      += ADC_Read_Vbatt();
#endif
  xSemaphoreGive(ADC1_Mutex);
   int16_t MCU_Temp = -999;                                                  // [0.1degC]
  uint16_t MCU_VCC = 0;                                                      // [0.01V]
  if(MCU_Vref)
  { MCU_Temp = 250 + ( ( ( (int32_t)1430 - ((int32_t)1200*(int32_t)MCU_Vtemp+(MCU_Vref>>1))/MCU_Vref )*(int32_t)37 +8 )>>4); // [0.1degC]
    MCU_VCC  = ( ((uint32_t)240<<12)+(MCU_Vref>>1))/MCU_Vref; }              // [0.01V]
  Packet.EncodeVoltage(((MCU_VCC<<4)+12)/25);                     // [1/64V]  write supply voltage to the status packet
#ifdef WITH_BATT_SENSE
  if(MCU_Vref)
    Packet.EncodeVoltage(((int32_t)154*(int32_t)Vbatt+(MCU_Vref>>1))/MCU_Vref); // [1/64V] battery voltage assuming 1:1 divider form battery to PB1
#endif
*/

  // Packet.clrHumidity();
#ifdef WITH_STM32
#ifdef WITH_JACEK
  uint16_t MCU_Vbatt   = Measure_Vbatt();                                    // [0.001V]
  Packet.EncodeVoltage(((MCU_Vbatt<<3)+62)/125);                             // [1/64V]
  if(MCU_Vbatt<3600)
  { uint16_t FlashLen = 3600-MCU_Vbatt; if(FlashLen>250) FlashLen=250;
    LED_BAT_Flash(FlashLen); }
#else // WITH_JACEK
  uint16_t MCU_VCC   = Measure_MCU_VCC();                                    // [0.001V]
  Packet.EncodeVoltage(((MCU_VCC<<3)+62)/125);                               // [1/64V]
#endif
  int16_t MCU_Temp  = Measure_MCU_Temp();                                    // [0.1degC]
#endif

#ifdef WITH_ESP32
  // Packet.clrTemperature();

  uint16_t BattVolt = BatterySense();                                        // [mV] measure battery voltage
  if(BatteryVoltage>0)
  { // int32_t PrevVolt = BatteryVoltage;
    int32_t Rate = ((uint32_t)BattVolt<<8) - BatteryVoltage;                 // [1/256 mV]
    BatteryVoltage += (Rate+32)>>6;                                          // [1/256 mV] low-pass battery voltage measurement
    uint16_t Volt = (BatteryVoltage+16)>>5;
    int16_t Diff = Volt-BatteryVoltagePipe.Input(Volt);
    BatteryVoltageRate = Diff; }
    // BatteryVoltageRate = BatteryVoltage - PrevVolt; }
    // int16_t BattVoltDiff = BattVolt - PrevBattVolt;
    // int32_t Diff = ((int32_t)BattVoltDiff<<8) - BatteryVoltageRate;
    // BatteryVoltageRate += Diff/256; }
    // BatteryVoltageRate = (((int32_t)BattVoltDiff<<8) + BatteryVoltageRate*255 + 128)>>8; }
  else
  { BatteryVoltage = BattVolt<<8;
    // PrevBattVolt = BattVolt;
    BatteryVoltagePipe.Clear(BattVolt<<3);
    BatteryVoltageRate = 0; }
  // PrevBattVolt = BattVolt;
  Packet.EncodeVoltage(((BatteryVoltage>>2)+500)/1000);            // [1/64V] encode into the status packet

#ifdef DEBUG_PRINT
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "Battery: ");
  // Format_UnsDec(CONS_UART_Write, BattVolt);
  // CONS_UART_Write(' ');
  // Format_UnsDec(CONS_UART_Write, PrevBattVolt);
  // CONS_UART_Write(' ');
  // Format_UnsDec(CONS_UART_Write, BatteryVoltage, 2);
  // CONS_UART_Write(' ');
  // Format_SignDec(CONS_UART_Write, BatteryVoltageRate, 2);
  // CONS_UART_Write(' ');
  Format_UnsDec(CONS_UART_Write, (10*BatteryVoltage+128)>>8, 5, 4);
  Format_String(CONS_UART_Write, "V ");
  Format_SignDec(CONS_UART_Write, (600*BatteryVoltageRate+128)>>8, 3, 1);
  Format_String(CONS_UART_Write, "mV/min\n");
  xSemaphoreGive(CONS_Mutex);
#endif
#endif

// #ifdef WITH_SX1262
//   if(Packet.Status.Pressure==0) Packet.clrTemperature();
// #else
//   if(Packet.Status.Pressure==0) Packet.EncodeTemperature(Radio_ChipTemperature*10); // [0.1degC]
// #endif
  Packet.Status.RadioNoise = floorf(-2*Radio_BkgRSSI+0.f); // TRX.averRSSI;                         // [-0.5dBm] write radio noise to the status packet

  uint8_t TxPower = Parameters.TxPower-4;
  if(TxPower>15) TxPower=15;
  Packet.Status.TxPower = TxPower;

  uint16_t RxRate = floorf(Radio_PktRate*60+0.5f)+1;
  uint8_t RxRateLog2=0; RxRate>>=1; while(RxRate) { RxRate>>=1; RxRateLog2++; }
  Packet.Status.RxRate = RxRateLog2;

  if(Parameters.Verbose)
  { uint8_t Len=0;
    Len+=Format_String(Line+Len, "$POGNR,");                                  // NMEA report: radio status
    Len+=Format_UnsDec(Line+Len, (uint32_t)Radio_FreqPlan.Plan);              // which frequency plan
    Line[Len++]=',';
    // Len+=Format_UnsDec(Line+Len, (uint32_t)RX_OGN_Count64);                   // number of OGN packets received
    Line[Len++]=',';
    Line[Len++]=',';
    // Len+=Format_SignDec(Line+Len, -5*TRX.averRSSI, 2, 1);                     // average RF level (over all channels)
    Line[Len++]=',';
    Len+=Format_SignDec(Line+Len, Radio_TxCredit/100, 2, 1);                       // [sec] transmitter on-air time counter
    Line[Len++]=',';
    // Len+=Format_SignDec(Line+Len, (int16_t)TRX.chipTemp);                     // the temperature of the RF chip
    Line[Len++]=',';
    // Len+=Format_SignDec(Line+Len, MCU_Temp, 2, 1);
    Line[Len++]=',';
#ifdef WITH_STM32
#ifdef WITH_JACEK
    Len+=Format_UnsDec(Line+Len, (MCU_Vbatt+5)/10, 3, 2);
#else
    // Len+=Format_UnsDec(Line+Len, (MCU_VCC+5)/10, 3, 2);
#endif
#endif

    Len+=NMEA_AppendCheckCRNL(Line, Len);                                    // append NMEA check-sum and CR+NL
    // LogLine(Line);
    // if(CONS_UART_Free()>=128)
    { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);                          // send the NMEA out to the console
      xSemaphoreGive(CONS_Mutex); }
#ifdef WITH_SDLOG
    if(Log_Free()>=128)
    { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
      Format_String(Log_Write, Line, 0, Len);                                // send the NMEA out to the log file
      xSemaphoreGive(Log_Mutex); }
#endif
  }
}

// static void ReadStatus(OGN_TxPacket<OGN_Packet> &StatPacket)
// { ReadStatus(StatPacket.Packet); }

#ifndef WITH_LOOKOUT                                    // with LookOut the PFLAU is produced inside LookOut
static uint8_t WritePFLAU(char *NMEA, uint8_t GPS=1)    // produce the (mostly dummy) PFLAU to satisfy XCsoar and LK8000
{ uint8_t Len=0;
  Len+=Format_String(NMEA+Len, "$PFLAU,");
  NMEA[Len++]='0';
  NMEA[Len++]=',';
  NMEA[Len++]='0'+GPS;                                  // TX status
  NMEA[Len++]=',';
  NMEA[Len++]='0'+GPS;                                  // GPS status
  NMEA[Len++]=',';
  NMEA[Len++]='1';                                      // power status: one could monitor the supply
  NMEA[Len++]=',';
  NMEA[Len++]='0';
  NMEA[Len++]=',';
  NMEA[Len++]=',';
  NMEA[Len++]='0';
  NMEA[Len++]=',';
  NMEA[Len++]=',';
  Len+=NMEA_AppendCheckCRNL(NMEA, Len);
  NMEA[Len]=0;
  return Len; }
#endif

// ---------------------------------------------------------------------------------------------------------------------------------------

// process received OGN packets
static void ProcessRxOGN(OGN_RxPacket<OGN_Packet> *RxPacket, uint8_t RxPacketIdx, uint32_t RxTime)
{ int32_t LatDist=0, LonDist=0; uint8_t Warn=0;
  if( RxPacket->Packet.Header.NonPos)                                                 // status or info packet
  {
#ifdef WITH_SDLOG
    IGClog_FIFO.Write(*RxPacket);                                                     // unconditionally log all non-position packets ?
#endif
    return ; }
  uint8_t MyOwnPacket = ( RxPacket->Packet.Header.Address  == Parameters.Address  )
                     && ( RxPacket->Packet.Header.AddrType == Parameters.AddrType );
  if(MyOwnPacket) return;                                                             // don't process my own (relayed) packets
  if(RxPacket->Packet.Header.Encrypted && RxPacket->RxErr<10)                         // here we attempt to relay encrypted packets
  { RxPacket->calcRelayRank(GPS_Altitude/10);
    OGN_RxPacket<OGN_Packet> *PrevRxPacket = OGN_RelayQueue.addNew(RxPacketIdx);      // add to the relay queue and get the previous packet of same ID
#ifdef WITH_SDLOG
    IGClog_FIFO.Write(*RxPacket);                                                     // log encrypted position packets
#endif
    return; }
  bool DistOK = RxPacket->Packet.calcDistanceVector(LatDist, LonDist, GPS_Latitude, GPS_Longitude, GPS_LatCosine)>=0;
  if(DistOK)                                                                          // reasonable reception distance
  { RxPacket->LatDist=LatDist;
    RxPacket->LonDist=LonDist;
    RxPacket->calcRelayRank(GPS_Altitude/10);                                         // calculate the relay-rank (priority for relay)
    OGN_RxPacket<OGN_Packet> *PrevRxPacket = OGN_RelayQueue.addNew(RxPacketIdx);          // add to the relay queue and get the previous packet of same ID
    // Serial.printf("ProcessRxOGN : %02X:%06X [%+5d,%+5d]m\n",
    //          RxPacket->Packet.Header.AddrType, RxPacket->Packet.Header.Address, LatDist, LonDist);
#ifdef WITH_POGNT
    { uint8_t Len=RxPacket->WritePOGNT(Line);                                         // print on the console as $POGNT
      if(Parameters.Verbose)
      { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
        Format_String(CONS_UART_Write, Line, 0, Len);
        xSemaphoreGive(CONS_Mutex); }
#ifdef WITH_SDLOG
      if(Log_Free()>=128)
      { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
        Format_String(Log_Write, Line, 0, Len);
        xSemaphoreGive(Log_Mutex); }
#endif
    }
#endif
//     Len=RxPacket->Packet.WriteAPRS(Line, RxTime);                                     // print on the console as APRS message
//     xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
//     Format_String(CONS_UART_Write, Line, 0, Len);
//     xSemaphoreGive(CONS_Mutex);
#ifdef WITH_LOOKOUT
    const LookOut_Target *Tgt=Look.ProcessTarget(RxPacket->Packet, RxTime);           // process the received target postion
    if(Tgt) Warn=Tgt->WarnLevel;                                                      // remember warning level of this target
    RxPacket->Warn = Warn>0;
#ifdef WITH_GDL90
    if(Tgt)
    { Look.Write(GDL_REPORT, Tgt);                                                    // produce GDL90 report for this target
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      GDL_REPORT.Send(CONS_UART_Write, 20);                                           // transmit as traffic position report (not own-ship)
      xSemaphoreGive(CONS_Mutex); }
#endif
#ifdef WITH_BEEPER
    if(KNOB_Tick>12) Play(Play_Vol_1 | Play_Oct_2 | (7+2*Warn), 3+16*Warn);
#endif
#else // if not WITH_LOOKOUT
#ifdef WITH_BEEPER
    if(KNOB_Tick>12) Play(Play_Vol_1 | Play_Oct_2 | 7, 3);                            // if Knob>12 => make a beep for every received packet
#endif
#endif // WITH_LOOKOUT
     bool Signif = PrevRxPacket==0;
     if(!Signif) Signif=OGN_isSignif(&(RxPacket->Packet), &(PrevRxPacket->Packet));  // compare against previous packet of same ID from the relay queue
#ifdef WITH_APRS
     if(Signif) APRSrx_FIFO.Write(*RxPacket);                                        // APRS queue for received packets
#endif
#ifdef WITH_LOG
     if(Signif) FlashLog(RxPacket, RxTime);                                          // log only significant packets
#endif
#ifdef WITH_SDLOG
     if(Signif || Warn) IGClog_FIFO.Write(*RxPacket);
#endif
#ifdef WITH_PFLAA
    if( Parameters.Verbose    // print PFLAA on the console for received packets
#ifdef WITH_LOOKOUT
    && (!Tgt)
#endif
    )
    { uint8_t Len=RxPacket->WritePFLAA(Line, Warn, LatDist, LonDist, RxPacket->Packet.DecodeAltitude()-GPS_Altitude/10);
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);
      xSemaphoreGive(CONS_Mutex);
#ifdef WITH_SDLOG
    if(Log_Free()>=128)
    { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
      Format_String(Log_Write, Line, 0, Len);                                // send the NMEA out to the log file
      xSemaphoreGive(Log_Mutex); }
#endif
    }
#endif // WITH_PFLAA
#ifdef WITH_MAVLINK
   MAV_ADSB_VEHICLE MAV_RxReport;
   RxPacket->Packet.Encode(&MAV_RxReport);
   MAV_RxMsg::Send(sizeof(MAV_RxReport), MAV_Seq++, MAV_SysID, MAV_COMP_ID_ADSB, MAV_ID_ADSB_VEHICLE, (const uint8_t *)&MAV_RxReport, GPS_UART_Write);
   // xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
   // MAV_RxMsg::Send(sizeof(MAV_RxReport), MAV_Seq++, MAV_SysID, MAV_COMP_ID_ADSB, MAV_ID_ADSB_VEHICLE, (const uint8_t *)&MAV_RxReport, CONS_UART_Write);
   // xSemaphoreGive(CONS_Mutex);
#endif
  }
}

// process received ADS-L packets
static void ProcessRxADSL(ADSL_RxPacket *RxPacket, uint8_t RxPacketIdx, uint32_t RxTime)
{ int32_t LatDist=0, LonDist=0; uint8_t Warn=0;
  if(!RxPacket->Packet.isPos())                                                 // status or info packet
  {
// #ifdef WITH_SDLOG
//     IGClog_FIFO.Write(*RxPacket);                                                     // unconditionally log all non-position packets ?
// #endif
    return ; }
  uint8_t AddrType = RxPacket->Packet.getAddrTable();
  if(AddrType<4) AddrType=0;
  else AddrType-=4;
  uint8_t MyOwnPacket = ( RxPacket->Packet.getAddress()  == Parameters.Address )
                     && (                       AddrType == Parameters.AddrType);
  if(MyOwnPacket) return;                                                             // don't process my own (relayed) packets
  bool DistOK = RxPacket->calcDistanceVector(LatDist, LonDist, GPS_Latitude, GPS_Longitude, GPS_LatCosine)>=0;
  if(DistOK)                                                                          // reasonable reception distance
  { RxPacket->LatDist=LatDist;
    RxPacket->LonDist=LonDist;
    RxPacket->calcRelayRank(GPS_Altitude/10);                                         // calculate the relay-rank (priority for relay)
    ADSL_RxPacket *PrevRxPacket = ADSL_RelayQueue.addNew(RxPacketIdx);                // add to the relay queue and get the previ>
    // Serial.printf("ProcessRxADSL: %02X:%06X [%+5d,%+5d]m\n",
    //          RxPacket->Packet.getAddrTable(), RxPacket->Packet.getAddress(), LatDist, LonDist);
#ifdef WITH_LOOKOUT
    const LookOut_Target *Tgt=Look.ProcessTarget(RxPacket->Packet, RxTime);           // process the received target postion
    if(Tgt) Warn=Tgt->WarnLevel;                                                      // remember warning level of this target
    RxPacket->Warn = Warn>0;
#ifdef WITH_GDL90
    if(Tgt)
    { Look.Write(GDL_REPORT, Tgt);                                                    // produce GDL90 report for this target
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      GDL_REPORT.Send(CONS_UART_Write, 20);                                           // transmit as traffic position report (not own-ship)
      xSemaphoreGive(CONS_Mutex); }
#endif
#ifdef WITH_BEEPER
    if(KNOB_Tick>12) Play(Play_Vol_1 | Play_Oct_2 | (7+2*Warn), 3+16*Warn);
#endif
#else // if not WITH_LOOKOUT
#ifdef WITH_BEEPER
    if(KNOB_Tick>12) Play(Play_Vol_1 | Play_Oct_2 | 7, 3);                            // if Knob>12 => make a beep for every received packet
#endif
#endif // WITH_LOOKOUT
/*
#ifdef WITH_PFLAA
    if( Parameters.Verbose    // print PFLAA on the console for received packets
#ifdef WITH_LOOKOUT
    && (!Tgt)
#endif
    )
    { uint8_t Len=RxPacket->WritePFLAA(Line, Warn, LatDist, LonDist, RxPacket->Packet.DecodeAltitude()-GPS_Altitude/10);
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_String(CONS_UART_Write, Line, 0, Len);
      xSemaphoreGive(CONS_Mutex);
    }
#endif
*/
  }
}

static void DecodeRxOGN(FSK_RxPacket *RxPkt)
{ uint8_t RxPacketIdx  = OGN_RelayQueue.getNew();                   // get place for this new packet
  OGN_RxPacket<OGN_Packet> *RxPacket = OGN_RelayQueue[RxPacketIdx];
  uint8_t Check = RxPkt->Decode(*RxPacket, Decoder);
#ifdef DEBUG_PRINT
  Serial.printf("DecodeRxOGN : #%d [%d] %02X:%06X Err:%d Corr:%d Check:%d [%d]\n",
     RxPkt->Channel, RxPkt->Bytes, RxPacket->Packet.Header.AddrType, RxPacket->Packet.Header.Address,
     RxPkt->ErrCount(), RxPacket->RxErr, Check, RxPacketIdx);
#endif
  if(Check!=0 || RxPacket->RxErr>=15) return;                     // what limit on number of detected bit errors ?
  RxPacket->Packet.Dewhiten();
  ProcessRxOGN(RxPacket, RxPacketIdx, RxPkt->Time); }

static void DecodeRxADSL(FSK_RxPacket *RxPkt)
{ uint8_t RxPacketIdx  = ADSL_RelayQueue.getNew();                   // get place for this new packet
  ADSL_RxPacket *RxPacket = ADSL_RelayQueue[RxPacketIdx];
  int CorrErr=RxPkt->ErrCount();
  if(RxPkt->Manchester) CorrErr=ADSL_Packet::Correct(RxPkt->Data, RxPkt->Err);
#ifdef DEBUG_PRINT
  Serial.printf("DecodeRxADSL: #%d [%d] Err:%d Corr:%d [%d]\n",
          RxPkt->Channel, RxPkt->Bytes, RxPkt->ErrCount(), CorrErr, RxPacketIdx);
#endif
  if(CorrErr<0) return;
  memcpy(&(RxPacket->Packet.Version), RxPkt->Data, RxPacket->Packet.TxBytes-3);
  RxPacket->RxErr   = CorrErr;
  RxPacket->RxChan  = RxPkt->Channel;
  RxPacket->RxRSSI  = RxPkt->RSSI;
  RxPacket->Correct = 1;
  RxPacket->Packet.Descramble();
  // Serial.printf("DecodeRxADSL : #%d %02X:%06X Err:%d Corr:%d\n",
  //          RxPkt->Channel, RxPacket->Packet.getAddrTable(), RxPacket->Packet.getAddress(), RxPkt->ErrCount(), CorrErr);
  ProcessRxADSL(RxPacket, RxPacketIdx, RxPkt->Time); }

static void DecodeRxLDR(FSK_RxPacket *RxPkt)
{ if(RxPkt->Bytes!=25 || RxPkt->Manchester) return;
  uint32_t CRC = ADSL_Packet::checkPI(RxPkt->Data, 24);
  uint8_t CRC8 = PAW_Packet::CRC8(RxPkt->Data, 24);
  if(CRC!=0 && CRC8!=RxPkt->Data[24])
  { uint8_t ErrBit=ADSL_Packet::FindCRCsyndrome(CRC);
    if(ErrBit!=0xFF)
    { ADSL_Packet::FlipBit(RxPkt->Data, ErrBit);
      ADSL_Packet::FlipBit(RxPkt->Err , ErrBit);
      CRC=0x000000;
      CRC8 = PAW_Packet::CRC8(RxPkt->Data, 24); }
  }
  if(CRC8!=RxPkt->Data[24]) return;
  if(CRC==0)
  { // Serial.printf("LDR: good ADS-L\n");
    DecodeRxADSL(RxPkt);
    return; }
  PAW_Packet::Whiten(RxPkt->Data, 24);
  if(PAW_Packet::IntCRC(RxPkt->Data, 24)!=0x00) return;
  // Serial.printf("LDR: good PAW\n");
  PAW_Packet *PAW = (PAW_Packet *)RxPkt->Data;
  uint8_t RxPacketIdx  = OGN_RelayQueue.getNew();
  OGN_RxPacket<OGN_Packet> *RxPacket = OGN_RelayQueue[RxPacketIdx];
  PAW->Write(RxPacket->Packet);
  RxPacket->RxErr  = 0;
  RxPacket->RxChan = RxPkt->Channel;
  RxPacket->RxRSSI = RxPkt->RSSI;
  RxPacket->Correct = 1;
  ProcessRxOGN(RxPacket, RxPacketIdx, RxPkt->Time); }

static void DecodeRxPacket(FSK_RxPacket *RxPkt)
{ if(RxPkt->SysID==Radio_SysID_OGN ) return DecodeRxOGN (RxPkt);
  if(RxPkt->SysID==Radio_SysID_ADSL) return DecodeRxADSL(RxPkt);
  if(RxPkt->SysID==Radio_SysID_LDR ) return DecodeRxLDR (RxPkt);
  return; }

// -------------------------------------------------------------------------------------------------------------------

#ifdef __cplusplus
  extern "C"
#endif
void vTaskPROC(void* pvParameters)
{
#ifdef WITH_FLASHLOG
  uint16_t kB = FlashLog_OpenForWrite();
  xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
  Format_String(CONS_UART_Write, "TaskPROC: ");
  Format_UnsDec(CONS_UART_Write, kB);
  Format_String(CONS_UART_Write, "KB FlashLog\n");
  xSemaphoreGive(CONS_Mutex);
#endif
  OGN_RelayQueue.Clear();
  ADSL_RelayQueue.Clear();

#ifdef WITH_LOOKOUT
  Look.Clear();
#endif

  OGN_TxPacket<OGN_Packet> PosPacket;                                  // position packet
  OGN_Packet        PrevLoggedPacket;                                  // most recent logged packet
  uint32_t                 PosTime=0;                                  // [sec] when the position was recorded
  OGN_TxPacket<OGN_Packet> StatPacket;                                 // status report packet
  // OGN_TxPacket<OGN_Packet> InfoPacket;                                 // information packet

  for( ; ; )
  { vTaskDelay(1);

    for( ; ; )
    { FSK_RxPacket *RxPkt = FSK_RxFIFO.getRead();                        // check for new received packets
      if(RxPkt==0) break;                                                // if there is a new received packet
#ifdef DEBUG_PRINT
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_UnsDec(CONS_UART_Write, TimeSync_Time()%60, 2);
      CONS_UART_Write('.');
      Format_UnsDec(CONS_UART_Write, TimeSync_msTime(), 3);
      Format_String(CONS_UART_Write, " FSK_RxFIFO -> ");
      RxPkt->Print(CONS_UART_Write);
      // CONS_UART_Write('\r'); CONS_UART_Write('\n');
      xSemaphoreGive(CONS_Mutex);
#endif
      DecodeRxPacket(RxPkt);                                            // decode and process the received packet
      FSK_RxFIFO.Read(); }                                              // remove this packet from the queue

    static uint32_t PrevSlotTime=0;                                     // remember previous time slot to detect a change
    uint32_t     Time;                                                  // [sec] time slot
    TickType_t msTime;                                                  // [msec]
    TimeSync_Time(Time, msTime);
    uint32_t SlotTime=Time;
#ifdef WITH_GPS_UBX
    if(msTime<200) SlotTime--;                                          // lasts up to 0.300sec after the PPS
#endif
#ifdef WITH_GPS_MTK
    if(msTime<300) SlotTime--;                                          // lasts up to 0.300sec after the PPS
#endif

    if(SlotTime==PrevSlotTime) continue;                                // stil same time slot, go back to RX processing

    PrevSlotTime=SlotTime;                                              // new slot started
                                                                        // this part of the loop is executed only once per slot-time
    uint8_t BestIdx; int16_t BestResid;
#ifdef WITH_MAVLINK
    GPS_Position *Position = GPS_getPosition(BestIdx, BestResid, (SlotTime-1)%60, 0);
#else
    GPS_Position *Position = GPS_getPosition(BestIdx, BestResid, SlotTime%60, 0); // get GPS position which isReady
#endif
    // GPS_Position *Position = GPS_getPosition();
#ifdef DEBUG_PRINT
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    // Format_UnsDec(CONS_UART_Write, TimeSync_Time()%60, 2);
    // Format_UnsDec(CONS_UART_Write, Time%60, 2);
    Format_UnsDec(CONS_UART_Write, Time, 10);
    CONS_UART_Write('.');
    // Format_UnsDec(CONS_UART_Write, TimeSync_msTime(), 3);
    Format_UnsDec(CONS_UART_Write, msTime, 3);
    Format_String(CONS_UART_Write, " -> getPos(");
    Format_UnsDec(CONS_UART_Write, SlotTime%60, 2);
    Format_String(CONS_UART_Write, ") => ");
    if(Position)
    { Format_UnsDec(CONS_UART_Write, (uint16_t)BestIdx);
      CONS_UART_Write(':');
      Format_SignDec(CONS_UART_Write, BestResid, 4, 3);
      Format_String(CONS_UART_Write, "s"); }
    Format_String(CONS_UART_Write, "\n");
    xSemaphoreGive(CONS_Mutex);
#endif

#ifdef WITH_GDL90
    GDL_HEARTBEAT.Clear();
    GDL_HEARTBEAT.Initialized=1;
    if(Position)
    { if(Position->isTimeValid())
      { GDL_HEARTBEAT.UTCvalid=1;
        GDL_HEARTBEAT.setTimeStamp(SlotTime);
        if(Position->isValid()) GDL_HEARTBEAT.PosValid = 1; }
    }
    GDL_REPORT.Clear();
    GDL_REPORT.setAddress(Parameters.Address);
    GDL_REPORT.setAddrType(Parameters.AddrType!=1);
    GDL_REPORT.setAcftType(Parameters.AcftType);
    if(Parameters.Reg[0]) GDL_REPORT.setAcftCall(Parameters.Reg);
                     // else GDL_REPORT.setAcftCall();
    if(Position && Position->isValid()) Position->Encode(GDL_REPORT);
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    GDL_HEARTBEAT.Send(CONS_UART_Write);
    GDL_REPORT.Send(CONS_UART_Write);
    xSemaphoreGive(CONS_Mutex);
#endif
    if(Position)
    { Position->EncodeStatus(StatPacket.Packet);             // encode GPS altitude and pressure/temperature/humidity
#ifdef WITH_SX1276
      if(!StatPacket.Packet.hasTemperature()) StatPacket.Packet.EncodeTemperature((int16_t)Radio_ChipTemperature*10);
#endif
      /* Flight.Process(*Position); */ }                     // flight monitor: takeoff/landing
    else
    { StatPacket.Packet.Status.FixQuality=0; StatPacket.Packet.Status.Satellites=0; } // or lack of the GPS lock

    { uint8_t SatSNR = (GPS_SatSNR+2)/4;                     // encode number of satellites and SNR in the Status packet
      if(SatSNR>8) { SatSNR-=8; if(SatSNR>31) SatSNR=31; }
              else { SatSNR=0; }
      StatPacket.Packet.Status.SatSNR = SatSNR; }

    // if(Position && Position->isTimeValid() && Position->isDateValid()) PosTime=Position->getUnixTime();
    //                                                              else  PosTime=0;

    if( Position && Position->isReady && (!Position->Sent) && Position->isValid() )
    { int16_t AverSpeed=GPS_AverageSpeed();                           // [0.1m/s] average speed, including the vertical speed
      if(Parameters.FreqPlan==0)
        Radio_FreqPlan.setPlan(Position->Latitude, Position->Longitude); // set the frequency plan according to the GPS position
      else Radio_FreqPlan.setPlan(Parameters.FreqPlan);
#ifdef DEBUG_PRINT
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_UnsDec(CONS_UART_Write, TimeSync_Time()%60);
      CONS_UART_Write('.');
      Format_UnsDec(CONS_UART_Write, TimeSync_msTime(), 3);
      Format_String(CONS_UART_Write, " -> Sent\n");
      xSemaphoreGive(CONS_Mutex);
#endif // DEBUG_PRINT
      PosTime=Position->getUnixTime();
      PosPacket.Packet.HeaderWord=0;
      PosPacket.Packet.Header.Address    = Parameters.Address;         // set address
      PosPacket.Packet.Header.AddrType   = Parameters.AddrType;        // address-type
#ifdef WITH_ENCRYPT
      if(Parameters.Encrypt)                                           // if position encryption is requested
      { PosPacket.Packet.Header.Encrypted = 1; }                       // then set the flg in the header
#endif // WITH_ENCRYPT
      PosPacket.Packet.calcAddrParity();                               // parity of (part of) the header
      if(BestResid==0) Position->Encode(PosPacket.Packet);             // encode position/altitude/speed/etc. from GPS position
      else                                                             // extrapolate the position when if not at an exact UTC second
      { while(BestResid>=500) BestResid-=1000;                         // remove full seconds
        Position->Encode(PosPacket.Packet, BestResid); }
      PosPacket.Packet.Position.AcftType = Parameters.AcftType;        // aircraft-type
      PosPacket.Packet.Position.Stealth = Parameters.Stealth;
#ifdef DEBUG_PRINT
      { uint8_t Len=PosPacket.Packet.WriteAPRS(Line, PosTime);         // print on the console as APRS message
        Line[Len++]='\n'; Line[Len]=0;
        xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
        Format_String(CONS_UART_Write, Line, 0, Len);
        xSemaphoreGive(CONS_Mutex); }
#endif // DEBUG_PRINT
      OGN_TxPacket<OGN_Packet> *TxPacket = OGN_TxFIFO.getWrite();
      TxPacket->Packet = PosPacket.Packet;                             // copy the position packet to the TxFIFO

#ifdef WITH_ENCRYPT
      if(Parameters.Encrypt) TxPacket->Packet.Encrypt(Parameters.EncryptKey); // if encryption is requested then encrypt
                        else TxPacket->Packet.Whiten();                       // otherwise only whiten
#else // WITH_ENCRYPT
      TxPacket->Packet.Whiten();                                              // just whiten if there is no encryption
#endif // WITH_ENCRYPT
      TxPacket->calcFEC();                                                    // calculate FEC code
      bool FloatAcft = Parameters.AcftType==3 || ( Parameters.AcftType>=0xB && Parameters.AcftType<=0xD);  // heli, balloon or drone
      XorShift32(Random.RX);
      static uint8_t TxBackOff=0;
      if(TxBackOff) TxBackOff--;
      else
      { OGN_TxFIFO.Write();                                                // complete the write into the TxFIFO
        TxBackOff = 0;
        if(AverSpeed<10 && !FloatAcft) TxBackOff += 3+(Random.RX&0x1);
        if(Radio_TxCredit<=0) TxBackOff+=1; }
      Position->Sent=1;
#ifdef WITH_ADSL
      XorShift32(Random.RX);
      ADSL_Packet *AdslPacket=0;                                               // keep the pointer to the 
      { static uint8_t TxBackOff=0;
        if(TxBackOff) TxBackOff--;
        else if(Radio_FreqPlan.Plan<=1)                                         // ADS-L only in Europe/Africa
        { AdslPacket = ADSL_TxFIFO.getWrite();
          AdslPacket->Init();
          AdslPacket->setAddress (Parameters.Address);
          AdslPacket->setAddrTypeOGN(Parameters.AddrType);
          AdslPacket->setRelay(0);
          AdslPacket->setAcftTypeOGN(Parameters.AcftType);
          Position->Encode(*AdslPacket);                                       // encode position packet from the GPS
          AdslPacket->Scramble();
          AdslPacket->setCRC();
          ADSL_TxFIFO.Write();
          if(AverSpeed<10 && !FloatAcft) TxBackOff += 3+(Random.RX&0x1);       // if stationary then don't transmit position every second
          if(Radio_TxCredit<=0) TxBackOff+=1; }
      }
#endif
#ifdef WITH_FANET
      static uint8_t FNTbackOff=0;
      if(FNTbackOff) FNTbackOff--;
      else if(Parameters.TxFNT && Position->isValid() && Radio_FreqPlan.Plan<=1)
      { FANET_Packet *Packet = FNT_TxFIFO.getWrite();
        Packet->setAddress(Parameters.Address);
        Position->EncodeAirPos(*Packet, Parameters.AcftType, !Parameters.Stealth);
        XorShift32(Random.RX);
        FNT_TxFIFO.Write();
        FNTbackOff = 8+(Random.RX&0x1); }                                   // every 9 or 10sec
#endif // WITH_FANET
#ifdef WITH_PAW
      XorShift32(Random.RX);
      static uint8_t PAW_BackOff=0;
      if(PAW_BackOff) PAW_BackOff--;
      else if(Parameters.TxFNT && Position->isValid() && Radio_FreqPlan.Plan<=1 && FNT_TxFIFO.Full()==0)
      { PAW_Packet *TxPacket = PAW_TxFIFO.getWrite();                    // get place for a new PAW packet in the transmitter queue
        int Good=TxPacket->Read(PosPacket.Packet);                       // convert OGN position packet to PilotAware
// #ifdef WITH_ADSL
//         if(AdslPacket && (RX&10)) { TxPacket->Copy(&(AdslPacket->Version)); Good=1; }
// #endif
        if(Good)
        { PAW_TxFIFO.Write();                                            // complete the write into the transmitter queue
          PAW_BackOff = 3+Random.RX%3; }                                 // randomly choose time to transmit next PAW packet
      }
#endif

#ifdef WITH_LOOKOUT
      // process own position, get the most dangerous target
      const LookOut_Target *Tgt=Look.ProcessOwn(PosPacket.Packet, PosTime, Position->GeoidSeparation/10);
#ifdef WITH_PFLAA
      if(Parameters.Verbose)
      { xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
        Look.WritePFLA(CONS_UART_Write);                                  // produce PFLAU and PFLAA for all tracked targets
        xSemaphoreGive(CONS_Mutex);
#ifdef WITH_SDLOG
        if(Log_Free()>=512)
        { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
          Look.WritePFLA(Log_Write);
          xSemaphoreGive(Log_Mutex); }
#endif // WITH_SDLOG
      }
#else // WITH_PFLAA
      if(Parameters.Verbose)
      { uint8_t Len=Look.WritePFLAU(Line);                                // $PFLAU, overall status
        xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
        Format_String(CONS_UART_Write, Line, 0, Len);
        xSemaphoreGive(CONS_Mutex);
#ifdef WITH_SDLOG
        if(Log_Free()>=128)
        { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
          Format_String(Log_Write, Line, 0, Len);                                // send the NMEA out to the log file
          xSemaphoreGive(Log_Mutex); }
#endif // WITH_SDLOG
      }
#endif // WITH_PFLAA
      uint8_t Warn = 0;
      if(Tgt) Warn = Tgt->WarnLevel;                                       // what is the warning level ?
      if( (Warn>0) /* && (AverSpeed>=10) */ )                                    // if non-zero warning level and we seem to be moving
      { // int16_t RelBearing = Look.getRelBearing(Tgt);                      // relative bearing to the Target
        // int8_t Bearing = (12*(int32_t)RelBearing+0x8000)>>16;              // [-12..+12]
#ifdef WITH_BEEPER                                                         // make the sound according to the level
        if(Warn<=1)
        { if(KNOB_Tick>8)
          { Play(Play_Vol_1 | Play_Oct_1 | 4, 200); }
        }
        else if(Warn<=2)
        { if(KNOB_Tick>4)
          { Play(Play_Vol_3 | Play_Oct_1 | 8, 150); Play(Play_Oct_1 | 8, 150);
            Play(Play_Vol_3 | Play_Oct_1 | 8, 150); }
        }
        else if(Warn<=3)
        { if(KNOB_Tick>2)
          { Play(Play_Vol_3 | Play_Oct_1 |11, 100); Play(Play_Oct_1 |11, 100);
            Play(Play_Vol_3 | Play_Oct_1 |11, 100); Play(Play_Oct_1 |11, 100);
            Play(Play_Vol_3 | Play_Oct_1 |11, 100); }
        }

#endif // WITH_BEEPER
#ifdef WITH_SOUND
        Sound_TrafficWarn(Tgt);
#endif // WITH_SOUND
      }
#else  // WITH_LOOKOUT
#ifdef WITH_PFLAA
      if(Parameters.Verbose)
      { uint8_t Len=Look.WritePFLAU(Line);                                // $PFLAU, overall status
        xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
        Format_String(CONS_UART_Write, Line, 0, Len);
        xSemaphoreGive(CONS_Mutex);
#ifdef WITH_SDLOG
        if(Log_Free()>=128)
        { xSemaphoreTake(Log_Mutex, portMAX_DELAY);
          Format_String(Log_Write, Line, 0, Len);                                // send the NMEA out to the log file
          xSemaphoreGive(Log_Mutex); }
#endif // WITH_SDLOG
      }
#endif // WITH_PFLAA
#endif // WITH_LOOKOUT
#ifdef WITH_FLASHLOG
      bool Written=FlashLog_Process(PosPacket.Packet, PosTime);
      // if(Written)
      // { uint8_t Len=FlashLog_Print(Line);
      //   xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      //   Format_String(CONS_UART_Write, Line);
      //   xSemaphoreGive(CONS_Mutex);
      // }
#endif // WITH_FLASHLOG
      bool isSignif = OGN_isSignif(&(PosPacket.Packet), &PrevLoggedPacket);
      if(isSignif)
      {
#ifdef WITH_APRS
        APRStx_FIFO.Write(PosPacket);
#endif // WITH_APRS
#ifdef WITH_LOG
        FlashLog(&PosPacket, PosTime);
#endif // WITH_APRS
        PrevLoggedPacket = PosPacket.Packet;
      }
    } else // if GPS position is not complete, contains no valid position, etc.
    { if((SlotTime-PosTime)>=30) { PosPacket.Packet.Position.Time=0x3F; } // if no valid position for more than 30 seconds then set the time as unknown for the transmitted packet
      OGN_TxPacket<OGN_Packet> *TxPacket = OGN_TxFIFO.getWrite();
      TxPacket->Packet = PosPacket.Packet;                            // copy the position packet
      TxPacket->Packet.Whiten(); TxPacket->calcFEC();                 // whiten and calculate FEC code
#ifdef DEBUG_PRINT
      xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
      Format_UnsDec(CONS_UART_Write, PosTime);
      Format_String(CONS_UART_Write, " (_) TxFIFO <- ");
      Format_Hex(CONS_UART_Write, TxPacket->Packet.HeaderWord);
      CONS_UART_Write('\r'); CONS_UART_Write('\n');
      xSemaphoreGive(CONS_Mutex);
#endif // DEBUG_PRINT
      XorShift32(Random.RX);
      if(PosTime && ((Random.RX&0x7)==0) )                              // send if some position in the packet and at 1/8 normal rate
        OGN_TxFIFO.Write();                                              // complete the write into the TxFIFO
      if(Position) Position->Sent=1;
    }
#ifdef DEBUG_PRINT
    // char Line[128];
    Line[0]='0'+OGN_TxFIFO.Full(); Line[1]=' ';                  // print number of packets in the TxFIFO
    OGN_RelayQueue.Print(Line+2);                                   // dump the relay queue
    xSemaphoreTake(CONS_Mutex, portMAX_DELAY);
    Format_String(CONS_UART_Write, Line);
    xSemaphoreGive(CONS_Mutex);
#endif // DEBUG_PRINT

#ifdef WITH_FANET
    if(Parameters.Pilot[0] && (SlotTime&0xFF)==(Random.RX&0xFF) )              // every 256sec
    { FANET_Packet *FNTpkt = FNT_TxFIFO.getWrite();
      FNTpkt->setAddress(Parameters.Address);
      FNTpkt->setName(Parameters.Pilot);
      XorShift32(Random.RX);
      FNT_TxFIFO.Write(); }
#endif // WITH_FANET

    StatPacket.Packet.HeaderWord=0;
    StatPacket.Packet.Header.Address    = Parameters.Address;    // set address
    StatPacket.Packet.Header.AddrType   = Parameters.AddrType;   // address-type
    StatPacket.Packet.Header.NonPos=1;
    StatPacket.Packet.calcAddrParity();                          // parity of (part of) the header
    StatPacket.Packet.Status.Hardware=HARDWARE_ID;
    StatPacket.Packet.Status.Firmware=SOFTWARE_ID;

    static uint8_t StatTxBackOff = 16;
    ReadStatus(StatPacket.Packet);                               // read status data and put them into the StatPacket
    XorShift32(Random.RX);                                       // generate a new random number
    if( StatTxBackOff==0 && OGN_TxFIFO.Full()<2 )                 // decide whether to transmit the status/info packet
    { OGN_TxPacket<OGN_Packet> *StatusPacket = OGN_TxFIFO.getWrite(); // ask for space in the Tx queue
      uint8_t doTx=1;
      if(Parameters.AddrType && Random.RX&0x10)                  // decide to transmit info or status packet ?
      { doTx=ReadInfo(StatPacket.Packet); }                      // and overwrite the StatPacket with the Info data
      if(doTx)
      { StatTxBackOff=16+(Random.RX%15);
#ifdef WITH_APRS
        APRStx_FIFO.Write(StatPacket);
#endif // WITH_APRS
#ifdef WITH_LOG
        FlashLog(&StatPacket, PosTime);                         // log the status packet
#endif // WITH_LOG
       *StatusPacket = StatPacket;                               // copy status packet into the Tx queue
        StatusPacket->Packet.Whiten();                           // whiten for transmission
        StatusPacket->calcFEC();                                 // calc. the FEC code
        OGN_TxFIFO.Write();                                       // finalize write into the Tx queue
      }
    }
    if(StatTxBackOff) StatTxBackOff--;

    while(OGN_TxFIFO.Full()<2)                                   // any received OGN positions to be relayed ?
    { OGN_TxPacket<OGN_Packet> *RelayPacket = OGN_TxFIFO.getWrite();
      if(!GetRelayPacket(RelayPacket)) break;
      OGN_TxFIFO.Write(); }

#ifdef WITH_ADSL
    { static uint8_t StatTxBackOff = 16;
      static uint8_t StatTxPkt = 0;
      XorShift32(Random.RX);
      if(StatTxBackOff) StatTxBackOff--;
      else if(ADSL_TxFIFO.Full()<2 )                    // decide whether to transmit the status/info packet
      { ADSL_Packet *Packet = ADSL_TxFIFO.getWrite();
        if(StatTxPkt==0) getTelemStatus(*Packet, Position);
        else if(StatTxPkt==1) getTelemSatSNR(*Packet);
        else if(!getTelemSatPPS(*Packet)) getTelemSatSNR(*Packet);
        StatTxPkt++; if(StatTxPkt>=3) StatTxPkt=0;
        Packet->Scramble();
        Packet->setCRC();
        ADSL_TxFIFO.Write();
        StatTxBackOff = 10+Random.RX%5; }
    }
    while(ADSL_TxFIFO.Full()<2)                                  // any received ADS-L pasition to be relayed ?
    { ADSL_Packet *RelayPacket = ADSL_TxFIFO.getWrite();
      if(!GetRelayPacket(RelayPacket)) break;
      ADSL_TxFIFO.Write(); }
#endif

    CleanRelayQueue(SlotTime);

  }


}
