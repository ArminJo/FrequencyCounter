/*
 *  FrequencyCounter.cpp
 *
 *  Counts frequencies up to 8 MHz with a gate time of 1 second.
 *
 *  Principle of operation:
 *  Input signal at Pin 4 is directly feed to the 8 bit counter of timer0.
 *  An interrupt is generated at every transition of the timer from 0xFF to 0x00.
 *  Then interrupt maintains a software counter for the signal frequency / 256.
 *  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *  !!! This disables the millis() function, which must be replaced by delayMilliseconds(unsigned int aMillis)  !!!
 *  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
 *
 *  Timer 1 generates an interrupt every second, which reads the timer0 8 bit counter,
 *  resets it and adds the value of the software counter.
 *
 *  Timer2 generates a 1 MHz signal at pin3 by hardware.
 *
 *  Special effort was taken to ensure that the 1 second interrupt is NOT delayed and therefore the gate timing is always exact.
 *  Thus, the handling of the transition of timer0 is changed to polling 0.5 ms before the 1 second interrupt.
 *  This allows to use the loop for other purposes the rest of the second :-).
 *
 *  Copyright (C) 2026  Armin Joachimsmeyer
 *  armin.joachimsmeyer@gmail.com
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *  See the GNU General Public License for more details.

 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <http://www.gnu.org/licenses/gpl.html>.
 *
 */

#include <Arduino.h>

#define VERSION_EXAMPLE "1.0"

#define USE_PARALLEL_1602_LCD
//#define USE_PARALLEL_2004_LCD
#include "LCDPrintUtils.hpp"
#include "PrintUtils.hpp"

#include "LiquidCrystal.h"
// select the pins used on the LCD panel
//LiquidCrystal myLCD(8, 9, 4, 5, 6, 7);
LiquidCrystal myLCD(12, 13, 8, 9, 10, 11); // Pins 8 to 123
char sStringBufferForLCDRow[LCD_COLUMNS + 1]; // For rendering LCD lines with snprintf_P()

//#define LOCAL_DEBUG // Must be after all includes

#define BURST_OUTPUT_PIN    6
#define TIMING_DEBUG_OUTPUT_PIN    7

volatile unsigned int sTimer0MatchCount = 0; // counts match with 128
volatile unsigned long sExternalClockCount = 0; // sTimer0OverflowCounter * 256 + current timer1 count
volatile bool sSecondHasPassed; //
volatile bool sTimer0InterruptIsDisabled;

void delayMilliseconds(unsigned int aMillis);

#if defined(LOCAL_DEBUG)
const char MatchPGM[] PROGMEM = "Match=";
PrintIfChanged<unsigned int> sMatchPrint(MatchPGM);
const char HzPGM[] PROGMEM = " Hz";
PrintIfChanged<unsigned long> sHzPrint(HzPGM);
void togglePin(uint8_t aPin, uint16_t aCount);
void outputTestBursts();
#endif

void setup() {

    pinMode(PD5, OUTPUT); //
    pinMode(BURST_OUTPUT_PIN, OUTPUT);
    pinMode(TIMING_DEBUG_OUTPUT_PIN, OUTPUT);

    /*
     * Set Timer0 for counting external pin input at 4 / PD4
     * This disables millis()!!!
     * Use Output Compare A Match Interrupt because Overflow interrupt is already occupied by millis()!
     * A match will set the Output Compare Flag (OCF0A or OCF0B) at the next timer clock cycle.
     */
#if defined(LOCAL_DEBUG)
    TCCR0A = _BV(COM0B0); // Normal mode, toggle Output 5 / PD5 / OC0B on every match / here overflow
    OCR0B = 0xFF;// To see Interrupt as output on PD5
#else
    TCCR0A = 0; // Normal mode.
#endif
    TCCR0B = _BV(CS02) | _BV(CS01) | _BV(CS00);     // External clock source on pin 4 / PD4 / T0 pin. Clock on rising edge.
    TIMSK0 = _BV(OCIE0A); // Output Compare A Match Interrupt Enable -> disable millis(), which uses TOIE0 (overflow)!!!
    OCR0A = 0xFF; // value 0xFF gives interrupt when switching to 0x00
    TCNT0 = 0;
    TIFR0 = 0xFF; // Clear all interrupt flags initially

    /*
     * Set Timer 1 for 1 s timing
     * It runs continuously in CTC mode with clock divider 1024 and OCR1A 15625 as TOP
     */
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS10) | _BV(CS12); // CTC with OCR1A and clock divider is 1024 -> 15.526 kHz / 64 us.
    // Set counter to a value which generates the first interrupt before the loop starts.
    // Thus the ISR delay of setting TCNT0 to 0 for first measurement is skipped.
    TCNT1 = 15625 - 2;
    TIFR1 = 0xFF; // Clear all flags initially
    TIMSK1 = _BV(OCIE1A) | _BV(OCIE1B); // Output Compare A + B Match Interrupt Enable
    OCR1A = (15625 - 1); // divider for 1 s
    OCR1B = (15625 - 8); // Interrupt 0.5 ms earlier to disable timer 0 interrupt

    /*
     * Set Timer2 for output 1 MHz on 3 / PD3 for external calibrating etc..
     */
    pinMode(3, OUTPUT);
    // Configure Timer2
    TCCR2A = _BV(COM2B1) | _BV(WGM21) | _BV(WGM20); // Fast PWM, toggle OC2B
    TCCR2B = _BV(WGM22) | _BV(CS20);                // Fast PWM, no prescaling
    OCR2A = 15;   // Top value for 1 MHz frequency at 16 MHz clock
    OCR2B = 7;   // 50% duty cycle (half of OCR2A)

// initialize the digital pin of the built in LED as an output.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    myLCD.begin(LCD_COLUMNS, LCD_ROWS);
    myLCD.print(F("FrequencyCounter"));

    Serial.begin(115200);
    // Just to know which program is running on my Arduino
    Serial.println(F("START " __FILE__ "\r\nVersion " VERSION_EXAMPLE " from " __DATE__));
    Serial.println(F("Only tested for AVR328 CPU!"));
    Serial.println(F("1 MHz Output at 3 / PD3"));
    Serial.println(F("Frequency input at pin 4 / PD4"));
    Serial.flush();

    sSecondHasPassed = false; // Skip first value, which is too small.

}

void loop() {
    if (sTimer0InterruptIsDisabled) {
        /*
         * Check and process interrupt by polling
         * This avoids eventually blocking the 1 second interrupt by the 2.2 us TIMER0_COMPA_vect ISR
         */
        noInterrupts();
        if (TIFR0 & _BV(OCF0A)) {
            sTimer0MatchCount++; // interrupt was pending, process it here
            TIFR0 = 0xFF; // reset all pending Timer0 interrupts
        }
        interrupts();
    } else {
        //    sMatchPrint.printWithLeadingText(sTimer0MatchCount);
        if (sSecondHasPassed) {
            sSecondHasPassed = false;
            snprintf_P(sStringBufferForLCDRow, sizeof(sStringBufferForLCDRow), PSTR("%7lu Hz "), sExternalClockCount);
            myLCD.setCursor(0, 1);
            myLCD.print(sStringBufferForLCDRow);

#if defined(LOCAL_DEBUG)
                Serial.print(sExternalClockCount);
                Serial.println(F(" Hz"));
                outputTestBursts();
        #endif
        }
    }
}

/*
 * Timer0 overflow ISR called at counter transition from 0xFF to 0x00
 * 2.2 us @16 MHz. ISR has 36 clock cycles including RETI
 */
ISR(TIMER0_COMPA_vect) {
    sTimer0MatchCount++;
}

/*
 * Called by 1 second Output Compare A Match Interrupt
 */
ISR(TIMER1_COMPA_vect) {

    uint8_t tTIFR0 = TIFR0; // Copy interrupt flags of timer0
    uint8_t tTCNT0 = TCNT0;
    TCNT0 = 0; // Start again
    TIFR0 = 0xFF; // reset all pending Timer0 interrupts
    auto tTimer0MatchCount = sTimer0MatchCount;
    sTimer0MatchCount = 0;
    // End of time critical section

    /*
     * Compute frequency
     */
    if (tTIFR0 & _BV(OCF0A)) {
        tTimer0MatchCount++; // interrupt was pending, process it here
    }
    sExternalClockCount = ((unsigned long) tTimer0MatchCount << 8) + tTCNT0;
    sSecondHasPassed = true;

    /*
     * Enable TIMER0_COMPA_vect
     */
    TIMSK0 = _BV(OCIE0A); // Output Compare A Match Interrupt Enable
    sTimer0InterruptIsDisabled = false;
}

ISR(TIMER1_COMPB_vect) {
    /*
     * Disable TIMER0_COMPA_vect
     * This happens 1 ms before the 1 second interrupt
     */
    TIMSK0 = 0; // Disable all timer0 interrupts
    sTimer0InterruptIsDisabled = true;
}

/*
 * Generate 100 kHz burst for testing
 */
void togglePin(uint8_t aPin, uint16_t aCount) {
    for (uint16_t i = 0; i < aCount; ++i) {
        digitalWrite(aPin, HIGH);
        delayMicroseconds(6);
        digitalWrite(aPin, LOW);
        delayMicroseconds(6);
    }
}

void outputTestBursts() {
    static uint8_t sState = 0;
    auto tState = sState;
    if (--tState == 0) {
        // sState == 1
        togglePin(BURST_OUTPUT_PIN, 2);
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 255); // 0
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 256); // 0
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 257);
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 511); // -256
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 512); // -256
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 513);
    } else if (--tState == 0) {
        togglePin(BURST_OUTPUT_PIN, 12345);
    } else {
        sState = 0; // NO burst output here
    }
    sState++;
}

void delayMilliseconds(unsigned int aMillis) {
    for (unsigned int i = 0; i < aMillis; ++i) {
        delayMicroseconds(1000);
    }
}
