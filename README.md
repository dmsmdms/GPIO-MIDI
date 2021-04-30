# GPIO-MIDI
This program can implement your own MIDI keyboard connected via GPIO. It would be nice to see how it works before. https://github.com/dmsmdms/GPIO-MIDI/wiki 
### Build guide and run (PC)
```
git clone https://github.com/dmsmdms/GPIO-MIDI  
make
./gpio_midi
```
### Build guide and run (RPI)
```
git clone https://github.com/dmsmdms/GPIO-MIDI  
make rpi
./gpio_midi -s 192.168.0.100
```
It is important to specify IP of your PC!
## Testing
After running a server on your PC, you can play test note.
```
./gpio_midi -t C4
./gpio_midi -t D#3
./gpio_midi -t Gb4
```
If you are running test on your RPI don't forget to specify server IP.
```
./gpio_midi -s 192.168.0.100 -t C4
```
