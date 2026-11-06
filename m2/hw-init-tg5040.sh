#!/bin/sh
# Smart Pro (TG5040): preparazione dell'hardware come /usr/trimui/bin/runtrimui.sh del firmware.
gpio() {   # numero, direzione, valore
    [ -d /sys/class/gpio/gpio$1 ] || echo $1 > /sys/class/gpio/export
    echo -n $2 > /sys/class/gpio/gpio$1/direction
    [ -n "$3" ] && echo -n $3 > /sys/class/gpio/gpio$1/value
    return 0
}
gpio 107 out 1     # PD11: alimentazione 5 V
gpio 227 out 0     # PH3: motore della vibrazione spento
gpio 110 out 1     # PD14: alimentazione della scheda della levetta sinistra
gpio 114 out 1     # PD18: alimentazione della scheda della levetta destra
gpio 243 in        # PH19: interruttore (DIP)
