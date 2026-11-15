int glassDistance = 0;      // wird in Bar_1.95.ino ausgewertet (< minGlassDistance = Glas)
int messurment = 0;         // Rohwert vom Sensor (Pulsbreite in µs)
int PWM_PIN = 35;
void workLoad (void);

// Hysterese: unter GLASS_ON = Glas, über GLASS_OFF = kein Glas, dazwischen Zustand halten
const int GLASS_ON    = 760;
const int GLASS_OFF   = 800;
const int GLASS_COUNT = 3;   // so viele Messungen in Folge, bevor umgeschaltet wird (à 100 ms)

void Task1( void * parameter ) {
   pinMode(PWM_PIN, INPUT);
   int onCnt = 0;
   int offCnt = 0;

   for (;;) {
     if (distanceSensor) {
          messurment = pulseIn(PWM_PIN, HIGH);          // Standard-Timeout 1 s (wie Original)

          if (debug && debugLoop) {
              Serial.printf("raw: %d\n", messurment);
          }

          if (messurment > 0 && messurment < GLASS_ON) {
              offCnt = 0;
              if (onCnt < GLASS_COUNT) onCnt++;
              if (onCnt >= GLASS_COUNT) glassDistance = messurment;      // Glas erkannt
          } else if (messurment == 0 || messurment > GLASS_OFF) {
              onCnt = 0;
              if (offCnt < GLASS_COUNT) offCnt++;
              if (offCnt >= GLASS_COUNT) glassDistance = GLASS_OFF + 1;  // kein Glas
          } else {
              onCnt = 0;                                                 // Totband 760..800:
              offCnt = 0;                                                // Zustand bleibt
          }
      } else {
         glassDistance = 200;
      }
      delay(100);
    }
}
