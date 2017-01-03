#include "lidar.h"


// For every degree, the following information is available
struct reading {
  uint16_t distance;        // Distance to object, in mm (if error, this is the error code)
  uint16_t signalStrength;  // Signal strength (higher = better)
  boolean isValid;          // True if the distance reading is valid
  boolean strengthWarning;  // True is there is a warning about signal strength (glass in the way?)
};

// Data from the LIDAR, containing data for 4 degrees of rotation
struct   {
  uint8_t start;            // The start bit of each packet is always 0xFA
  uint8_t index;            // The first degree this information is for
  uint8_t speedL;           // The LIDAR's rotation speed.  Rotation speed is controlled in a closed-loop
  uint8_t speedH; 
  uint32_t data[4];         // Data for each of the 4 degrees this packet is for
  uint16_t checksum;        // The checksum of the packet, to ensure it is not corrupt
} packet;

// Create a pointer to the memory location holding the packet data.
// This makes it easier to manipulate
uint8_t *ptr = (uint8_t *) &packet;


// Anything you put in setup() is only run once, at the beginning
void setup() {
  // Initialize the pins used to communicate with the RoboRIO
  pinMode(PIN_ENABLE, INPUT);
  pinMode(PIN_MOVE, OUTPUT);
  pinMode(PIN_TURN, OUTPUT);
  pinMode(PIN_FORWARD_CW, OUTPUT);
  pinMode(PIN_MOVE_RATE, OUTPUT);
  pinMode(PIN_FIRE, OUTPUT);
  
  // Always a good idea to stop the robot from moving as soon as possible
  moveRobot(MOVE_STOP, 0);
  
  // Handle the case where you plug the Arduino into the socket, when the robot is still enabled. You
  // definitely don't want it to start moving immediately. Also, if you ever screw-up your software to
  // the point where you can't communicate with the Arduino anymore this gives us a chance to recover it.
  if (digitalRead(PIN_ENABLE) == HIGH)
    delay(10000);
  
  // Start the serial communication with the LIDAR and debugging
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1);
  delay(500);
  
  // Initialize the timer that controls the LIDAR motor
  initializeTimer1();
  
  // Initialize the timer that controls robot speed
  initializeTimer3();
}


// Anything you put in loop() is run over and over again
void loop() {
  static uint8_t offset = 0;          // Offset into the packet being read
  static uint16_t distance[360];  // Array storing the distance for every degree
  struct reading degreeData;      // Structure holding data for each degree

  // Do nothing if there is no data to read
  if (!Serial1.available())
    return;

  // Read this character
  uint8_t c = (uint8_t) Serial1.read();

  // Packets must start with 0xFA
  if (offset == 0 && c != 0xFA)
    return;

  // Save this character
  ptr[offset++] = c;

  // End of this data packet?  If not, then wait for additional characters
  if (offset != 22)
    return;

  // Set offset to zero to be ready for the next packet
  offset = 0;
  
  // Verify the checksum.  This ensures the packet was received correctly from the LIDAR
  if (!checksumIsGood(ptr))
    return;
    
  // Now we get to take a look at the packet.  The format is given here:
  // https://xv11hacking.wikispaces.com/LIDAR+Sensor

  // The rotation speed is given in every packet.  Make sure the LIDAR RPM is good
  verifyMotorSpeed(processSpeed(ptr[2], ptr[3]));

  // Determine which degree of direction this data is for
  uint16_t degree = (packet.index - 0xA0) << 2;
  
  // Adjust this bearing because the LIDAR might not be mounted straight
  degree = (degree + DEGREE_OFFSET) % 360;

  // Each packet contains data for 4 degrees
  for (uint8_t i=0; i<4; i++, degree++) {
    // Convert the data to a format we can understand
    interpretData(packet.data[i], &degreeData);

    // Store the distance information
    // Adjust the angle so that points to the right are 0 degrees and increasing, and
    // points to the left are 359 degrees and decreasing.
    distance[359 - degree] = degreeData.distance;

    // Display some information every rotation
    if (degree == 0) {
      // Figure out where the closest object is
      uint32_t closestDistance = 0xFFFFFFFF;
      uint32_t closestAngle = 0;
        
      // Look at all the stored readings to figure out which one is closest
      for (uint16_t i=0; i< 360; i++) {
        // If the distance is very small then it is an error.  Ignore it
        if (distance[i] < 100)
          continue;
 
        // Is this reading further than the one we've found so far?
        if (distance[i] >= closestDistance)
          continue;
        // Yay!  This reading is the current closest :-)
        closestDistance = distance[i];
        closestAngle = i;
      }
        Serial.print("Closest point is ");
        Serial.print(closestDistance);
        Serial.print("mm away at ");
        Serial.print(closestAngle);
        Serial.println(" degrees");

      // Now that the closest object has been found try to face it and be 3 feet away from it
      // First priority is to face the closest object (the puppy's owner)
      // To prevent oscillation around 0 degrees we check for < 250 degrees and > 10 degrees
      // Turn counter-clockwise if the owner is on the left
      if (closestAngle < 345 && closestAngle > 180)
        moveRobot(MOVE_TURN_CCW, (360 - closestAngle) * 100 / 360);
      // Turn clockwise if the owner is on the right
      else if (closestAngle > 15 && closestAngle <= 180)
        moveRobot(MOVE_TURN_CLOCKWISE, (closestAngle) * 100 / 360);

      // If the puppy is facing its owner then move forward or backward until 3 feet away
      // We add an error margin to prevent oscillation around the 3' mark
      // 3'2" = 38 inches = 965mm
      else if (closestDistance > 965)
        moveRobot(MOVE_FORWARD, closestDistance / 100);
      // 2'10" = 34 inches = 864mm
      else if (closestDistance < 864)
        moveRobot(MOVE_BACKWARDS, (1000 - closestDistance) / 80);

      // And now the most important part!!  Make it stop!
      else
        moveRobot(MOVE_STOP, 0);
    } 
  } // End processing 4 degrees 
}


// Convert the received packet into something we can use
void interpretData(uint32_t data, struct reading *r) {
  uint8_t *c = (uint8_t *) &data;
  r->isValid = (c[1] & 0x80) == 0;
  r->strengthWarning = (c[1] & 0x40) == 0;
  r->distance = ((c[1] & 0x3F) << 8) + c[0];
  r->signalStrength = (c[3] << 8) + c[2];
}


// Get the revolutions per minute data from the packet
uint16_t processSpeed(uint8_t low, uint8_t high) {
  return  (high << 2) + (low >> 6);
}


// Verify the checksum
boolean checksumIsGood(byte* data)
{
  uint16_t dataList[10];
  for (int i=0; i<10; i++)
    dataList[i] = *data++ + (*data++ << 8);
  uint32_t chk32 = 0;
  for (int i=0; i<10; i++)
    chk32 = (chk32 << 1) + dataList[i];
  uint32_t checksum = (chk32 & 0x7FFF) + (chk32 >> 15);
  checksum &= 0x7FFF;
  if (checksum == *data++ + (*data << 8))
    return true;
  return false;
}


// Move the robot in the given direction
void moveRobot(uint8_t movement, uint8_t powerPercentage)
{
  // Stop the robot from moving.  It is important to do this first!
  digitalWrite(PIN_MOVE, LOW);
  setPowerLevel(0);
  
  // If the robot is not enabled then ignore this instruction
  if (digitalRead(PIN_ENABLE) == LOW)
    return;
  
  switch(movement) {
    case MOVE_STOP:
      // Do nothing!
      break;
    case  MOVE_FORWARD:
      digitalWrite(PIN_TURN, LOW);
      digitalWrite(PIN_FORWARD_CW, HIGH);
      digitalWrite(PIN_MOVE, HIGH);
      break;
    case  MOVE_BACKWARDS:
      digitalWrite(PIN_TURN, LOW);
      digitalWrite(PIN_FORWARD_CW, LOW);
      digitalWrite(PIN_MOVE, HIGH);
      break;
    case  MOVE_TURN_CLOCKWISE:
      digitalWrite(PIN_TURN, HIGH);
      digitalWrite(PIN_FORWARD_CW, HIGH);
      digitalWrite(PIN_MOVE, HIGH);
      break;
    case  MOVE_TURN_CCW:
      digitalWrite(PIN_TURN, HIGH);
      digitalWrite(PIN_FORWARD_CW, LOW);
      digitalWrite(PIN_MOVE, HIGH);
      break;
  }

  // Set the speed
  setPowerLevel(powerPercentage);
}



