# **ESP32-S3 based Amplifier Controller**

This project transforms an ESP32-S3 development board into a sophisticated remote control for Hypex Fusion series amplifiers. It provides a web-based interface for real-time control and monitoring, accessible from any device on the same network.

The primary motivation was to create a flexible and modern control solution that goes beyond a standard IR remote, enabling control via a web browser and integration into smart home systems. The project was developed by reverse-engineering the proprietary USB HID protocol used by the Hypex Filter Designer (HFD) software.

## **Features**

* **Web-Based Control:** A clean, responsive web interface to control all major amplifier functions.  
* **Real-Time Updates:** Utilizes WebSockets to ensure the UI is always in sync with the amplifier's actual state, providing instant feedback.  
* **Comprehensive Control:**  
  * Set Presets (1, 2, 3\)  
  * Adjust Volume (-99 dB to 0 dB)  
  * Toggle Mute  
  * Select Input Source (XLR, RCA, SPDIF, etc.)  
  * Toggle EQ (placeholder, requires further protocol analysis)  
* **Status Display:**  
  * Displays the name of the currently loaded DSP filter.  
  * Shows the current state of all controllable parameters.  
* **Safe Secret Management:** WiFi credentials are kept out of version control using a secrets.h file, which is ignored by Git.  
* **Robust Architecture:** Built on the native ESP-IDF framework using FreeRTOS for clear, decoupled tasks handling USB communication, the web server, and the main control logic.

## **Hardware Requirements**

1. **ESP32-S3 Development Board:** An ESP32-S3 with a USB-OTG capable port is required (e.g., ESP32-S3-DevKitC-1).  
2. **Hypex Fusion Amplifier:** Any model that is controllable via the Hypex Filter Designer (HFD) software over USB.  
3. **USB-A to USB-C/Micro-USB Cable:** To connect the ESP32-S3 (as Host) to the Hypex amplifier (as Device). A USB-OTG adapter might be necessary depending on your board and cable.  
4. **(Optional) 5V Relay Module:** To physically switch the amplifier's main power.  
5. **(Optional) Optocoupler Modules:** For safely connecting 12V trigger signals from another pre-amplifier to the 3.3V GPIOs of the ESP32.  
6. **5V Power Supply:** A stable power supply for the ESP32-S3.

## **Software & Setup**

This project is built using **PlatformIO** with **Visual Studio Code**.

1. **Install VS Code and PlatformIO:** If you haven't already, install VS Code and the PlatformIO IDE extension.  
2. **Clone the Repository:** Clone this project from your GitHub repository to your local machine.  
3. **Open in PlatformIO:** Open the cloned project folder in VS Code. PlatformIO should automatically recognize the platformio.ini file.  
4. **Configure WiFi:**  
   * In the include/ directory, copy the secrets.h.example file and rename the copy to secrets.h.  
   * Open secrets.h and enter your WiFi network's SSID and password.  
5. **Build & Upload:**  
   * PlatformIO will automatically detect the required frameworks (ESP-IDF) and components (cJSON) and install them during the first build.  
   * Use the PlatformIO controls in the status bar to **Build** and **Upload** the firmware to your ESP32-S3.

## **USB Protocol Summary**

The control is based on a reverse-engineered USB HID protocol.

* **Communication:** A "Command-Response" pattern over two Interrupt Endpoints.  
  * **OUT 0x01:** Send a 64-byte command packet.  
  * **IN 0x81:** Receive a 64-byte status packet.  
* **Command Packet (Packet ID 0x05):**  
  * Byte 1: Input Source (e.g., 0x02 for RCA)  
  * Byte 2: Preset (1, 2, or 3\)  
  * Bytes 3-4: Volume as a little-endian int16\_t (dB \* 100\)  
  * Byte 6: Mute flag (0x80 for Mute ON)  
* **Status Request Packets:**  
  * 0x06 0x02...: Request main status.  
  * 0x03 0x08...: Request filter name as an ASCII string.

## **How to Use**

1. Ensure all hardware is wired correctly according to the provided schematics.  
2. Power on the ESP32. It will automatically connect to the configured WiFi network.  
3. Open the PlatformIO Serial Monitor to see the log output and the IP address assigned to the ESP32.  
4. Open a web browser on any device in the same network and navigate to the ESP32's IP address.  
5. The web interface will load, connect via WebSocket, and display the current amplifier status. You can now control all functions directly from the web page.
