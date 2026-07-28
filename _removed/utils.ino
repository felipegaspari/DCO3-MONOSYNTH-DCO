// Removed unused code from utils.ino

// --- from original:4-22 ---

char * uintToStr( const uint64_t num, char *str )
{
  uint8_t i = 0;
  uint64_t n = num;
  
  do
    i++;
  while ( n /= 10 );
  
  str[i] = '\0';
  n = num;
 
  do
    str[--i] = ( n % 10 ) + '0';
  while ( n /= 10 );

  return str;
}

// --- from original:35-47 ---

uint16_t linearToExponential(uint16_t linearValue, float base, uint16_t maxValue) {

  if (linearValue < 0) linearValue = 0;
  if (linearValue > maxValue) linearValue = maxValue;

  float normalizedValue = (float)linearValue / (float)maxValue;
  float expValue = pow(base, normalizedValue) - 1;
  float maxExpValue = pow(base, 1.0) - 1;
  uint16_t scaledExpValue = (uint16_t)(expValue * ((float)maxValue / maxExpValue));

  return scaledExpValue;
}

// --- from original:48-53 ---

uint16_t faderExpConverter(uint16_t readingValue) {
  uint16_t pow3Calc = readingValue / 4;
  uint16_t expValOut = pow3Calc * pow3Calc * pow3Calc / 20000;
  return expValOut;
}

// --- from original:72-76 ---

uint16_t expConverterReverse(uint16_t readingValue, uint16_t curve) {
  uint16_t expValOut = sqrt((float)readingValue / curve);
  return expValOut;
}

// --- from original:77-81 ---

uint16_t expConverterFloatReverse(float readingValue, uint16_t curve) {
  uint16_t expValOut = sqrt(readingValue / curve);
  return expValOut;
}

// --- from original:82-87 ---

uint16_t expConverter2(uint16_t readingValue, uint16_t curve) {
  uint16_t pow3Calc = readingValue;
  uint16_t expValOut = pow3Calc * pow3Calc * pow3Calc / curve;
  return expValOut;
}

// --- from original:99-140 ---

void formula_update(byte formulaN) {
  // switch (formulaN) {
  //   case 1:
  //     if (VCFKeytrack > 0) {
  //       VCFKeytrackModifier = (float)VCFKeytrack / 8000;
  //     } else {
  //       VCFKeytrackModifier = 1;
  //     }
  //     break;
  //   case 2:
  //     ADSR2toVCF_formula = (float)1 / 512 * ADSR2toVCF;
  //     break;
  //   case 3:
  //     LFO1toVCF_formula = (float)1 / 512 * LFO1toVCF;
  //     break;
  //   case 4:
  //     LFO2toVCF_formula = (float)1 / 512 * LFO2toVCF;
  //     break;
  //   case 5:
  //     ADSR3toPWM_formula = (float)1 / 512 * ADSR3toPWM;
  //     break;
  //   case 6:
  //     LFO1toPWM_formula = (float)1 / 512 * LFO1toPWM;
  //     break;
  //   case 7:
  //     LFO1toVCA_formula = (float)1 / 512 * LFO1toVCA;
  //     break;
  //   case 8:
  //     ADSR1toVCA_formula = (float)1 / 512 * ADSR1toVCA;
  //     break;
  //   case 9:
  //     LFO1toDCO_formula = (float)1 / 1080000 * LFO1toDCO;
  //     break;
  //   case 10:
  //     ADSR3toDETUNE1_formula = (float)1 / 1080000 * ADSR3toDETUNE1;
  //     break;
  //   case 11:
  //   LFO2toPWM_formula = (float)1 / 512 * LFO2toPWM;
  //   break;
  // }
}

// --- from original:141-156 ---

void controls_formula_update(byte formulaN) {
  switch (formulaN) {
    case 1:
      LFO1Speed = expConverterFloat(LFO1SpeedVal, 5000);
      //LFO1_class.setMode0Freq(LFO1Speed, micros());
      break;
          case 2:
      LFO2Speed = expConverterFloat(LFO2SpeedVal, 5000);
      //LFO2_class.setMode0Freq(LFO2Speed, micros());
      break;
      case 3:
      LFO1toDCO = expConverterFloat(LFO1toDCOVal, 500);
      break;
  }
}

// --- from original:157-163 ---


void led_blinking_task() {
  if (millis() - LED_BLINK_START < 50)
    return;
  digitalWrite(LED_BUILTIN, LOW);
}
