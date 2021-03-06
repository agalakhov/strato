/// Stratosphere sensor board

//libraries are needed to access the sensors easier 
#include <TinyGPS.h>    // https://www.instructables.com/id/How-to-Communicate-Neo-6M-GPS-to-Arduino/
#include <Wire.h>       // Standart
#include <SPI.h>        // Standart
// Requires "Adafruit Unified Sensors Library"
#include <Adafruit_BME280.h>    // https://github.com/adafruit/Adafruit_BME280_Library
#include <SD.h>         // Standard
#include <MPU9250.h>    // https://github.com/bolderflight/MPU9250


#define LED0 PB0
#define LED1 PB1
#define gpsSerial Serial1
char filename[16];
TinyGPS gpsDecoder;
Adafruit_BME280 BME0; 
Adafruit_BME280 BME1;
MPU9250 IMU(Wire,0x68);

static void gpsdump(File& fd);
static void printValues();
static void setGPSFlightMode();

// Arduino tool does not generate forward declarations of template functions properly.
// An explicit forward declaration preceding all executable code stops Arduino from these attempts.
template <typename T> static void printCSV(File& fd, T x);
template <typename Out> static void printFloat(Out& out, double number, uint8_t digits);

/// Circular bufer for incoming GPS data.
class CircularBuffer {
public:
    CircularBuffer() : read_pointer(0), write_pointer(0), newline_count(0) { }
    /// Push byte into circular buffer. Usually called from interrupt handler.
    inline void push(char c) {
        this->buffer[this->write_pointer] = c;
        this->write_pointer = (this->write_pointer + 1) % MAX_SIZE;
        if (c == '\n') {
            noInterrupts();
            ++this->newline_count;
            interrupts();
        }
    }
    /// Check if the circular buffer contains whole lines.
    inline bool has_lines() const {
        noInterrupts();
        bool x = this->newline_count > 0;
        interrupts();
        return x;
    }
    /// Read one byte.
    inline char pop() {
        char c = this->buffer[this->read_pointer];
        this->read_pointer = (this->read_pointer + 1) % MAX_SIZE;
        if (c == '\n') {
            noInterrupts();
            --this->newline_count;
            interrupts();
        }
        return c;
    }
private:
    static const uint16_t MAX_SIZE = 4096; // must be big, or else we may lose data
    uint16_t read_pointer;
    uint16_t write_pointer;
    volatile uint8_t newline_count;
    char buffer[MAX_SIZE];
};

static CircularBuffer gps_buffer;

/// Timer interrupt hacking.
// We use 8-bit Timer2 of ATMEGA1284.
// See datasheet, pp. 140-160
static void setupTimerInterrupt() {
    noInterrupts();
    // Register-level configuration of TIMER2
    TCCR2A = 0x00; // Normal mode
    TCCR2B = 0x05; // Normal mode, 1:128 prescaler. This corresponds to approx. 500 events/sec.
                   // See Table 15-9 in the datasheet for possible values.
    // Unmask timer interrupt.
    TIMSK2 = 0x01;
    interrupts();
}

/// Timer interrupt function.
// This function is called periodically approx. 500 times per second.
// Keep it short. No I/O here!!! 
//
// The internal Arduino circular bufer has 64 bytes.
// At 9600 baud this would overflow in a 1/150 second.
// Here we pull data approx. every 1/500 second into a larger buffer
// protecting the internal one from overflow.
ISR(TIMER2_OVF_vect) {
    while (gpsSerial.available()) {
        gps_buffer.push(gpsSerial.read());
    }
}

/// Write a number to CSV file.
template <typename T> static void printCSV(File& fd, T x) {
    fd.print(x);
    fd.print(";");
}

/// Write a floating-point number to CSV file with given precision.
static void printCSV(File& fd, float x, int digits) {
    printFloat(fd, x, digits);
    fd.print(";");
}

/// Write a floating-point number with correct rounding to arbitrary output.
template <typename Out> static void printFloat(Out& out, double number, uint8_t digits) {
    // Handle negative numbers
    if (number < 0.0) {
        out.print('-');
        number = - number;
    }

    // Round correctly so that print(1.999, 2) prints as "2.00"
    {
        double rounding = 0.5;
        for (uint8_t i = 0; i < digits; ++i) {
            rounding /= 10.0;
        }
        number += rounding;
    }

    // Extract the integer part of the number and print it
    uint32_t int_part = (uint32_t) number;
    double remainder = number - (double) int_part;

    out.print(int_part);
    if (digits > 0) {  
        out.print("."); 
    }

    // Extract digits from the remainder one at a time
    while (digits-- > 0) {
        remainder *= 10.0;
        uint8_t digit = (uint8_t) remainder;
        remainder -= digit;
        out.print(digit);  
    }
}

/// Create new file and get its number
static void generateFilename() {
    unsigned n = 0;
    do {
        // include a three-digit sequence number in the file name
        snprintf(filename, sizeof(filename), "data%03d.csv", n);
        ++n;
    }
    while(SD.exists(filename));
    Serial.print("Writing to ");
    Serial.println(filename);
}

/// Get time since startup in ms
static void Runtime(File& fd) {
    unsigned long runtime = millis();
    Serial.print("Runtime = ");
    Serial.println(runtime); 
    fd.print(runtime);
    fd.print(";");
}

/// Halt the system if something goes wrong
static void hang() {
    Serial.println("System halted.");
    LED0_on();
    LED1_on();
    while (true);
}

/// Activate LED 0
static void LED0_on() {
    digitalWrite(LED0, 0);  
}

/// Activate LED 1
static void LED1_on() {
    digitalWrite(LED1, 0);
}

/// Deactivate LED 0
static void LED0_off() {
    digitalWrite(LED0, 1);
}

/// Deacticate LED 1
static void LED1_off() {
    digitalWrite(LED1, 1);
}


/// Output value from the BME sensor to serial console
static void printBMEValue(uint8_t bmeIdx, const char *name, float value, const char *unit) {
    Serial.print(name);
    Serial.print("_BME_");
    Serial.print(bmeIdx);
    Serial.print(" = ");
    Serial.print(value);
    Serial.print(" ");
    Serial.println(unit);  
}

/// Output value from the BME sensor to CSV file
static void printBME(File& fd, Adafruit_BME280& bme, uint8_t bmeIdx) {
    float temperature = bme.readTemperature();
    float pressure = bme.readPressure() / 100.0f;
    float humidity = bme.readHumidity();

    printBMEValue(bmeIdx, "Temperature", temperature, "*C");
    printBMEValue(bmeIdx, "Pressure", pressure, "hPa");
    printBMEValue(bmeIdx, "Humidity", humidity, "%");

    printCSV(fd, temperature);
    printCSV(fd, pressure);
    printCSV(fd, humidity); 
}

/// Output value from the IMU to the serial console
static void printIMUValue(const char* name, float value, const char *unit) {
    Serial.print(name);
    Serial.print(" ");
    Serial.print("IMU");
    Serial.print(" = ");
    Serial.print(value);
    Serial.print(" ");
    Serial.println(unit);  
}

/// Read the IMU sensor and output the data to CSV and console
static void printIMU(File& fd, MPU9250& IMU) {
    IMU.readSensor();
    float ax = IMU.getAccelX_mss();
    float ay = IMU.getAccelY_mss();
    float az = IMU.getAccelZ_mss();

    float rx = IMU.getGyroX_rads();
    float ry = IMU.getGyroY_rads();
    float rz = IMU.getGyroZ_rads();

    float bx = IMU.getMagX_uT();
    float by = IMU.getMagY_uT();
    float bz = IMU.getMagZ_uT();

    printIMUValue("AccelX", ax, "mss");
    printIMUValue("AccelY", ay, "mss");
    printIMUValue("AccelZ", az, "mss");

    printIMUValue("GyroX", rx, "rads");    
    printIMUValue("GyroY", ry, "rads");
    printIMUValue("GyroZ", rz, "rads");

    printIMUValue("MagX", bx, "uT");
    printIMUValue("MagY", by, "uT");
    printIMUValue("MagZ", bz, "uT");

    printCSV(fd, ax);
    printCSV(fd, ay);
    printCSV(fd, az);

    printCSV(fd, rx);
    printCSV(fd, ry);
    printCSV(fd, rz);

    printCSV(fd, bx);
    printCSV(fd, by);
    printCSV(fd, bz); 
}

static bool pollGPS(const char* filename) {
    const unsigned long timeout = 5000; // milliseconds
    bool newdata = false;

    unsigned long start = millis();
    while (millis() - start < timeout) {
        if (gps_buffer.has_lines()) {
            // We reopen file every line so that the file won't get corrupted
            // if we can't write
            File fd = SD.open(filename, FILE_WRITE);
            while (true) { // repeat until end of line
                char c = gps_buffer.pop();

                if (gpsDecoder.encode(c)) {
                    newdata = true;
                }

                Serial.print(c);
                fd.print(c);

                if (c == '\n') {
                    break; // end of line
                }
            }
            fd.close();
        }
        if (newdata) {
            break;
        }
    }

    return newdata;
}

static void gpsdump(File& fd) {
    unsigned long age = 0;
    int year = 0;
    byte month = 0;
    byte day = 0;
    byte hour = 0;
    byte minute = 0;
    byte second = 0;
    byte hundredths = 0;
    gpsDecoder.crack_datetime(&year, &month, &day, &hour, &minute, &second, &hundredths, &age);

    float flat = 0;
    float flon = 0;
    gpsDecoder.f_get_position(&flat, &flon, &age);

    float altitude = gpsDecoder.f_altitude();
    float speed = gpsDecoder.f_speed_kmph(); 
    unsigned satellites = gpsDecoder.satellites();

    char date[11] = { 0 };
    sprintf(date, "%04d/%02d/%02d", year, month, day);
    char time[12] = { 0 };
    sprintf(time, "%02d:%02d:%02d.%02d",
        (hour), /* UTC */
        minute, second, hundredths
    ); 
    // prints the data on the Serial Monitor to check the data 
    Serial.print("Date: ");
    Serial.println(date);
    Serial.print("Time: ");
    Serial.println(time);

    Serial.print("Lat/Long(float): ");
    printFloat(Serial, flat, 5);
    Serial.print(", ");
    printFloat(Serial, flon, 5);
    Serial.print(" Alt(float): ");
    printFloat(Serial, altitude, 2);
    Serial.print(" kmph: ");
    printFloat(Serial, speed, 2);
    Serial.print(" Satellites: ");
    if (satellites != TinyGPS::GPS_INVALID_SATELLITES) { 
        Serial.println(satellites);
    } else {
        Serial.println("unknown");
    }
    // prints the IMU data
    printCSV(fd, date);
    printCSV(fd, time);
    printCSV(fd, flat, 5);
    printCSV(fd, flon, 5);  
    printCSV(fd, altitude, 2);
    printCSV(fd, speed, 2);
    fd.print(satellites);
}


/// Internal function of flight mode setting
static void ubxFinalize(byte *data, uint8_t size) {
    data[0] = 0xB5;
    data[1] = 0x62;
    data[4] = size - 8;

    byte ck1 = 0;
    byte ck2 = 0;
    for (uint8_t i = 2; i < size - 2; ++i) {
        ck1 += data[i];
        ck2 += ck1;
    }

    data[size - 2] = ck1;
    data[size - 1] = ck2;
}

/// Internal function of flight mode setting
static void ubxIO(byte *data, uint8_t size) {
    ubxFinalize(data, size);
    byte ack[] = {
        0xB5, 0x62,
        0x05, 0x01, // ack
        2, 0,
        data[2], data[3],
        0, 0
    };

    ubxFinalize(ack, sizeof(ack));

    for (uint8_t attempt = 0; attempt < 10; ++attempt) {
        gpsSerial.flush();
        gpsSerial.write(0xFF);
        delay(500);
        gpsSerial.write(data, size);

        unsigned long start = millis();
        uint8_t pos = 0;
        const unsigned long TIMEOUT = 3000;
        while (millis() - start < TIMEOUT) {
            if (gpsSerial.available()) {
                byte b = gpsSerial.read();
                if (b == ack[pos]) {
                    ++pos;
                    if (pos >= sizeof(ack))
                        return;
                } else {
                    pos = 0;
                }
            }
        }
    }
    Serial.println("Init error - No reply from GPS module");
    hang();
}

/// Set flight mode to "stratosphere"
static void setGPSFlightMode() {
    byte ubx[] = {
        0xB5, 0x62,
        0x06, // CFG 
        0x24, // CFG-NAV5, see page 119 of the datasheet 
        36, 0, // size etc.
        0xFF, 0xFF, // set all

        6, // Airborne with <1g acceleration
        3, // Auto 2D/3D
        0, 0, 0, 0, // fixed altitude for 2D mode, meter * 0.01
        0x10, 0x27, 0x00, 0x00, // 0x00002710 fixed altitude variance, meter * 0.0001
        5, // Minimum satellite elevation, degrees
        0, // Maximum time to perform dead reckoning
        0xFA, 0x00, // 0x00FA Position DOP mask
        0xFA, 0x00, // 0x00FA Time DOP mask
        0x64, 0x00, // 0x0064 Position accuracy mask
        0x2C, 0x01, // 0x012C Time accuracy mask
        0, // Static hold threshold, cm/s
        0, // DGPS timeout, seconds  

        // reserved, always 0
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    ubxIO(ubx, sizeof(ubx));
    Serial.println("GPS switched to flight mode");
}

/// Initialization
void setup() {
    pinMode (LED0, OUTPUT);
    pinMode (LED1, OUTPUT);

    Serial.begin(115200);
    gpsSerial.begin(9600);
    SD.begin(PB4);

    Serial.println(F("BME280 test"));
    generateFilename();

    {
        File fd = SD.open(filename, FILE_WRITE);
        fd.println(
            "T_BME280_0_C;p_BME280_0_hPa;rH_BME280_0_%;"
            "T_BME280_1_C;p_BME280_1_hPa;rH_BME280_1_%;"
            "AccelX_mss;AccelY_mss;AccelZ_mss;"
            "GyroX_rads;GyroY_rads;GyroZ_rads;"
            "MagX_uT;MagY_uT;MagZ_uT;"
            "millis_ms;"
            "Jahr/Monat/Tag;Stunden:Minuten:Sekunden;"
            "f_lat;f_lon;f_altitude;f_kmph;Sateliten"
        );
        fd.close();
    }

    {
        bool bme0Status = BME0.begin(); //0x76
        bool bme1Status = BME1.begin(0x77);
        int statusIMU = IMU.begin();

        // Show BMEs on the serial monitor
        if (!bme0Status) {
            Serial.println("Could not find a valid BME280 Nr. 0 sensor, check wiring!");
            hang();
        }
        if (!bme1Status) {
            Serial.println("Could not find a valid BME280 Nr. 1 sensor, check wiring!");
            hang();
        }
        if (statusIMU < 0) {
            Serial.println("IMU initialization unsuccessful");
            hang();
        }
    }

    setGPSFlightMode();
    setupTimerInterrupt(); // this must be AFTER setGpsFlightMode()
                           // since we do GPS buffering within the interrupt handler
    Serial.println("-- Default Test --");
}


/// Main loop
void loop() {
    LED0_on();
    bool newdata = pollGPS(filename);
    LED0_off();

    File fd = SD.open(filename, FILE_WRITE);
    printBME(fd, BME0, 0);
    printBME(fd, BME1, 1);
    printIMU(fd, IMU);
    Runtime(fd);

    if (newdata) {
        LED1_on();
        Serial.println("Acquired GPS Data");
        Serial.println("-------------");
        gpsdump(fd);
        Serial.println("-------------");
        Serial.println();
    } else {
        Serial.println("No GPS data available");
    }

    fd.println("");
    fd.close();
    LED1_off();
} 
