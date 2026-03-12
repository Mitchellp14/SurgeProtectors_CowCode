// loadCells.h
// ADC / Load cell interface
// configures ADC channel, reads that channel 
// Compiles a overall sum of readings for output from either scale

#pragma once
#include <Arduino.h>

class LoadCell_Tasks {
public:
  //enables and defines spi communication (may need to input spi pointer/s instead)
  void begin_ADC(int arr_cs[], int Dout1, int Din1, int Dout2, int Din2); 

  //configures adc1 grabs measurment reconfigures grabs second measurment 
  //configures adc2 grabs measurment reconfigures grabs second measurment
  //Sums outputs and converts to weight
  float ADC_Sum_Reading(int cs1, int cs2, (spi pointer or din / dout), int arr_cv[]);

private:

  //configures adc 
void Config_ADC(...);




};
