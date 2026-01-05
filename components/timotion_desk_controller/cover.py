import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from . import TimotionDeskControllerComponent, CONF_TIMOTION_DESK_CONTROLLER_ID

DEPENDENCIES = ["esp32", "timotion_desk_controller"]

# ESPHome 2025.x no longer exposes COVER_SCHEMA; _COVER_SCHEMA is the
# public way to extend the common cover options for custom platforms.
CONFIG_SCHEMA = cover._COVER_SCHEMA.extend(
    {
        cv.GenerateID(CONF_TIMOTION_DESK_CONTROLLER_ID): cv.use_id(TimotionDeskControllerComponent),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_TIMOTION_DESK_CONTROLLER_ID])
    await cover.register_cover(hub, config)
