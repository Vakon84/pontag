// PS/2 protocol implementation
//
// This implementation is entirely interrupt-driven so all comms happens in background.
// Clock is tied to INT0 pin and events are handled in INT0 ISR handler.
//
// Events not triggered by clock (end of transmission, transmission request, watchdog,
// error recovery) use Timer0. Watch out how state changes in different handlers.
//
// Original code taken from here: https://github.com/svofski/mouse1351
//

#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>

#include <stdio.h>

#include "ioconfig.h"

#include "ps2.h"

// Read PS2 data into bit 7
#define ps2_datin() ((PS2PIN & _BV(PS2DAT)) ? 0x80 : 0x00)

// Read PS2 clk into bit 7
#define ps2_clkin() ((PS2PIN & _BV(PS2CLK)) ? 0x80 : 0x00)


static volatile uint8_t state;                  // PS2 protocol state

static volatile uint8_t recv_byte;              // Byte being received
static volatile uint8_t rx_head;                // Buffer head offset
static volatile uint8_t rx_tail;                // Buffer tail offset
static volatile uint8_t rx_buf[PS2_RXBUF_LEN];  // Receive buffer

static volatile uint8_t tx_byte;                // Byte being transmitted

// internals for tx/rx bitbanging
static volatile uint8_t bits = 0;
static volatile uint8_t parity;

static volatile uint8_t waitcnt = 0;

// PS2 protocol states
enum _state {
    IDLE = 0,           // Idle waiting
    RX_DATA,            // Receiving data
    RX_PARITY,          // Receiving parity bit
    RX_STOP,            // Receiving stop bit

    TX_REQ0,            // Requesting to send
    TX_DATA,            // Transmitting data
    TX_PARITY,          // Transmitting parity bit
    TX_STOP,            // Transmitting stop bit
    TX_ACK,             // Waiting ACK
    TX_END,             // Waiting for TX end

    ERROR = 255         // Error state
};

void ps2_dir(uint8_t dat_in,uint8_t clk_in);      // Set buses direction (1 == in)
void ps2_clk(uint8_t c);              // Set clk
void ps2_dat(uint8_t d);              // Set dat

uint8_t ps2_busy(void) {
    return state != IDLE;
}

void ps2_init(void) {
    state = IDLE;
    rx_head = 0;
    rx_tail = 0;
    ps2_enable_recv(0);

    // Toggle INT0 at the falling edge
#if defined (__AVR_ATmega8A__)
    MCUCR |= _BV(ISC01);
#elif defined (__AVR_ATmega328P__)
    EICRA |= _BV(ISC01);
#endif

    // Disable the timer 0 interrupts
#if defined (__AVR_ATmega8A__)
    TIMSK &= ~_BV(TOIE0);
#elif defined (__AVR_ATmega328P__)
    TIMSK0 &= ~_BV(TOIE0);
#endif
}

/// Begin error recovery: disable reception and wait for timer interrupt
void ps2_recover(void) {
    if (state == ERROR) {
        ps2_enable_recv(0);
#if defined (__AVR_ATmega8A__) // 8 Mhz
        TCNT0 = 255-35; // approx 1ms
        TIMSK |= _BV(TOIE0);
        TCCR0 = 4;  // enable: clk/256
#elif defined (__AVR_ATmega328P__)
#if (F_CPU==16000000)
        TCNT0 = 255-70; // approx 1ms
        TIMSK0 |= _BV(TOIE0);
        TCCR0B = 0x04;
#else /* 8Mhz */
        TCNT0 = 255-35; // approx 1ms
        TIMSK0 |= _BV(TOIE0);
        TCCR0B = 0x04;
#endif /* CPU speed */
#endif /* AVR Type */
    }
}

void ps2_enable_recv(uint8_t enable) {
    if (enable) {
        state = IDLE;
        ps2_dir(1, 1);
        // enable INT0 interrupt
#if defined (__AVR_ATmega8A__)
        GIFR |= _BV(INTF0);
        GICR |= _BV(INT0);
#elif defined (__AVR_ATmega328P__)
        EIFR |= _BV(INTF0);
        EIMSK |= _BV(INT0);
#endif
    } else {
        // disable INT0, then everything else
#if defined (__AVR_ATmega8A__)
        GICR &= ~_BV(INT0);
#elif defined (__AVR_ATmega328P__)
        EIMSK &= ~_BV(INT0);
#endif
        ps2_clk(0);
        ps2_dir(1, 0);
    }
}

// when 0 -> input, when 1 -> output
void ps2_dir(uint8_t dat_in, uint8_t clk_in) {
    dat_in ? (PS2DDR &= ~_BV(PS2DAT)) : (PS2DDR |= _BV(PS2DAT));
    clk_in ? (PS2DDR &= ~_BV(PS2CLK)) : (PS2DDR |= _BV(PS2CLK));
}

void ps2_clk(uint8_t c) {
    c ? (PS2PORT |= _BV(PS2CLK)) : (PS2PORT &= ~_BV(PS2CLK));
}

void ps2_dat(uint8_t d) {
    d ? (PS2PORT |= _BV(PS2DAT)) : (PS2PORT &= ~_BV(PS2DAT));
}

uint8_t ps2_avail() {
    return rx_head != rx_tail;
}

uint8_t ps2_getbyte() {
    uint8_t result = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % PS2_RXBUF_LEN;

    return result;
}

void ps2_sendbyte(uint8_t byte) {
    while (state != IDLE);

    // 1. pull clk low for 100us
    ps2_enable_recv(0);

    tx_byte = byte;
    state = TX_REQ0;

    // 128us
#if defined (__AVR_ATmega8A__)
    TCNT0 = 255-4;
    TIMSK |= _BV(TOIE0);
    TCCR0 = 4;
#elif defined (__AVR_ATmega328P__)
#if (F_CPU==16000000)
    TCNT0 = 255-8;
    TIMSK0 |= _BV(TOIE0);
    TCCR0B = 0x04;
#else /* 8Mhz */
    TCNT0 = 255-4;
    TIMSK0 |= _BV(TOIE0);
    TCCR0B = 0x04;
#endif 
#endif

    while (state != IDLE);
}

// Happens every negative PS2 clock transition.
//
// ISR_NOBLOCK because nothing here is really critical
ISR(INT0_vect, ISR_NOBLOCK) {
    uint8_t ps2_indat = ps2_datin();
    switch (state) {
    case ERROR:
        break;

    // Receive states

    case IDLE:
        if (ps2_indat == 0) {
            state = RX_DATA;
            bits = 8;
            parity = 0;
            recv_byte = 0;
        } else {
            state = ERROR;
        }
        break;
    case RX_DATA:
        recv_byte = (recv_byte >> 1) | ps2_indat;
        parity ^= ps2_indat;

        if (--bits == 0) {
            state = RX_PARITY;
        }
        break;
    case RX_PARITY:
        parity ^= ps2_indat;
        if (parity) {
            state = RX_STOP;
        } else {
            state = ERROR;
        }
        break;
    case RX_STOP:
        if (!ps2_indat) {
            state = ERROR;
        } else {
            rx_buf[rx_head] = recv_byte;
            rx_head = (rx_head + 1) % PS2_RXBUF_LEN;

            state = IDLE;
        }
        break;

    // Transmit states

    case TX_REQ0:
        // state will be switched in timer interrupt handler
        break;
    case TX_DATA:
        ps2_dat(tx_byte & 0x01);
        parity ^= tx_byte & 0x01;
        tx_byte >>= 1;
        if (--bits == 0) {
            state = TX_PARITY;
        }
        break;
    case TX_PARITY:
        ps2_dat(parity ^ 0x01);
        state = TX_STOP;
        break;
    case TX_STOP:
        ps2_dat(0);
        ps2_dir(1,1);
        state = TX_ACK;
        break;
    case TX_ACK:
        if (ps2_indat) {
            state = ERROR;
        } else {
            // this will end in TMR0 interrupt
            state = TX_END;

#if defined (__AVR_ATmega8A__)
            waitcnt = 50;           // after 100us it's an error
            TIMSK |= _BV(TOIE0);    // enable TMR0 interrupt
            TCNT0 = 255-2;              // 4 counts: 2us
            TCCR0 = 2;              // prescaler = f/8: go!
#elif defined (__AVR_ATmega328P__)
#if (F_CPU==16000000)
            waitcnt = 50;
            TIMSK0 |= _BV(TOIE0);
            TCNT0 = 255-4;
            TCCR0B = 0x02;
#else /* 8Mhz */
            waitcnt = 50;
            TIMSK0 |= _BV(TOIE0);
            TCNT0 = 255-2;
            TCCR0B = 0x02;
#endif
#endif
        }
        break;
    case TX_END:
        break;
    }
    ps2_recover();
}

/// transmit timer and error recovery vector
ISR(TIMER0_OVF_vect) {
    static uint8_t barkcnt = 0;

    switch (state) {
    case ERROR:
        state = IDLE;
        ps2_clk(0);
        ps2_dat(0);
        ps2_enable_recv(1);

        // stop timer
#if defined (__AVR_ATmega8A__)
        TIMSK &= ~_BV(TOIE0);
        TCCR0 = 0;
#elif defined (__AVR_ATmega328P__)
        TIMSK0 &= ~_BV(TOIE0);
        TCCR0B = 0; // Clear CS00,02 // FIXME: Do we need to configure only this?
#endif
        break;
    case TX_REQ0:
        // load the timer to serve as a watchdog
        // after 20 barks this is an error
#if defined (__AVR_ATmega8A__)
        barkcnt = 20;
        TIMSK |= _BV(TOIE0);    // enable TMR0 interrupt
        TCNT0 = 0;//255;            // 20*255*256/8e6 == 163ms
        TCCR0 = 4;               // prescaler = /256, go!
#elif defined (__AVR_ATmega328P__)
#if (F_CPU==16000000)
        barkcnt = 40;
        TIMSK0 |= _BV(TOIE0);
        TCNT0 = 0;
        TCCR0B = 0x04;
#else /* 8Mhz */
        barkcnt = 20;
        TIMSK0 |= _BV(TOIE0);
        TCNT0 = 0;
        TCCR0B = 0x04;
#endif
#endif
        // waited for 100us after pulling clock low, pull data low
        ps2_dat(0);
        ps2_dir(0, 0);

        // release the clock line
        ps2_dir(0, 1);

#if defined (__AVR_ATmega8A__)
        GIFR |= _BV(INTF0); // clear INT0 flag
        GICR |= _BV(INT0);  // enable INT0 @(negedge clk)
#elif defined (__AVR_ATmega328P__)
        EIFR |= _BV(INTF0);
        EIMSK |= _BV(INT0);
#endif

        // see you in INT0 handler
        bits = 8;
        parity = 0;

        state = TX_DATA;
        break;
    case TX_END:
        // wait until both clk and dat are up, that will be all
        if (ps2_clkin() && ps2_datin()) {
#if defined (__AVR_ATmega8A__)
            TIMSK &= ~_BV(TOIE0);
            TCCR0 = 0;
#elif defined (__AVR_ATmega328P__)
            TIMSK0 &= ~_BV(TOIE0);
            TCCR0B = 0x00; // FIXME
#endif
            state = IDLE;
        } else {
            if (waitcnt == 0) {
                state = ERROR;
                ps2_recover();
            } else {
                waitcnt--;
            }
        }
        break;
    default:
        // watchdog barked: probably not a mouse!
        if (barkcnt == 0) {
            state = ERROR;
            ps2_recover();
        } else {
            barkcnt--;
        }
        break;
    }
}
