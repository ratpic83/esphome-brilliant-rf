"""Light platform: one lamp has a white part (brightness + colour temperature) and an RGB ring.

The lamp sends no feedback, so the Home Assistant state is the last state that was sent.
Use `default_transition_length: 0s` - every intermediate step would be a radio packet.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import (
    CONF_COLD_WHITE_COLOR_TEMPERATURE,
    CONF_OUTPUT_ID,
    CONF_TYPE,
    CONF_WARM_WHITE_COLOR_TEMPERATURE,
)

from . import BrilliantRF, brilliant_rf_ns

DEPENDENCIES = ["brilliant_rf"]

CONF_BRILLIANT_RF_ID = "brilliant_rf_id"
CONF_LAMP = "lamp"

BrilliantLight = brilliant_rf_ns.class_("BrilliantLight", light.LightOutput, cg.Component)

CONFIG_SCHEMA = light.RGB_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(BrilliantLight),
        cv.GenerateID(CONF_BRILLIANT_RF_ID): cv.use_id(BrilliantRF),
        # Index into the `lamps:` list of the brilliant_rf component
        cv.Optional(CONF_LAMP, default=0): cv.int_range(0, 15),
        cv.Required(CONF_TYPE): cv.one_of("white", "rgb", lower=True),
        cv.Optional(CONF_COLD_WHITE_COLOR_TEMPERATURE, default="6500K"): cv.color_temperature,
        cv.Optional(CONF_WARM_WHITE_COLOR_TEMPERATURE, default="2700K"): cv.color_temperature,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    parent = await cg.get_variable(config[CONF_BRILLIANT_RF_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_lamp(config[CONF_LAMP]))
    cg.add(var.set_rgb(config[CONF_TYPE] == "rgb"))
    cg.add(
        var.set_mireds(
            config[CONF_COLD_WHITE_COLOR_TEMPERATURE],
            config[CONF_WARM_WHITE_COLOR_TEMPERATURE],
        )
    )
