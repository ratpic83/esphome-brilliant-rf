"""Control Brilliant / LAMDEN 2.4 GHz RGB+CCT ceiling lamps with an nRF24L01+.

The stock remote (Brilliant 98770/05, LAMDEN "CCT+RGB-31Key", Powerlink PL51WT020 SoC) speaks a
PL1167/LT8900-style packet format at 1 Mbit/s. This component builds those packets on an nRF24,
listens to the original remote to stay in sync, and exposes the lamps as Home Assistant lights.
See docs/PROTOCOL.md for the packet format.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import spi
from esphome.const import CONF_CHANNEL, CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["spi"]
MULTI_CONF = False

CONF_CE_PIN = "ce_pin"
CONF_SYNC_WORD = "sync_word"
CONF_LAMPS = "lamps"
CONF_REMOTE_ID = "remote_id"
CONF_REPEATS = "repeats"
CONF_LOG_PACKETS = "log_packets"
CONF_SCAN = "scan"
CONF_SCAN_FIRST = "first_channel"
CONF_SCAN_LAST = "last_channel"
CONF_SCAN_DWELL = "dwell_time"

brilliant_rf_ns = cg.esphome_ns.namespace("brilliant_rf")
BrilliantRF = brilliant_rf_ns.class_("BrilliantRF", cg.Component, spi.SPIDevice)

REMOTE_ID = cv.All([cv.hex_uint8_t], cv.Length(min=5, max=5))

LAMP_SCHEMA = cv.Schema(
    {
        # The 5 bytes that identify the paired remote (found with the remote discovery log)
        cv.Required(CONF_REMOTE_ID): REMOTE_ID,
    }
)

SCAN_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_SCAN_FIRST, default=0): cv.int_range(0, 125),
        cv.Optional(CONF_SCAN_LAST, default=83): cv.int_range(0, 125),
        cv.Optional(CONF_SCAN_DWELL, default="2ms"): cv.positive_time_period_milliseconds,
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BrilliantRF),
            cv.Required(CONF_CE_PIN): pins.gpio_output_pin_schema,
            # RF channel; frequency is 2400 + channel MHz. The 98770/05 remote uses 50.
            cv.Optional(CONF_CHANNEL, default=50): cv.int_range(0, 125),
            # PL1167 sync word as it appears on air (MSB first, as an nRF24 reads it)
            cv.Optional(CONF_SYNC_WORD, default=[0x8C, 0x42, 0xF3, 0x7B]): cv.All(
                [cv.hex_uint8_t], cv.Length(min=4, max=4)
            ),
            cv.Optional(CONF_LAMPS, default=[]): cv.ensure_list(LAMP_SCHEMA),
            # Copies of each packet on air; the original remote sends about 70 per key press
            cv.Optional(CONF_REPEATS, default=40): cv.int_range(1, 200),
            # Log every packet with the sync word (raw hex) - for protocol work only
            cv.Optional(CONF_LOG_PACKETS, default=False): cv.boolean,
            # Hop across channels instead of listening on `channel` (to find a remote's channel)
            cv.Optional(CONF_SCAN): SCAN_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(spi.spi_device_schema(cs_pin_required=True))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await spi.register_spi_device(var, config)
    ce = await cg.gpio_pin_expression(config[CONF_CE_PIN])
    cg.add(var.set_ce_pin(ce))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_sync_word(config[CONF_SYNC_WORD]))
    cg.add(var.set_repeats(config[CONF_REPEATS]))
    cg.add(var.set_log_packets(config[CONF_LOG_PACKETS]))
    for lamp in config[CONF_LAMPS]:
        cg.add(var.add_lamp(lamp[CONF_REMOTE_ID]))
    if CONF_SCAN in config:
        scan = config[CONF_SCAN]
        cg.add(
            var.set_scan(
                scan[CONF_SCAN_FIRST],
                scan[CONF_SCAN_LAST],
                scan[CONF_SCAN_DWELL].total_milliseconds,
            )
        )
