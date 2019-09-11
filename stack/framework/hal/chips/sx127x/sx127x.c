/* * OSS-7 - An opensource implementation of the DASH7 Alliance Protocol for ultra
 * lowpower wireless sensor communication
 *
 * Copyright 2017 University of Antwerp
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* \file
 *
 * Driver for sx127x
 *
 * @author philippe.nunes@cortus.com
 * @author liam.oorts@aloxy.io
 * @author glenn.ergeerts@uantwerpen.be
 */

#include "string.h"
#include "types.h"
#include "stdlib.h"

#include "debug.h"
#include "log.h"
#include "hwradio.h"
#include "hwdebug.h"
#include "hwspi.h"
#include "platform.h"
#include "errors.h"

#include "sx1276Regs-Fsk.h"

#include "crc.h"
#include "pn9.h"
#include "fec.h"

#define SX127X_FXOSC       32000000UL

#define FREQ_STEP 61.03515625

#define FIFO_SIZE   64
#define BYTES_IN_RX_FIFO            32
#define BG_THRESHOLD                5
#define FG_THRESHOLD                32
#define FIFO_AVAILABLE_SPACE        FIFO_SIZE - FG_THRESHOLD

#if defined(FRAMEWORK_LOG_ENABLED) && defined(FRAMEWORK_PHY_LOG_ENABLED)
#define DPRINT(...) log_print_stack_string(LOG_STACK_PHY, __VA_ARGS__)
#define DPRINT_DATA(...) log_print_data(__VA_ARGS__)
#else
#define DPRINT(...)
#define DPRINT_PACKET(...)
#define DPRINT_DATA(...)
#endif

// #define testing_ADV

#if PLATFORM_NUM_DEBUGPINS >= 2
    #ifndef testing_ADV
        #define DEBUG_TX_START() hw_debug_set(0);
        #define DEBUG_TX_END() hw_debug_clr(0);
        #define DEBUG_RX_START() hw_debug_set(1);
        #define DEBUG_RX_END() hw_debug_clr(1);
        #define DEBUG_FG_START()
        #define DEBUG_FG_END()
        #define DEBUG_BG_START()
        #define DEBUG_BG_END()
    #else
        #define DEBUG_TX_START()
        #define DEBUG_TX_END()
        #define DEBUG_RX_START()
        #define DEBUG_RX_END()
        #define DEBUG_FG_START() hw_debug_set(0);
        #define DEBUG_FG_END() hw_debug_clr(0);
        #define DEBUG_BG_START() hw_debug_set(1);
        #define DEBUG_BG_END() hw_debug_clr(1);
    #endif
#else
    #define DEBUG_TX_START()
    #define DEBUG_TX_END()
    #define DEBUG_RX_START()
    #define DEBUG_RX_END()
    #define DEBUG_FG_START()
    #define DEBUG_FG_END()
    #define DEBUG_BG_START()
    #define DEBUG_BG_END()
#endif

#ifdef PLATFORM_SX127X_USE_DIO3_PIN
  #define CHECK_FIFO_EMPTY() hw_gpio_get_in(SX127x_DIO3_PIN)
#else
  #define CHECK_FIFO_EMPTY() (read_reg(REG_IRQFLAGS2) & 0x40)
#endif

#define CHECK_FIFO_LEVEL() (read_reg(REG_IRQFLAGS2) & 0x20)
#define CHECK_FIFO_FULL()  (read_reg(REG_IRQFLAGS2) & 0x80)

#if defined(PLATFORM_USE_ABZ) && defined(PLATFORM_SX127X_USE_MANUAL_RXTXSW_PIN)
  #error "Invalid configuration"
#endif

static const uint16_t rx_bw_startup_time[21] = {63, 74, 85, 100, 84, 120, 119, 144, 169, 215, 264, 313, 
  407, 504, 601, 791, 984, 1180, 1560, 1940, 2330};

static uint8_t rx_bw_number = 21;

typedef enum {
  OPMODE_SLEEP = 0,
  OPMODE_STANDBY = 1,
  OPMODE_FSTX = 2,
  OPMODE_TX = 3,
  OPMODE_FSRX = 4,
  OPMODE_RX = 5,
} opmode_t;

typedef enum {
  STATE_IDLE,
  STATE_TX,
  STATE_RX,
  STATE_STANDBY
} state_t;

/*
 * FSK packet handler structure
 */
typedef struct
{
    uint8_t Size;
    uint8_t NbBytes;
    uint8_t FifoThresh;
}FskPacketHandler_t;

FskPacketHandler_t FskPacketHandler_sx127x;

/*
 * TODO:
 * - packets > 64 bytes
 * - FEC
 * - background frames
 * - validate RSSI measurement (CCA)
 * - after CCA chip does not seem to go into TX
 * - research if it has advantages to use chip's top level sequencer
 */

static spi_handle_t* spi_handle = NULL;
static spi_slave_handle_t* sx127x_spi = NULL;
static alloc_packet_callback_t alloc_packet_callback;
static release_packet_callback_t release_packet_callback;
static rx_packet_callback_t rx_packet_callback;
static tx_packet_callback_t tx_packet_callback;
static rx_packet_header_callback_t rx_packet_header_callback;
static tx_refill_callback_t tx_refill_callback;
static state_t state = STATE_STANDBY;
static hw_radio_packet_t* current_packet;

static bool is_sx1272 = false;
static bool enable_refill = false;
static bool enable_preloading = false;
static uint8_t remaining_bytes_len = 0;
static uint8_t previous_threshold = 0;
static uint16_t previous_payload_length = 0;
static bool io_inited = false;

void enable_spi_io() {
  if(!io_inited){
    hw_radio_io_init();
    io_inited = true;
  }
  spi_enable(spi_handle);
}

void set_opmode(uint8_t opmode);
static void fifo_threshold_isr();


static uint8_t read_reg(uint8_t addr) {
  enable_spi_io();
  spi_select(sx127x_spi);
  spi_exchange_byte(sx127x_spi, addr & 0x7F); // send address with bit 7 low to signal a read operation
  uint8_t value = spi_exchange_byte(sx127x_spi, 0x00); // get the response
  spi_deselect(sx127x_spi);
  //DPRINT("READ %02x: %02x\n", addr, value);
  return value;
}

static void write_reg(uint8_t addr, uint8_t value) {
  enable_spi_io();
  spi_select(sx127x_spi);
  spi_exchange_byte(sx127x_spi, addr | 0x80); // send address with bit 8 high to signal a write operation
  spi_exchange_byte(sx127x_spi, value);
  spi_deselect(sx127x_spi);
  //DPRINT("WRITE %02x: %02x", addr, value);
}

void write_reg_16(uint8_t start_reg, uint16_t value) {
  write_reg(start_reg, (uint8_t)((value >> 8) & 0xFF));
  write_reg(start_reg + 1, (uint8_t)(value & 0xFF));
}

static void write_fifo(uint8_t* buffer, uint8_t size) {
  enable_spi_io();
  spi_select(sx127x_spi);
  spi_exchange_byte(sx127x_spi, 0x80); // send address with bit 8 high to signal a write operation
  spi_exchange_bytes(sx127x_spi, buffer, NULL, size);
  spi_deselect(sx127x_spi);
  // DPRINT("WRITE FIFO %i", size);
  // DPRINT_DATA(buffer, size);
}

static void read_fifo(uint8_t* buffer, uint8_t size) {
  enable_spi_io();
  spi_select(sx127x_spi);
  spi_exchange_byte(sx127x_spi, REG_FIFO);
  spi_exchange_bytes(sx127x_spi, NULL, buffer, size);
  spi_deselect(sx127x_spi);
  DPRINT("READ FIFO %i", size);
}

static opmode_t get_opmode() {
  return (read_reg(REG_OPMODE) & ~RF_OPMODE_MASK);
}


//static void dump_register()
//{

//    DPRINT("************************DUMP REGISTER*********************");

//    for (uint8_t add=0; add <= REG_VERSION; add++)
//        DPRINT("ADDR %2X DATA %02X \r\n", add, read_reg(add));

//    // Please note that when reading the first byte of the FIFO register, this
//    // byte is removed so the dump is not recommended before a TX or take care
//    // to fill it after the dump

//    DPRINT("**********************************************************");
//}

static void set_antenna_switch(opmode_t opmode) {
  if(opmode == OPMODE_TX) {
#ifdef PLATFORM_SX127X_USE_MANUAL_RXTXSW_PIN
    hw_gpio_set(SX127x_MANUAL_RXTXSW_PIN);
#endif
#ifdef PLATFORM_USE_ABZ
    hw_gpio_clr(ABZ_ANT_SW_RX_PIN);
    if((read_reg(REG_PACONFIG) & RF_PACONFIG_PASELECT_PABOOST) == RF_PACONFIG_PASELECT_PABOOST) {
      hw_gpio_clr(ABZ_ANT_SW_TX_PIN);
      hw_gpio_set(ABZ_ANT_SW_PA_BOOST_PIN);
    } else {
      hw_gpio_set(ABZ_ANT_SW_TX_PIN);
      hw_gpio_clr(ABZ_ANT_SW_PA_BOOST_PIN);
    }
#endif
  } else {
#ifdef PLATFORM_SX127X_USE_MANUAL_RXTXSW_PIN
    hw_gpio_clr(SX127x_MANUAL_RXTXSW_PIN);
#endif
#ifdef PLATFORM_USE_ABZ
    hw_gpio_set(ABZ_ANT_SW_RX_PIN);
    hw_gpio_clr(ABZ_ANT_SW_TX_PIN);
    hw_gpio_clr(ABZ_ANT_SW_PA_BOOST_PIN);
#endif
  }
}

static inline void flush_fifo() {
  sched_cancel_task(&fifo_threshold_isr);
  DPRINT("Flush fifo @ %i\n", timer_get_counter_value());
  write_reg(REG_IRQFLAGS2, RF_IRQFLAGS2_FIFOOVERRUN);
}

static void init_regs() {
  uint8_t gaussian_shape_filter = 2; // 0: no shaping, 1: BT=1.0, 2: BT=0.5, 3: BT=0.3 // TODO benchmark?
  if(is_sx1272) {
    write_reg(REG_OPMODE, RF_OPMODE_SLEEP | gaussian_shape_filter << 3); // FSK; modulation shaping, sleep
  } else {
    write_reg(REG_OPMODE, RF_OPMODE_SLEEP | RF_OPMODE_MODULATIONTYPE_FSK); // FSK, hi freq, sleep
  }

  // PA
  hw_radio_set_tx_power(10);
  if(is_sx1272) {
    write_reg(REG_PARAMP, RF_PARAMP_0040_US | RF_PARAMP_LOWPNTXPLL_OFF); // PaRamp=40us // TODO, use LowPnRxPll?
  } else {
    write_reg(REG_PARAMP, RF_PARAMP_0040_US | (gaussian_shape_filter << 5)); // modulation shaping and PaRamp=40us
  }

  // RX
  //  write_reg(REG_OCP, 0); // TODO default for now
  write_reg(REG_LNA, RF_LNA_GAIN_G1 | RF_LNA_BOOST_ON); // highest gain for now, for 868 // TODO LnaBoostHf consumes 150% current compared to default LNA

  // TODO validate:
  // - RestartRxOnCollision (off for now)
  // - RestartRxWith(out)PllLock flags: set on freq change
  // - AfcAutoOn: default for now (use AGC)
  // - AgcAutoOn: default for now (use AGC)
  // - RxTrigger: default for now
  write_reg(REG_RXCONFIG, RF_RXCONFIG_RESTARTRXONCOLLISION_OFF | RF_RXCONFIG_AFCAUTO_ON |
                          RF_RXCONFIG_AGCAUTO_ON | RF_RXCONFIG_RXTRIGER_PREAMBLEDETECT);

  write_reg(REG_RSSICONFIG, RF_RSSICONFIG_OFFSET_P_00_DB | RF_RSSICONFIG_SMOOTHING_8); // TODO no RSSI offset for now + using 8 samples for smoothing
  //  write_reg(REG_RSSICOLLISION, 0); // TODO not used for now
  write_reg(REG_RSSITHRESH, RF_RSSITHRESH_THRESHOLD); // TODO using -128 dBm for now

  //  write_reg(REG_AFCBW, 0); // TODO not used for now (AfcAutoOn not set)
  //  write_reg(REG_AFCFEI, 0); // TODO not used for now (AfcAutoOn not set)
  //  write_reg(REG_AFCMSB, 0); // TODO not used for now (AfcAutoOn not set)
  //  write_reg(REG_AFCLSB, 0); // TODO not used for now (AfcAutoOn not set)
  //  write_reg(REG_FEIMSB, 0); // TODO freq offset not used for now
  //  write_reg(REG_FEILSB, 0); // TODO freq offset not used for now
  write_reg(REG_PREAMBLEDETECT, RF_PREAMBLEDETECT_DETECTOR_ON | RF_PREAMBLEDETECT_DETECTORSIZE_3 | RF_PREAMBLEDETECT_DETECTORTOL_15);  
  // TODO validate PreambleDetectorSize (2 now) and PreambleDetectorTol (10 now)
  // write_reg(REG_RXTIMEOUT1, 0); // not used for now
  // write_reg(REG_RXTIMEOUT2, 0); // not used for now
  // write_reg(REG_RXTIMEOUT3, 0); // not used for now
  // write_reg(REG_RXDELAY, 0); // not used for now
  // write_reg(REG_OSC, 0x07); // keep as default: off

  write_reg(REG_SYNCCONFIG, RF_SYNCCONFIG_AUTORESTARTRXMODE_OFF | RF_SYNCCONFIG_PREAMBLEPOLARITY_AA | 
    RF_SYNCCONFIG_SYNC_ON | RF_SYNCCONFIG_SYNCSIZE_2); // no AutoRestartRx, default PreambePolarity, enable syncword of 2 bytes
  write_reg(REG_SYNCVALUE1, 0xE6); // by default, the syncword is set for CS0(PN9) class 0
  write_reg(REG_SYNCVALUE2, 0xD0);

  write_reg(REG_PACKETCONFIG1, RF_PACKETCONFIG1_PACKETFORMAT_FIXED | RF_PACKETCONFIG1_DCFREE_OFF |
    RF_PACKETCONFIG1_CRC_OFF | RF_PACKETCONFIG1_CRCAUTOCLEAR_OFF | RF_PACKETCONFIG1_ADDRSFILTERING_OFF | 
    RF_PACKETCONFIG1_CRCWHITENINGTYPE_CCITT); // fixed length (unlimited length mode), CRC auto clear OFF, whitening and CRC disabled (not compatible), addressFiltering off.
  write_reg(REG_PACKETCONFIG2, RF_PACKETCONFIG2_WMBUS_CRC_DISABLE | RF_PACKETCONFIG2_DATAMODE_PACKET |
    RF_PACKETCONFIG2_IOHOME_OFF | RF_PACKETCONFIG2_BEACON_OFF); // packet mode
  write_reg(REG_PAYLOADLENGTH, 0x00); // unlimited length mode (in combination with PacketFormat = 0), so we can encode/decode length byte in software
  previous_payload_length = 0;
  write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | 0x03); // tx start condition true when there is at least one byte in FIFO (we are in standby/sleep when filling FIFO anyway)
                                   // For RX the threshold is set to 4 since this is the minimum length of a D7 packet (number of bytes in FIFO >= FifoThreshold + 1).

  write_reg(REG_SEQCONFIG1, RF_SEQCONFIG1_SEQUENCER_STOP | RF_SEQCONFIG1_IDLEMODE_STANDBY |
    RF_SEQCONFIG1_FROMSTART_TOLPS | RF_SEQCONFIG1_LPS_SEQUENCER_OFF | RF_SEQCONFIG1_FROMIDLE_TOTX |
    RF_SEQCONFIG1_FROMTX_TOLPS); // force off for now
  //  write_reg(REG_SEQCONFIG2, 0); // not used for now
  //  write_reg(REG_TIMERRESOL, 0); // not used for now
  //  write_reg(REG_TIMER1COEF, 0); // not used for now
  //  write_reg(REG_TIMER2COEF, 0); // not used for now
  //  write_reg(REG_IMAGECAL, 0); // TODO not used for now
  //  write_reg(REG_LOWBAT, 0); // TODO not used for now

  write_reg(REG_DIOMAPPING1, RF_DIOMAPPING1_DIO0_00 | RF_DIOMAPPING1_DIO1_00 |
    RF_DIOMAPPING1_DIO2_11 | RF_DIOMAPPING1_DIO3_00); // DIO2 = 0b11 => interrupt on sync detect 
  write_reg(REG_DIOMAPPING2, RF_DIOMAPPING2_DIO4_00 | RF_DIOMAPPING2_DIO5_11 |
    RF_DIOMAPPING2_MAP_RSSI); // ModeReady TODO configure for RSSI interrupt when doing CCA?
  //  write_reg(REG_PLLHOP, 0); // TODO might be interesting for channel hopping
  //  write_reg(REG_TCXO, 0); // default
  //  write_reg(REG_PADAC, 0); // default
  //  write_reg(REG_FORMERTEMP, 0); // not used for now
  //  write_reg(REG_BITRATEFRAC, 0); // default
  //  write_reg(REG_AGCREF, 0); // default, TODO validate
  //  write_reg(REG_AGCTHRESH1, 0); // not used for now
  //  write_reg(REG_AGCTHRESH2, 0); // not used for now
  //  write_reg(REG_AGCTHRESH3, 0); // not used for now
  //  write_reg(REG_PLL, 0); // not used for now

  // TODO
//  sx127x_set_tx_timeout(dev, SX127X_TX_TIMEOUT_DEFAULT);
//  sx127x_set_modem(dev, SX127X_MODEM_DEFAULT);
//  sx127x_set_channel(dev, SX127X_CHANNEL_DEFAULT);
//  sx127x_set_bandwidth(dev, SX127X_BW_DEFAULT);
//  sx127x_set_payload_length(dev, SX127X_PAYLOAD_LENGTH);
//  sx127x_set_tx_power(dev, SX127X_RADIO_TX_POWER);
  // TODO validate:
  // bitrate
  // GFSK settings
  // preamble detector
  // packet handler
  // sync word
  // packet len
  // CRC
  // whitening
  // PA

  // TODO burst write reg?
}

static inline int16_t get_rssi() {
  return (- read_reg(REG_RSSIVALUE) >> 1);
}

static void packet_transmitted_isr() {

  DEBUG_TX_END();
  DEBUG_FG_END();
  hw_busy_wait(110); // TO DO: OPTIMISE

  if(tx_packet_callback)
    tx_packet_callback(timer_get_counter_value());
}

static void bg_scan_rx_done() 
{

  //  assert(current_syncword_class == PHY_SYNCWORD_CLASS0);
   timer_tick_t rx_timestamp = timer_get_counter_value();
   DPRINT("BG packet received!");

   DEBUG_BG_END();

   current_packet = alloc_packet_callback(FskPacketHandler_sx127x.Size);
   assert(current_packet); // TODO handle
   current_packet->length = FskPacketHandler_sx127x.Size;

   read_fifo(current_packet->data, FskPacketHandler_sx127x.Size); //current_packet->data + 1

   current_packet->rx_meta.timestamp = rx_timestamp;
   current_packet->rx_meta.crc_status = HW_CRC_UNAVAILABLE;
   current_packet->rx_meta.rssi = get_rssi();
   current_packet->rx_meta.lqi = 0; // TODO

   rx_packet_callback(current_packet);
   flush_fifo(); 
}

static void rx_timeout(void *arg) {
  DEBUG_BG_END();
  DPRINT("RX timeout");
  hw_radio_set_idle();
}

static void dio0_isr(void *arg) {
  hw_gpio_disable_interrupt(SX127x_DIO0_PIN);  

  if(state == STATE_RX) {
    sched_post_task(&bg_scan_rx_done);
  } else {
    sched_post_task(&packet_transmitted_isr);
  }
}

static void set_packet_handler_enabled(bool enable) {
  write_reg(REG_PREAMBLEDETECT, (read_reg(REG_PREAMBLEDETECT) & RF_PREAMBLEDETECT_DETECTOR_MASK) | (enable << 7));
  write_reg(REG_SYNCCONFIG, (read_reg(REG_SYNCCONFIG) & RF_SYNCCONFIG_SYNC_MASK) | (enable << 4));
}

static void fifo_level_isr()
{
    uint8_t flags;

    hw_gpio_disable_interrupt(SX127x_DIO1_PIN);

    flags = read_reg(REG_IRQFLAGS2);
    // detect underflow
    if (flags & 0x08)
    {
        DPRINT("FlagsIRQ2: %x means that packet has been sent! ", flags);
        assert(false);
    }
    tx_refill_callback(remaining_bytes_len);
}

static void reinit_rx() {
 FskPacketHandler_sx127x.NbBytes = 0;
 FskPacketHandler_sx127x.Size = 0;
 FskPacketHandler_sx127x.FifoThresh = 0;

 write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | 0x03);
 write_reg(REG_DIOMAPPING1, RF_DIOMAPPING1_DIO2_11);
 previous_payload_length = 0;
 write_reg(REG_PAYLOADLENGTH, 0);

 // Trigger a manual restart of the Receiver chain (no frequency change)
 write_reg(REG_RXCONFIG, RF_RXCONFIG_RESTARTRXWITHOUTPLLLOCK | RF_RXCONFIG_AFCAUTO_ON| 
  RF_RXCONFIG_AGCAUTO_ON | RF_RXCONFIG_RXTRIGER_PREAMBLEDETECT);
 flush_fifo();

//  //DPRINT("Before enabling interrupt: FLAGS1 %x FLAGS2 %x\n", read_reg(REG_IRQFLAGS1), read_reg(REG_IRQFLAGS2));
 hw_gpio_set_edge_interrupt(SX127x_DIO1_PIN, GPIO_RISING_EDGE);
 hw_gpio_enable_interrupt(SX127x_DIO1_PIN);
}

// TODO
static void fifo_threshold_isr() {
 // TODO might be optimized. Initial plan was to read length byte and reconfigure threshold
 // based on the expected length so we can wait for next interrupt to read remaining bytes.
 // This doesn't seem to work for now however: the interrupt doesn't fire again for some unclear reason.
 // So now we do it as suggest in the datasheet: reading bytes from FIFO until FifoEmpty flag is set.
 // Reading more bytes at once might be more efficient, however getting the number of bytes in the FIFO seems
 // not possible at least in FSK mode (for LoRa, the register RegRxNbBytes gives the number of received bytes).
   hw_gpio_disable_interrupt(SX127x_DIO1_PIN);
   DPRINT("THR ISR with IRQ %x\n", read_reg(REG_IRQFLAGS2));
   assert(state == STATE_RX);

   if (FskPacketHandler_sx127x.Size == 0 && FskPacketHandler_sx127x.NbBytes == 0)
   {
       // For RX, the threshold is set to 4, so if the DIO1 interrupt occurs, it means that can read at least 4 bytes
       uint8_t rx_bytes = 0;
       uint8_t buffer[4];
       uint8_t backup_buffer[4];
       int16_t rssi = get_rssi();
       while(!(CHECK_FIFO_EMPTY()) && rx_bytes < 4)
       {
           buffer[rx_bytes++] = read_reg(REG_FIFO);
       }

       assert(rx_bytes == 4);

      memcpy(backup_buffer, buffer, rx_bytes);
       rx_packet_header_callback(buffer, rx_bytes);
       if(FskPacketHandler_sx127x.Size == 0) {
         DPRINT("Length was too large, discarding packet");
         reinit_rx();
         return;
       }

       current_packet = alloc_packet_callback(FskPacketHandler_sx127x.Size);
       if(current_packet == NULL) {
         DPRINT("Could not allocate package, discarding.");
         reinit_rx();
         return;
       }

       current_packet->rx_meta.rssi = rssi;
       memcpy(current_packet->data, backup_buffer, 4);
       current_packet->length = FskPacketHandler_sx127x.Size;

       FskPacketHandler_sx127x.NbBytes = 4;
   }

   if (FskPacketHandler_sx127x.FifoThresh)
   {
       read_fifo(&current_packet->data[FskPacketHandler_sx127x.NbBytes], FskPacketHandler_sx127x.FifoThresh);
       FskPacketHandler_sx127x.NbBytes += FskPacketHandler_sx127x.FifoThresh;
   }

   while(!(CHECK_FIFO_EMPTY()) && (FskPacketHandler_sx127x.NbBytes < FskPacketHandler_sx127x.Size))
      current_packet->data[FskPacketHandler_sx127x.NbBytes++] = read_reg(REG_FIFO);

   uint8_t remaining_bytes = FskPacketHandler_sx127x.Size - FskPacketHandler_sx127x.NbBytes;

   if(remaining_bytes == 0) {
    current_packet->rx_meta.timestamp = timer_get_counter_value();
    current_packet->rx_meta.crc_status = HW_CRC_UNAVAILABLE;
    current_packet->rx_meta.lqi = 0; // TODO

    // RSSI is measured during reception of the first part of the packet
    // to make sure we are actually measuring during a TX, instead of after

    // Restart the reception until upper layer decides to stop it
    reinit_rx(); // restart already before doing decoding so we don't miss packets on low clock speeds

    DEBUG_FG_END();

    rx_packet_callback(current_packet);

    return;
   }

   //Trigger FifoLevel interrupt
   if ( remaining_bytes > FIFO_SIZE)
   {
       write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | (BYTES_IN_RX_FIFO - 1));
       FskPacketHandler_sx127x.FifoThresh = BYTES_IN_RX_FIFO;
   } else {
       write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | (remaining_bytes - 1));
       FskPacketHandler_sx127x.FifoThresh = remaining_bytes;
   }

   hw_gpio_set_edge_interrupt(SX127x_DIO1_PIN, GPIO_RISING_EDGE);
   hw_gpio_enable_interrupt(SX127x_DIO1_PIN);

   DPRINT("read %i bytes, %i remaining, FLAGS2 %x, time: %i \n", FskPacketHandler_sx127x.NbBytes, remaining_bytes, read_reg(REG_IRQFLAGS2), timer_get_counter_value());
}

static void dio1_isr(void *arg) {
    DPRINT("DIO1_irq");

    hw_gpio_disable_interrupt(SX127x_DIO1_PIN);
    if(state == STATE_RX) {
      sched_post_task(&fifo_threshold_isr);
    } else {
      sched_post_task(&fifo_level_isr);
    }
}

static void restart_rx_chain() {
  // TODO restarting by triggering RF_RXCONFIG_RESTARTRXWITHPLLLOCK seems not to work
  // for some reason, when already in RX and after a freq change.
  // The chip is unable to receive on the new freq
  // For now the workaround is to go back to standby mode, to be optimized later
  set_opmode(OPMODE_STANDBY);

  // TODO for now we assume we need a restart with PLL lock.
  // this can be optimized for case where there is no freq change
  // write_reg(REG_RXCONFIG, read_reg(REG_RXCONFIG) | RF_RXCONFIG_RESTARTRXWITHPLLLOCK);
  DPRINT("restart RX chain with PLL lock");
}

static void calibrate_rx_chain() {
  // TODO currently assumes to be called on boot only
  DPRINT("Calibrating RX chain");
  assert(get_opmode() == OPMODE_STANDBY);
  uint8_t reg_pa_config_initial_value = read_reg(REG_PACONFIG);

  // Cut the PA just in case, RFO output, power = -1 dBm
  write_reg(REG_PACONFIG, 0x00);

  // We are not calibrating for LF band for now, this is done at POR already

  hw_radio_set_center_freq(863150000);   // Sets a Frequency in HF band

  write_reg(REG_IMAGECAL, RF_IMAGECAL_TEMPMONITOR_OFF | RF_IMAGECAL_IMAGECAL_START); // TODO temperature monitoring disabled for now
  while((read_reg(REG_IMAGECAL) & RF_IMAGECAL_IMAGECAL_RUNNING) == RF_IMAGECAL_IMAGECAL_RUNNING) { }

  write_reg(REG_PACONFIG, reg_pa_config_initial_value);
}

error_t hw_radio_init(hwradio_init_args_t* init_args) {
  alloc_packet_callback = init_args->alloc_packet_cb;
  release_packet_callback = init_args->release_packet_cb;
  rx_packet_callback = init_args->rx_packet_cb;
  rx_packet_header_callback = init_args->rx_packet_header_cb;
  tx_packet_callback = init_args->tx_packet_cb;
  tx_refill_callback = init_args->tx_refill_cb;

  if(sx127x_spi == NULL) {
    spi_handle = spi_init(SX127x_SPI_INDEX, SX127x_SPI_BAUDRATE, 8, true, false);
    sx127x_spi = spi_init_slave(spi_handle, SX127x_SPI_PIN_CS, true);
  }

  spi_enable(spi_handle);
  hw_radio_io_init();
  io_inited = true;
  hw_radio_reset();

  set_opmode(OPMODE_STANDBY);
  while(get_opmode() != OPMODE_STANDBY) {}

  uint8_t chip_version = read_reg(REG_VERSION);
  if(chip_version == 0x12) {
    DPRINT("Detected sx1276");
    is_sx1272 = false;
  } else if(chip_version == 0x22) {
    DPRINT("Detected sx1272");
    is_sx1272 = true;
  } else {
    assert(false);
  }


  calibrate_rx_chain();
  init_regs();

#ifdef PLATFORM_SX127X_USE_LOW_BAT_SHUTDOWN
  write_reg(REG_LOWBAT, read_reg(REG_LOWBAT) | (1 << 3) | 0x02);
#endif

  hw_radio_set_idle();

  error_t e;
  e = hw_gpio_configure_interrupt(SX127x_DIO0_PIN, GPIO_RISING_EDGE, &dio0_isr, NULL); assert(e == SUCCESS);
  e = hw_gpio_configure_interrupt(SX127x_DIO1_PIN, GPIO_RISING_EDGE, &dio1_isr, NULL); assert(e == SUCCESS);
  DPRINT("inited sx127x");

  sched_register_task(&rx_timeout);
  sched_register_task(&bg_scan_rx_done);
  sched_register_task(&packet_transmitted_isr);
  sched_register_task(&fifo_threshold_isr);
  sched_register_task(&fifo_level_isr);

  return SUCCESS; // TODO FAIL return code
}

void hw_radio_stop() {
  // TODO reset chip?
  hw_radio_set_idle();
}

error_t hw_radio_set_idle() {
    if(state == STATE_IDLE && !io_inited)
        return EALREADY;
    hw_radio_set_opmode(HW_STATE_SLEEP);
    if(FskPacketHandler_sx127x.Size - FskPacketHandler_sx127x.NbBytes != 0 && FskPacketHandler_sx127x.NbBytes != 0) {
      DPRINT("going to idle while still %i bytes to read.", FskPacketHandler_sx127x.Size - FskPacketHandler_sx127x.NbBytes);
      FskPacketHandler_sx127x.Size = 0;
      FskPacketHandler_sx127x.NbBytes = 0;
      release_packet_callback(current_packet);
    }
    sched_cancel_task(&fifo_threshold_isr);
    sched_cancel_task(&fifo_level_isr);
    sched_cancel_task(&bg_scan_rx_done);
    sched_cancel_task(&packet_transmitted_isr);
    timer_cancel_task(&rx_timeout);
    DPRINT("set to sleep at %i\n", timer_get_counter_value());
    DEBUG_RX_END();
    DEBUG_TX_END();
    DEBUG_BG_END();
    DEBUG_FG_END();
    return SUCCESS;
}

bool hw_radio_is_idle() {
  if(state != STATE_IDLE)
    return false;
  else
    return true;
}

hw_radio_state_t hw_radio_get_opmode(void) {
  switch(get_opmode()) {
    case OPMODE_TX:
      return HW_STATE_TX;
      break;
    case OPMODE_RX:
      return HW_STATE_RX;
      break;
    case OPMODE_SLEEP:
      return HW_STATE_SLEEP;
      break;
    case OPMODE_STANDBY:
      return HW_STATE_STANDBY;
      break;
    default:
      return HW_STATE_IDLE;
      break;
  }
}

void set_opmode(uint8_t opmode) {
  switch(opmode) {
    case OPMODE_SLEEP:
      state = STATE_IDLE;
      break;
    case OPMODE_RX:
      state = STATE_RX;
      break;
    case OPMODE_TX:
      state = STATE_TX;
      break;
    case OPMODE_STANDBY:
      state = STATE_STANDBY;
      break;
  }
  #if defined(PLATFORM_SX127X_USE_MANUAL_RXTXSW_PIN) || defined(PLATFORM_USE_ABZ)
  set_antenna_switch(opmode);
  #endif
  write_reg(REG_OPMODE, (read_reg(REG_OPMODE) & RF_OPMODE_MASK) | opmode);

  #ifdef PLATFORM_SX127X_USE_VCC_TXCO
  if(opmode == OPMODE_SLEEP)
    hw_gpio_clr(SX127x_VCC_TXCO);
  else
    hw_gpio_set(SX127x_VCC_TXCO);
  #endif
}

void set_state_rx() {
  if(get_opmode() >= OPMODE_FSRX || get_opmode() == OPMODE_SLEEP) {
    set_opmode(OPMODE_STANDBY); //Restart when changing freq/datarate
    while(!(read_reg(REG_IRQFLAGS1) & 0x80));
  }
  flush_fifo();

  write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | 0x03);
  write_reg(REG_DIOMAPPING1, RF_DIOMAPPING1_DIO2_11);

  FskPacketHandler_sx127x.FifoThresh = 0;
  FskPacketHandler_sx127x.NbBytes = 0;

  set_packet_handler_enabled(true);

  if(FskPacketHandler_sx127x.Size == 0) {
    hw_gpio_set_edge_interrupt(SX127x_DIO1_PIN, GPIO_RISING_EDGE);
    hw_gpio_enable_interrupt(SX127x_DIO1_PIN);
  } else {
    hw_gpio_set_edge_interrupt(SX127x_DIO0_PIN, GPIO_RISING_EDGE);
    hw_gpio_enable_interrupt(SX127x_DIO0_PIN);
  }

  set_opmode(OPMODE_RX);
}

void hw_radio_set_opmode(hw_radio_state_t opmode) {
  switch(opmode) {
    case HW_STATE_OFF:
    case HW_STATE_SLEEP:
      DEBUG_TX_END();
      DEBUG_RX_END();
      hw_gpio_disable_interrupt(SX127x_DIO0_PIN);
      hw_gpio_disable_interrupt(SX127x_DIO1_PIN);
      set_opmode(OPMODE_SLEEP);
      spi_disable(spi_handle);
      hw_radio_io_deinit();
      io_inited = false;
      break;
    case HW_STATE_STANDBY:
      set_opmode(OPMODE_STANDBY);
      break;
    case HW_STATE_TX:
      DEBUG_RX_END();
      DEBUG_TX_START();
      set_opmode(OPMODE_TX);
      break;
    case HW_STATE_IDLE:
    case HW_STATE_RX:
      DEBUG_RX_START();
      set_state_rx();
      break;
    case HW_STATE_RESET:
      hw_reset();
      break;
  }
}

void hw_radio_set_center_freq(uint32_t center_freq) {
  center_freq = (uint32_t)(center_freq / FREQ_STEP);

  write_reg(REG_FRFMSB, (uint8_t)((center_freq >> 16) & 0xFF));
  write_reg(REG_FRFMID, (uint8_t)((center_freq >> 8) & 0xFF));
  write_reg(REG_FRFLSB, (uint8_t)(center_freq & 0xFF));
}

void hw_radio_set_rx_bw_hz(uint32_t bw_hz) {
  uint8_t bw_exp_count, bw_mant_count;
  uint32_t computed_bw;
  uint32_t min_bw_dif = 10e6;
  uint8_t reg_bw;

  for(bw_exp_count = 1; bw_exp_count < 8; bw_exp_count++) {
    for(bw_mant_count = 16; bw_mant_count <= 24; bw_mant_count += 4) {
      computed_bw = SX127X_FXOSC / (bw_mant_count * (1 << (bw_exp_count + 2)));
      if(abs(computed_bw - bw_hz) < min_bw_dif) {
        min_bw_dif = abs(computed_bw - bw_hz);
        reg_bw = ((((bw_mant_count - 16) / 4) << 3) | bw_exp_count);
        rx_bw_number = (bw_exp_count - 1) * 3 + ((bw_mant_count - 16) >> 2);
      }
    }
  }

  write_reg(REG_RXBW, reg_bw);
  // set the same bandwidth for the AFC if AFC is enabled
  write_reg(REG_AFCBW, reg_bw);
}

void hw_radio_set_bitrate(uint32_t bps) {
  /* Bitrate(15,0) + (BitrateFrac / 16) = FXOSC / bps */
  uint16_t bps_downscaled = (uint16_t)(SX127X_FXOSC / bps); 
  
  write_reg_16(REG_BITRATEMSB, bps_downscaled);
}

void hw_radio_set_tx_fdev(uint32_t fdev) {
  /* Fdev(13,0) = Fdev / Fstep */
  uint16_t fdev_downscaled = fdev / FREQ_STEP;

  write_reg_16(REG_FDEVMSB, fdev_downscaled);
}

void hw_radio_set_preamble_size(uint16_t size) {
  if(size > 4) {
    write_reg(REG_PREAMBLEDETECT, RF_PREAMBLEDETECT_DETECTOR_ON | RF_PREAMBLEDETECT_DETECTORSIZE_3 | RF_PREAMBLEDETECT_DETECTORTOL_10);  
  } else {
    write_reg(REG_PREAMBLEDETECT, RF_PREAMBLEDETECT_DETECTOR_ON | RF_PREAMBLEDETECT_DETECTORSIZE_3 | RF_PREAMBLEDETECT_DETECTORTOL_15);  
  }
  write_reg_16(REG_PREAMBLEMSB, size);
}

void hw_radio_set_dc_free(uint8_t scheme) {
  write_reg(REG_PACKETCONFIG1, (read_reg(REG_PACKETCONFIG1) & RF_PACKETCONFIG1_DCFREE_MASK) | (scheme << 5));
}

void hw_radio_set_sync_word(uint8_t *sync_word, uint8_t sync_size) {
  //TODO: make sync word dependant on size
  uint16_t full_sync_word = *((const uint16_t *)sync_word);
  write_reg_16(REG_SYNCVALUE1, full_sync_word);
}

void hw_radio_set_crc_on(uint8_t enable) {
  write_reg(REG_PACKETCONFIG1, (read_reg(REG_PACKETCONFIG1) & RF_PACKETCONFIG1_CRC_MASK) | (enable << 4));
}

error_t hw_radio_send_payload(uint8_t * data, uint16_t len) {
  if(len == 0)
    return ESIZE;

#ifdef PLATFORM_SX127X_USE_LOW_BAT_SHUTDOWN
  /*activate low battery detector*/
  if(read_reg(REG_IRQFLAGS2) & 0x01){
    write_reg(REG_IRQFLAGS2, 0x01);
    hw_radio_set_idle();
    return;
  }
#endif
  
  if(state == STATE_RX) {
    hw_gpio_disable_interrupt(SX127x_DIO0_PIN);
    hw_gpio_disable_interrupt(SX127x_DIO1_PIN);
    set_opmode(OPMODE_STANDBY);
    while(!(read_reg(REG_IRQFLAGS1) & 0x80));
  }

  if(state == STATE_IDLE) { //Sleeping
    set_opmode(OPMODE_STANDBY);
    while(!(read_reg(REG_IRQFLAGS1) & 0x80));
  }

  uint8_t start = 0;
  uint8_t available_size = FIFO_SIZE - previous_threshold;
  if(remaining_bytes_len == 0)
    remaining_bytes_len = len;
  else
    start = len - remaining_bytes_len;

  write_reg(REG_DIOMAPPING1, 0x00); //FIFO LEVEL ISR or Packet Sent ISR

  if(remaining_bytes_len > available_size) {
    previous_threshold = FG_THRESHOLD;
    write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | FG_THRESHOLD);
    write_fifo(data + start, available_size);
    remaining_bytes_len = remaining_bytes_len - available_size;
    hw_gpio_set_edge_interrupt(SX127x_DIO1_PIN, GPIO_FALLING_EDGE);
    hw_gpio_enable_interrupt(SX127x_DIO1_PIN); 
  } else {
    if(!enable_refill) {
      previous_threshold = 0;
      write_fifo(data + start, remaining_bytes_len);
      remaining_bytes_len = 0;
      hw_gpio_set_edge_interrupt(SX127x_DIO0_PIN, GPIO_RISING_EDGE);
      hw_gpio_enable_interrupt(SX127x_DIO0_PIN); 
    } else {
      previous_threshold = 2;
      write_reg(REG_FIFOTHRESH, RF_FIFOTHRESH_TXSTARTCONDITION_FIFONOTEMPTY | 2);
      write_fifo(data + start, remaining_bytes_len);
      remaining_bytes_len = 0;
      hw_gpio_set_edge_interrupt(SX127x_DIO1_PIN, GPIO_FALLING_EDGE);
      hw_gpio_enable_interrupt(SX127x_DIO1_PIN); 
    }
  }

  set_packet_handler_enabled(true);

  if(!enable_preloading)
    hw_radio_set_opmode(HW_STATE_TX);
  else
    enable_preloading = false;

  return SUCCESS;
}

void hw_radio_set_payload_length(uint16_t length) {
  if(length > 0xFF) //Max length is 255 for this chip, return impossible length
    FskPacketHandler_sx127x.Size = 0;
  else {
    if(previous_payload_length != length) {
      write_reg(REG_PAYLOADLENGTH, length);
      previous_payload_length = length;
    }
    FskPacketHandler_sx127x.Size = length;
  }
}


bool hw_radio_is_rx(void) {
  return (hw_radio_get_opmode() == HW_STATE_RX);
}

void hw_radio_enable_refill(bool enable) {
  enable_refill = enable;
}

void hw_radio_enable_preloading(bool enable) {
  enable_preloading = enable;
}

void hw_radio_set_tx_power(int8_t eirp) {
  if(eirp < -5) {
    eirp = -5;
    DPRINT("The given eirp is too low, adjusted to %d dBm, offset excluded", eirp);
    // assert(eirp >= -5); // -4.2 dBm is minimum
  } 
#ifdef PLATFORM_SX127X_USE_PA_BOOST
 // Pout = 17-(15-outputpower)
  if(eirp > 20) {
    eirp = 20;
    DPRINT("The given eirp is too high, adjusted to %d dBm, offset excluded", eirp);
    // chip supports until +15 dBm default, +17 dBm with PA_BOOST and +20 dBm with PaDac enabled. 
  }
  if(eirp <= 5) {
    write_reg(REG_PACONFIG, (uint8_t)(eirp - 10.8 + 15));
    write_reg(REG_PADAC, (read_reg(REG_PADAC) & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_OFF); //Default Power
  } else if(eirp <= 15) {
    write_reg(REG_PACONFIG, 0x70 | (uint8_t)(eirp));
    write_reg(REG_PADAC, (read_reg(REG_PADAC) & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_OFF); //Default Power
  } else if(eirp <= 17) {
    write_reg(REG_PACONFIG, RF_PACONFIG_PASELECT_PABOOST | (eirp - 2));
    write_reg(REG_PADAC, (read_reg(REG_PADAC) & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_OFF); //Default Power
  } else {
    write_reg(REG_PACONFIG, RF_PACONFIG_PASELECT_PABOOST | (eirp - 5));
    write_reg(REG_PADAC, (read_reg(REG_PADAC) & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_ON);  //High Power
  }
#else
  // Pout = Pmax-(15-outputpower)
  if(eirp > 15) {
    eirp = 15;
    DPRINT("The given eirp is too high, adjusted to %d dBm, offset excluded", eirp);
    // assert(eirp <= 15); // Pmax = 15 dBm
  }
  if(eirp <= 5)
    write_reg(REG_PACONFIG, (uint8_t)(eirp - 10.8 + 15));
  else
    write_reg(REG_PACONFIG, 0x70 | (uint8_t)(eirp));
#endif

}

void hw_radio_set_rx_timeout(uint32_t timeout) {
  timer_post_task_delay(&rx_timeout, timeout);
}

__attribute__((weak)) void hw_radio_reset() {
  // needs to be implemented in platform for now (until we have a public API to configure GPIO pins)
}

__attribute__((weak)) void hw_radio_io_init() {
  // needs to be implemented in platform for now (until we have a public API to configure GPIO pins)
}

__attribute__((weak)) void hw_radio_io_deinit() {
  // needs to be implemented in platform for now (until we have a public API to configure GPIO pins)
}

int16_t hw_radio_get_rssi() {
    set_opmode(OPMODE_RX); //0.103 ms
    hw_gpio_disable_interrupt(SX127x_DIO0_PIN); //3.7µs
    hw_gpio_disable_interrupt(SX127x_DIO1_PIN);
    hw_busy_wait(rx_bw_startup_time[rx_bw_number]); //TODO: optimise this timing. Now it should wait for ~700µs but actually waits ~926µs (low rate)
    return (- read_reg(REG_RSSIVALUE) >> 1);
}
