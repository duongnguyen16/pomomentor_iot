#ifndef CHARACTER_HELPER_H
#define CHARACTER_HELPER_H

#include <LiquidCrystal_I2C.h>

// Custom character for phone icon (for focus mode)
byte phoneIcon[8] = {
  B00100,
  B01110,
  B10101,
  B10101,
  B10101,
  B10101,
  B01110,
  B00100
};

// Custom character for clock icon
byte clockIcon[8] = {
  B00000,
  B01110,
  B10101,
  B10111,
  B10001,
  B01110,
  B00000,
  B00000
};

// Initialize custom characters in LCD
void initCustomCharacters(LiquidCrystal_I2C &lcd) {
  // Create phone icon at position 0
  lcd.createChar(0, phoneIcon);
  
  // Create clock icon at position 1
  lcd.createChar(1, clockIcon);
}

#endif // CHARACTER_HELPER_H
