/*
   Main frequency monitor
   Copyright (C) 2014 Paul Brook

   Released under the GNU GPL v3
 */
 
#include <SPI.h>

#define rck_pin 8
#define g_pin 9
#define clr_pin 10
#define servo_pin A1

static uint16_t last_capture;
static volatile uint16_t period;
static volatile uint8_t capture_head;

static int16_t servo_pos = 0;

static uint16_t blip;

static uint8_t digit_val[6];

// 60 readings * 5 seconds/reading = 5 minute rolling average
#define NUM_TOTALS 60

static uint32_t totals[NUM_TOTALS];
static uint8_t long_total;
static uint8_t valid_totals;

static const uint8_t cathode_mask[6] = 
{
  1<<4, 1<<5, 1<<0, 1<<1, 1<<2, 1<<3
};

static const uint8_t digit_mask[10] = {
  0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f,
};

static const uint8_t anode_pin[8] = {
    6, 5, 0, 3, 2, 7, 4, 1
};

ISR(TIMER1_CAPT_vect)
{
  uint16_t now;

  now = ICR1;
  period = now - last_capture;
  if (period < 10000)
    return;
  last_capture = now;
  capture_head++;
}

// Record a frequency measurement
// Cycles is the number of 2MHz clock ticks for 256 cycles
static void
record_freq(uint32_t ticks)
{
  uint32_t freq;
  int i;
  uint8_t r;

  totals[long_total] = ticks;
  long_total++;
  if (long_total > valid_totals)
    valid_totals = long_total;
  if (long_total == 16)
    long_total = 0;

  ticks = 0;
  for (i = 0; i < valid_totals; i++) {
      ticks += totals[i];
  }

  // Single precision floating point (24-bit mantissa) should have sufficient precision
  // We could do this in fixed point, but long division is hard.
  freq = (2000000.0f * 10000.0f * 256.0f * valid_totals) / ticks;

  servo_pos = 500000l - freq;
  for (i = 0; i < 6; i++) {
      r = freq % 10;
      freq /= 10;
      digit_val[i] = r;
  }
}

void setup()
{
  capture_head = 0xf0;
  blip = 10000;

  // Disable ADC
  ADCSRA = 0;
  // Enable analog comparator on A0 with sample triggering in Timer1
  ADCSRB = _BV(ACME);
#ifdef __AVR_ATmega32U4__
  ADMUX = 7;
#else
  ADMUX = 5;
#endif
  DIDR0 = _BV(ADC0D);
  ACSR = _BV(ACBG) | _BV(ACIC) | _BV(ACIS1);

  // 50Hz = 320k CPU cycles. A /8 divider allows us to capture with a 16-bit timer
  TCNT1 = 0;
  TCCR1A = 0;
  TCCR1B = _BV(ICNC1) | _BV(CS11);
  TIMSK1 = _BV(ICIE1);

  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV64);
  SPI.setDataMode(SPI_MODE0);
  SPI.begin();

  pinMode(g_pin, OUTPUT);
  digitalWrite(g_pin, 0);
  pinMode(clr_pin, OUTPUT);
  digitalWrite(clr_pin, 1);
  pinMode(rck_pin, OUTPUT);
  digitalWrite(rck_pin, 0);
  pinMode(servo_pin, OUTPUT);
  digitalWrite(servo_pin, 0);
}

static void
do_digit(uint8_t digit)
{
  uint8_t mask;
  uint8_t val;
  int n;

  for (n = 0; n < 8 ; n++)
    digitalWrite(anode_pin[n], 0);
  if (digit >= 6)
    return;
  SPI.transfer(cathode_mask[digit]);
  digitalWrite(rck_pin, 1);
  delayMicroseconds(1);
  digitalWrite(rck_pin, 0);
  mask = 1;
  if (blip > 1000)
      val = 0x40;
  else
    val = digit_mask[digit_val[digit]];
  if (digit == 4 && !blip)
    val |= 0x80;
  for (n = 0; n < 8; n++) {
      digitalWrite(anode_pin[n], (val & mask) != 0);
      mask <<= 1;
  }
}

ISR(TIMER2_COMPA_vect)
{
  digitalWrite(servo_pin, 0);
  TCCR2B = 0;
}

static void
start_servo(void)
{
  uint8_t ticks;

  // Normal mode
  TCCR2A = 0;
  TCNT2 = 0;
  TIMSK2 = _BV(OCIE2A);
  // A /256 prescale gives 62.5kHz timer tick = 16us/tick.
  // Center position = 1.5ms = 94 ticks
  ticks = 94;
  // We use a discontinuous scale, with 2.5 mHz/tick up to +-0.1Hz,
  // and 25mHz/tick up to +-0.5Hz
  // The crossover point corresponds to 57.6 degrees
  if (servo_pos >= -1000 && servo_pos <= 1000) {
      ticks += servo_pos / 25;
  } else if (servo_pos > 0) {
      if (servo_pos > 5000)
	  servo_pos = 5000;
      ticks += servo_pos / 250 + 36;
  } else {
      if (servo_pos < -5000)
	servo_pos = -5000;
      ticks += servo_pos / 250 - 36;
  }

  // TODO: Change this.
  OCR2A = ticks;
  cli();
  GTCCR = _BV(PSRASY);
  asm volatile("nop");
  TCCR2B = _BV(CS22) | _BV(CS21);
  digitalWrite(servo_pin, 1);
  sei();
}

void loop()
{
  uint8_t last_capture;
  uint32_t total;
  uint8_t digit;
  unsigned long last_display;
  unsigned long now;

  DDRD = 0xff;

  last_capture = 0;
  total = 0;
  while (capture_head != 0)
    /* no-op */;

  last_display = micros();
  digit = 0;
  while (true) {
      if (capture_head != last_capture) {
	  last_capture++;
	  total += period;
	  if (last_capture == 0) {
	      record_freq(total);
	      total = 0;
	      blip = 100;
	  }
	  start_servo();
      }
      now = micros();
      if (now - last_display > 1000) {
	  do_digit(digit);
	  if (blip)
	    blip--;
	  digit++;
	  if (digit == 8) {
	      digit = 0;
	  }
	  last_display = now;
      }
  }
}
