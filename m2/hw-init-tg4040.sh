#!/bin/sh
# Brick Pro (TG4040): preparazione dell'hardware come /usr/trimui/bin/runtrimui.sh del firmware.
gpio() {   # numero, direzione, valore
    [ -d /sys/class/gpio/gpio$1 ] || echo $1 > /sys/class/gpio/export
    echo -n $2 > /sys/class/gpio/gpio$1/direction
    [ -n "$3" ] && echo -n $3 > /sys/class/gpio/gpio$1/value
    return 0
}
gpio 107 out 1     # PD11: alimentazione 5 V
gpio 227 out 0     # PH3: motore della vibrazione spento
gpio 243 in        # PH19: interruttore FN
