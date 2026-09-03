int glassDistance = 0;
int messurment = 0;
int aux = 0;
int PWM_PIN = 35;
void workLoad (void);     

void Task1( void * parameter ) {
   // Everything defined on the setup
   pinMode(PWM_PIN, INPUT);
   for (;;) {
     if (distanceSensor) {     
          // Everything defined on the loop
          messurment = pulseIn(PWM_PIN, HIGH);
          if ((messurment < minGlassDistance && messurment > 0) || (messurment > minGlassDistance && aux > 1)) {
            glassDistance = messurment;
            aux = 0;    
          } else if (glassDistance > 0) {
            aux++;
          }
      } else {
         glassDistance = 200;  
      }      
      delay(100);
    }
}
