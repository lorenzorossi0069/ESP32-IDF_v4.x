Porting of https://github.com/lucadentella/esp32-tutorial/tree/master/14_basic_webserver to ESP-IDF v4.1 (original (in v3.x) is explained in http://www.lucadentella.it/2017/07/08/esp32-20-webserver/)

Further changes, besides porting:
1) ESP is WiFi AP (default SSID=myESP and password=myPassword can be changed calling idf.py menuconfig).
2) On Off buttons replaced by blue refresh button
3) read 2 GPIO input and set mirror output

