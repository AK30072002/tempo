#include <avr/io.h>
#include <avr/interrupt.h>

volatile uint8_t counter = 0;

static inline uint8_t to_gray(uint8_t n) { return n ^ (n >> 1); }

static inline void show(uint8_t g)
{
    PORTD = (uint8_t)((PORTD & 0x03) | (uint8_t)((g & 0x3F) << 2));
    PORTB = (uint8_t)((PORTB & 0xFC) | ((g >> 6) & 0x03));
}

ISR(TIMER1_COMPA_vect)
{
    counter++;
    show(to_gray(counter));
}

void setup()
{
    Serial.begin(115200);

    DDRD |= 0b11111100;
    DDRB |= 0b00000011;
    pinMode(10, INPUT);

    show(0xFF);
    delay(1000);
    show(0x00);
    delay(500);

    noInterrupts();
    TCCR1A = 0;
    TCCR1B = 0;
    TCNT1  = 0;
    OCR1A  = 1999;
    TCCR1B |= (1 << WGM12);
    TCCR1B |= (1 << CS11);
    TIMSK1 |= (1 << OCIE1A);
    TIMSK0 = 0;
    interrupts();

    Serial.println(F("GRAY COUNTER OK: 8 bits on D2..D9, 1 ms/step, 256 ms wrap"));
}

void loop()
{
}
