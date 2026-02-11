
# Pico TPM SPIder

Pico TPM SPIder is a Volume Master Key (VMK) recovery tool for SPI-based TPM 2.0 chips, built on [Raspberry Pi Pico](https://www.digikey.com/en/products/detail/raspberry-pi/SC0915/13624793) hardware.
It passively monitors SPI communication with a discrete TPM 2.0 chip and parses transactions in real time to extract the VMK.
This project specifically targets the STMicroelectronics [ST33TPHF2XSPI](https://www.st.com/en/product/st33tphf2xspi) used in several Dell laptops,
though other devices and TPM 2.0 chips may be compatible.

Pico TPM SPIder is designed to capture TPM SPI traffic clocked at 25MHz to 50MHz with or without the use of a chip select signal.
Lower frequencies can be supported by modifying [sniffer.pio](https://github.com/decrazyo/pico-tpm-spider/blob/main/sniffer.pio).
Higher frequencies may be supported through overclocking.

<!-- TODO: add link to cyphercon talk -->

## Compatibility

The following devices have been tested.

| device              | working | bus voltage | chip select  | notes |
|---------------------|---------|-------------|--------------|-------|
| Dell Latitude 5320  | yes     | 3.3V        | inaccessible | SPI flash memory, unpopulated SPI connector |
| Dell Latitude 5431  | yes     | 3.3V        | inaccessible | SPI flash memory |
| Dell Precision 3490 | yes     | 1.8V        | inaccessible | SPI flash memory |

> [!CAUTION]
> Check the voltage of your target device's SPI bus!
> The Raspberry Pi Pico is only designed to work with 3.3V logic.
> A converter should be used to interface with other bus voltages.
> The [TXS0108E](https://www.amazon.com/TXS0108E-Bi-Directional-Converter-Channel-Conversion/dp/B0F285G5JT) has been proven to work for this.

## Hardware

The following methods of connecting to the SPI bus have been tested.

### WSON8 Probe

All of the tested devices have a [SPI flash memory chip](https://www.winbond.com/resource-files/W25R256JV%20RevD%2006162020.pdf) on the same bus as the TPM chip.
A [WSON8 probe](https://www.amazon.com/Amagogo-Download-Efficient-Industrial-Electronic/dp/B0DRZGRN81) can be used to connect to the SPI bus via the flash chip.
This is by far the easiest way to connect to the SPI bus.

![WSON8 SPI flash memory chip pinout](https://github.com/decrazyo/pico-tpm-spider/blob/main/img/wson8.png)

### Custom PCB

A custom PCB, inspired by [stacksmashing's design](https://github.com/stacksmashing/pico-tpmsniffer), is provided in the `hardware` directory and simplifies connecting to the SPI bus on Dell Latitude 5320 laptops via an unpopulated connector.
The PCB provides two methods for interfacing with the connector.

![Unpopulated SPI connector pinout](https://github.com/decrazyo/pico-tpm-spider/blob/main/img/connector.png)

1) **Pogo Pins** (hard to solder, easy to use)  
The board includes footprints for pogo pins that align with the Latitude 5320's unpopulated connector.

2) **Flat Flex Ribbon** (easy to solder, hard to use)  
The board accommodates a flat flex ribbon connector.
A flat flex ribbon can be connected to the PCB and then pressed against the Latitude 5320's unpopulated connector.

Since the unpopulated connector does not include a chip select (CS) signal, the PCB grounds the Pi Pico's CS input (GPIO 3) by default.
Cutting jumper `JP1` will disconnect the CS input from GND.  

| description                | part number |
|----------------------------|-------------|
| Pogo Pins                  | [P50-B1-16mm](https://www.amazon.com/KooingTech-Pressure-Testing-Fingers-Printed/dp/B0DMGG23QF) |
| Flat Flex Ribbon           | [0982660209](https://www.digikey.com/en/products/detail/molex/0982660209/2750458) |
| Flat Flex Ribbon Connector | [CF31201D0R0-05-NH](https://www.digikey.com/en/products/detail/cvilux-usa/CF31201D0R0-05-NH/15792943) |

### Needle Probes / Soldering

[Needle probes](https://sensepeek.com/4x-sq10-probes-with-test-wires) can be used to connect to any exposed part of the SPI bus.
Such a setup can be precarious so soldering may be a better option if you have some [30 gauge wire](https://www.amazon.com/Jonard-R-30B-0050-Replacement-Kynar-Dispenser/dp/B006C4ABR0) and a steady hand.

> [!NOTE]
> Interference can cause various issues including
> failure to boot, entering Dell's "manufacturing mode", BIOS/UEFI corruption, and failure to recover the VMK.
> Keep your SPI wires short, away from sources of interference, and preferably shielded to avoid those issues.

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
> [!IMPORTANT]
> If you're not providing the Pi Pico with the TPM's CS signal then GPIO 3 must be grounded!  
> GPIO 3 is grounded by default on the provided PCB.  

Connect to the Pi Pico with your serial terminal of choice and boot the target system.  

```
picocom -q /dev/ttyACM0 -b 115200
```

You should see output similar to this.

> Capturing SPI traffic into 12 16384-byte buffers.  
> Press any key to finalize the capture and reboot.  
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
* https://github.com/decrazyo/spi-analyzer
* https://github.com/ReversecLabs/bitlocker-spi-toolkit
* https://github.com/NoobieDog/TPM-Sniffing
* https://github.com/stacksmashing/pico-tpmsniffer
* https://github.com/zaphoxx/pico-tpmsniffer-spi
* https://github.com/aplhk/pico-spisniffer
