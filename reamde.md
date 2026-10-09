# esp32-voltage-protector
How to run it

Option 1: open the Wokwi project

Open: [TODO: paste your Wokwi project link here]
Click the green Start button.
Open the serial monitor (115200 baud) and type HELP.

Option 2: rebuild it from the files

Go to wokwi.com and create a new ESP32 project (Arduino).
Replace the contents of sketch.ino, diagram.json and libraries.txt with the files in project_files/. The only library needed is LiquidCrystal I2C; Preferences comes with the ESP32 core.
Click Start.

Using the simulation

Click a potentiometer and drag its slider. The voltage pot goes from 0 to 300 V and the current pot goes from 0 to 10 A.
The relay module switches on when the load is connected and off after a trip.
Press and hold the push buttons in the circuit view to use them.
