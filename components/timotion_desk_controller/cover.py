import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover
from esphome.const import CONF_ID
from . import TimotionDeskControllerComponent, CONF_TIMOTION_DESK_CONTROLLER_ID

DEPENDENCIES = ["esp32", "timotion_desk_controller"]

# ESPHome 2025.x no longer exposes COVER_SCHEMA; _COVER_SCHEMA is the
# supported way to extend the common cover options for custom platforms.
CONFIG_SCHEMA = cover._COVER_SCHEMA.extend(
    {
        cv.GenerateID(CONF_TIMOTION_DESK_CONTROLLER_ID): cv.use_id(TimotionDeskControllerComponent),
    }
)


async def to_code(config):
    # "timotion_desk_controller_id" points at an existing TimotionDeskControllerComponent
    # instance that also subclasses cover.Cover. The cover registry, however,
    # expects an "id" key, so mirror the same ID there before registration.
    config[CONF_ID] = config[CONF_TIMOTION_DESK_CONTROLLER_ID]

    hub = await cg.get_variable(config[CONF_TIMOTION_DESK_CONTROLLER_ID])
    await cover.register_cover(hub, config)
