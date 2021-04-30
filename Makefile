CC = gcc -Wall -pipe -O3 -march=native -o gpio_midi

all:
	@ $(CC) gpio_midi.c

rpi:
	@ $(CC) gpio_midi_rpi.c

clean:
	@ rm gpio_midi