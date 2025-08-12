
# Pico TPM SPIder

Pico TPM SPIder is a Volume Master Key (VMK) recovery tool for SPI-based TPM 2.0 chips, built on Raspberry Pi Pico hardware.
It passively monitors SPI communication with a discrete TPM 2.0 chip and parses transactions in real time to extract the VMK.
This project specifically targets the STMicroelectronics [ST33TPHF2XSPI](https://www.st.com/en/product/st33tphf2xspi) used in Dell Latitude 5320 laptops,
though other devices and TPM 2.0 chips may be compatible.

## Hardware

A custom PCB is provided in the `hardware` directory that simplifies connecting to the SPI bus on Dell Latitude 5320 laptops via an unpopulated connector.
<!-- TODO: insert image pcb design -->
<!-- TODO: insert image of laptop motherboard -->
The PCB provides two methods for interfacing with the motherboard.

1) **Pogo Pins**  
The board includes footprints for pogo pins that align with the Latitude 5320's unpopulated connector.
<!-- TODO: insert image of pogo pins being used -->

2) **Flat Flex Ribbon**  
The board accommodates a flat flex ribbon connector. A flat flex ribbon can be connected to the PCB and then pressed against the Latitude 5320's unpopulated connector.
<!-- TODO: insert image of ribbon being used -->

Since the unpopulated connector does not include a chip select (CS) signal, the PCB grounds the Pi Pico's CS input (GPIO 3) by default.
Cutting jumper `JP1` will disconnect the CS input from GND.  


### Parts
| description                | part number |
|----------------------------|-------------|
| Raspberry Pi Pico          | [SC0915](https://www.digikey.com/en/products/detail/raspberry-pi/SC0915/13624793) |
| Pogo Pins                  | [P50-B1-16mm](https://www.amazon.com/KooingTech-Pressure-Testing-Fingers-Printed/dp/B0DMGG23QF) |
| Flat Flex Ribbon           | [0982660209](https://www.digikey.com/en/products/detail/molex/0982660209/2750458) |
| Flat Flex Ribbon Connector | [CF31201D0R0-05-NH](https://www.digikey.com/en/products/detail/cvilux-usa/CF31201D0R0-05-NH/15792943) |

## Build

Download the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk).  
Install dependencies specified by the Raspberry Pi Pico SDK documentation.  
Build the project.  

```
git clone https://github.com/decrazyo/pico-tpm-spider.git
cd pico-tpm-spider
export PICO_SDK_PATH=/path/to/your/Pico-SDK
mkdir build
cd build
cmake ..
make
```

## Install

Press and hold the `BOOTSEL` button while plugging in the Pi Pico to make it appear as a mass storage device.  
Install the Pico TPM SPIder firmware by copying `pico-tpm-spider/build/pico-tpm-spider.uf2` to the Pi Pico's mass storage device.  

## Usage

Connect the Pi Pico's pins to your TPM chip according to the following table.  

| pico pin | spi signal |
|----------|------------|
| GPIO 0   | MOSI       |
| GPIO 1   | MISO       |
| GND      | GND        |
| GPIO 2   | CLK        |
| GPIO 3   | CS or GND  |

The use of a chip select (CS) signal is optional but recommenced since it significantly reduces the amount of data that the Pi Pico needs to parse.
**If you're not using the CS signal then GPIO 3 must be grounded!**
GPIO 3 is grounded by default on the provided PCB.  

Connect to the Pi Pico with your serial terminal of choice and boot the target system.  

```
picocom -q /dev/ttyACM0 -b 115200
```

You should see output similar to this.

> Capturing SPI traffic into 12 16384-byte buffers.  
> Press any key to finalize the capture.  
> VMK: DEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DE  

If the VMK does not automatically appear by the time the system has booted then press a key to finalize the capture.
This will cause the Pi Pico to parse any data remaining in its buffer and may recover the VMK.  

The recovered VMK can be used with the `dislocker` utility to decrypt the target system's hard drive.  

```
echo DEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DEDEADC0DE | xxd -r -p > vmk.bin
mkdir /mnt/dislocker
mkdir /mnt/drive_c
dislocker -K vmk.bin /dev/sdb3 /mnt/dislocker
mount -t ntfs-3g -o remove_hiberfile,recover /mnt/dislocker/dislocker-file /mnt/drive_c
ls /mnt/drive_c/
```

## Related Projects
* https://github.com/ReversecLabs/bitlocker-spi-toolkit
* https://github.com/stacksmashing/pico-tpmsniffer
* https://github.com/zaphoxx/pico-tpmsniffer-spi
* https://github.com/aplhk/pico-spisniffer
