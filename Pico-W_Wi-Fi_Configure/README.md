
# Pico-W Runtime Wi-Fi Configuration

This integrated version uses runtime provisioning. Wi-Fi and SMTP values are entered in the setup portal and stored in flash.

In this project, credentials are not configured via CMake flags.


**This software makes it unnecessary to know the network name, password and - if required - IP address, network mask and default gateway when compiling. These can be set directly on the Pico W and modified afterwards through runtime provisioning.**

To do this, the Pico W is first started in setup AP mode. In this mode, it provides the setup portal web page for entering the data. The user connects to the Pico W SSID (`picow_config`, unless changed in source) and opens the setup portal in a browser at `http://192.168.0.1`.

After completing the settings (press the Setup button), the data is stored in flash, AP mode is terminated, and the Pico W connects to the specified WLAN in STA mode using the saved data.

On subsequent boots, if flash contains a valid configuration, the Pico W skips AP mode and starts directly in STA mode.

To change stored data later, pull `SETUP_GPIO` (GPIO22) to GND for at least 3 seconds during reboot. This forces setup AP mode so the values can be edited again.

# DHCP versus fixed IP:
If you require the user to enter a fixed IP address (which means you don't need DHCP support), set `LWIP_DHCP` to `0` in `lwipopts.h`. This reduces code size.

# How to use this software:
Copy the `wifi_setup` subdirectory into your project.
Copy the lines in `main.c` between the "configuration code starts here / ends here" tags to somewhere near the beginning of your code.
Use the code between the "Typical connection sequence starts/ends here" tags as a template.

Do not forget to include `access_point.h`.
`CMakeLists.txt` shows the necessary directives, include paths, and link libraries.

# The demo software:
`main.c` and `tcp-server.c` together form a simple example. After completing the configuration described above, a TCP echo server is started on port 4711.

How to build:
See chapter 8 of the 'Getting Started with Raspberry Pi Pico' for instructions.

1. Create a directory for your project, alongside the pico-sdk directory.
2. Download 'Wi-Fi Configure' into this directory.
3. Then copy the pico_sdk_import.cmake file from the external folder in your pico-sdk installation to your project folder.
4. Now follow the usual steps:
$ mkdir build
$ cd build
$ cmake ...
$ make

You can use the `client` program in the "linux" subdirectory to send data to the server.
To create `client`, it is usually sufficient to change to the `linux` subdirectory and run `make client`.

Run it with : `./client IP-ADDRESS 4711`, enter any text and send it with ".<RTN\>".<br>
To exit, type "q<RTN\>".<br>
To erase the configuration from the flash and return the pico to "unconfigured", use the special command "erase!”
In addition, the server will respond to the "conf!" command with its IP address.

Note: If you have not configured a fixed IP address, you need to find out the address either by viewing the debug output on a terminal, using the `nmap` utility, or from your wireless router.<br>

# Modify The Web Pages:
For the Pico-W, the HTML files must be converted to binary form. The Perl script "wifi_setup /external/makefsdata" is used for this. Do not use it directly, but change to the subdirectory "wifi_setup" and run the shell script "rebuild_fs.sh".
This will create the file "my_fsdata.c" which will be included in "pico-sdk/lib/lwip/src/apps/http/fs.c" during compilation.

**ATTENTION:**
"makefsdata" translates ALL FILES in the access_point/fs directory, not just html pages. If you get strange error messages when compiling, check if there are other files in that directory. Check that there are no hidden `.*` files.

The HTML page names (without the extensions) must result in a valid variable name. Do not use "my-page.html". Use "my_page.html".

# Copyright:
Gerhard Schiller (c) 2025 gerhard.schiller@protonmail.com

# Licences:
This software is licensed under the "The 3-Clause BSD Licence".<br>
See LICENSE.TXT for details.

This license also applies to the pico-sdk examples used as a basis to create this software, with the exception of the dhcp-server, which is licensed under the 'MIT License'.

