#include "ch32fun.h"
#include <stdbool.h>
#include <stdio.h>

#define STATUS_LED PC0

static void setup_led(void) {
    funGpioInitAll();
    funPinMode(STATUS_LED, GPIO_Speed_10MHz | GPIO_CNF_OUT_PP);
    funDigitalWrite(STATUS_LED, FUN_LOW);
}

int main(void) {
    SystemInit();
    setup_led();

    printf("Whisker Breeze hello world!\r\n");

    bool led_on = false;
    while (1) {
        led_on = !led_on;
        funDigitalWrite(STATUS_LED, led_on ? FUN_HIGH : FUN_LOW);
        puts("Fan controller heartbeat...");
        Delay_Ms(1000);
    }
}
