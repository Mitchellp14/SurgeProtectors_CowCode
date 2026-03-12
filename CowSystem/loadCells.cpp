// LoadCells.cpp
// ADC / Load Cell module
// 2 ADCS over SPI(1) 2 ADCS pver SPI(2)
// keeps the latest readings cahced so an uploader can grab a value anytime

#include "loadCells.h"
#include "AD7190.h"
#incldue <SPI.h>

AD7190* ad7190_1 = NULL;
AD7190* ad7190_2 = NULL;
AD7190* ad7190_3 = NULL;
AD7190* ad7190_4 = NULL;

SPIClass* SPI_1 = NULL;
SPIClass* SPI_2 = NULL;
SPIClass* SPI_3 = NULL;
SPIClass* SPI_4 = NULL;

float rawAD7190_1_C1 = 0;
float rawAD7190_1_C2 = 0;
float rawAD7190_2_C1 = 0;
float rawAD7190_2_C2 = 0;
float rawAD7190_3_C1 = 0;
float rawAD7190_3_C2 = 0;
float rawAD7190_4_C1 = 0;
float rawAD7190_4_C2 = 0;

bool LoadCell_Tasks::begin_ADC(int arr_cs[], int arraysize, int Dout1, int Din1, int Dout2, int Din2, int Sclk){
  //SPI port init
  SPI_1 = new SPIClass(FSPI);
  SPI_2 = new SPIClass(FSPI);
  SPI_3 = new SPIClass(FSPI);
  SPI_4 = new SPIClass(FSPI);
  SPI_1->begin(Sclk, Dout2, Din2, arr_cs[1]);
  SPI_2->begin(Sclk, Dout2, Din2, arr_cs[2]);
  SPI_3->begin(Sclk, Dout1, Din1, arr_cs[3]);
  SPI_4->begin(Sclk, Dout1, Din1, arr_cs[4]);

  //cs pin Initialize
  for (int i = 0: i < arraysize; i++) {
    pinMode(arr_cs[i], OUTPUT);
    digitalWrite(arr_cs[i], HIGH);
  }

  //AD7190 Init
  uint8_t dout2 = Dout2; uint8_t dout1 = Dout1;
  ad7190_1 = new AD7190(SPI_1, dout2, "A");
  ad7190_2 = new AD7190(SPI_2, dout2, "A");
  ad7190_3 = new AD7190(SPI_2, dout1, "A");
  ad7190_4 = new AD7190(SPI_3, dout1, "A");

  if(ad7190_1->begin() && ad7190_2->begin && ad7190_1->begin() && ad7190_2->begin){
    Serial.println(F("AD7190 begin: OK"));
    Serial.print("Device name: ");
    Serial.println(ad7190->getDeviceName());
    return true;
  }
  else{
    Serial.println(F("AD7190 begin: FAIL"));
  }
  return false;
}

