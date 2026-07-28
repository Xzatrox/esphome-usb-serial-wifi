import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["wifi"]

usb_ip_ns = cg.esphome_ns.namespace("usb_ip")
USBIPComponent = usb_ip_ns.class_("USBIPComponent", cg.Component)

CONF_VID = "vid"
CONF_PID = "pid"

CONFIG_SCHEMA = cv.COMPONENT_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(USBIPComponent),
        cv.Optional(CONF_PORT, default=3240): cv.port,
        cv.Required(CONF_VID): cv.hex_uint16_t,
        cv.Required(CONF_PID): cv.hex_uint16_t,
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_vid(config[CONF_VID]))
    cg.add(var.set_pid(config[CONF_PID]))
