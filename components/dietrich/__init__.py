import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_OPENING,
    DEVICE_CLASS_RUNNING,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_HOUR,
    UNIT_MINUTE,
    UNIT_PERCENT,
    UNIT_REVOLUTIONS_PER_MINUTE,
)

CODEOWNERS = ["@kakaki"]
AUTHORS = ["@kakaki", "Claude Fable 5 (Anthropic)"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]
MULTI_CONF = False

dietrich_ns = cg.esphome_ns.namespace("dietrich")
Dietrich = dietrich_ns.class_("Dietrich", cg.PollingComponent, uart.UARTDevice)

CONF_VARIANT = "variant"
DietrichVariant = dietrich_ns.enum("DietrichVariant")
VARIANTS = {
    "mcr3": DietrichVariant.DIETRICH_VARIANT_MCR3,
    "calenta_v1_p5": DietrichVariant.DIETRICH_VARIANT_CALENTA_V1_P5,
    "pcu05_p3": DietrichVariant.DIETRICH_VARIANT_PCU05_P3,
}


def _temp_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _temp0_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _percent_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _rpm_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_REVOLUTIONS_PER_MINUTE,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _index_schema():
    # Recom's PCU-05 P3 map gives "Airflow set" (byte 22) and "Airflow"
    # (byte 24) no unit at all - only "Fan speed" (byte 44) is labelled Rpm.
    # The two are not the same scale, so don't claim rpm for the airflow pair.
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    )


def _hours_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_HOUR,
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    )


def _count_schema():
    return sensor.sensor_schema(
        accuracy_decimals=0,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    )


def _param_temp_schema():
    # Stored parameters are whole-degree settings, not measurements, so they get
    # no state_class - they would only clutter long-term statistics.
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_TEMPERATURE,
    )


def _param_percent_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
    )


def _param_minutes_schema():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_MINUTE,
        accuracy_decimals=0,
    )


def _flag_schema(device_class=None):
    # Status bits from the sample block. Recom renders these as yes/no,
    # open/closed or off/on depending on the field; a device_class is only set
    # where the polarity was confirmed against live burn data, so nothing here
    # asserts a meaning that has not actually been observed on the bus.
    if device_class is None:
        return binary_sensor.binary_sensor_schema()
    return binary_sensor.binary_sensor_schema(device_class=device_class)


def _raw_schema():
    return sensor.sensor_schema(accuracy_decimals=0)


SENSOR_SCHEMAS = {
    # frame status/state
    "state": _raw_schema(),
    "sub_state": _raw_schema(),
    "lockout": _raw_schema(),
    "blocking": _raw_schema(),
    # sample data - temperatures
    "flow_temp": _temp_schema(),
    "return_temp": _temp_schema(),
    "dhw_in_temp": _temp_schema(),
    "outside_temp": _temp_schema(),
    "calorifier_temp": _temp_schema(),
    "boiler_control_temp": _temp_schema(),
    "room_temp": _temp_schema(),
    "ch_setpoint": _temp_schema(),
    "dhw_setpoint": _temp_schema(),
    "room_temp_setpoint": _temp_schema(),
    # fan - see _index_schema(); these two are the unitless airflow pair
    "fan_speed_setpoint": _index_schema(),
    "fan_speed": _index_schema(),
    # power / misc
    "ionisation_current": sensor.sensor_schema(
        unit_of_measurement="µA",
        accuracy_decimals=1,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # Byte 27, decoded as sgn(A.0 + B.1) x 0.01 - Recom lists it as "Internal
    # setpoint" in degrees C, not a percentage. It is the boiler's live target
    # flow temperature and jumps to ~90 C at ignition before settling.
    "internal_setpoint": _temp_schema(),
    "available_power": _percent_schema(),
    "pump_percentage": _percent_schema(),
    "desired_max_power": _percent_schema(),
    "actual_power": _percent_schema(),
    # misc sample data
    "hydro_pressure": sensor.sensor_schema(
        unit_of_measurement="bar",
        accuracy_decimals=2,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    "control_temp": _temp_schema(),
    "dhw_flowrate": sensor.sensor_schema(
        unit_of_measurement="l/min",
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    # pcu05_p3 additions
    "fan_speed_rpm": _rpm_schema(),
    "su_state": _raw_schema(),
    "su_locking": _raw_schema(),
    "su_blocking": _raw_schema(),
    "solar_temp": _temp_schema(),
    "hmi_active": _raw_schema(),
    "ch_setpoint_hmi": _temp0_schema(),
    "dhw_setpoint_hmi": _temp0_schema(),
    "service_mode": _raw_schema(),
    "rs232_mode": _raw_schema(),
    # counter data 1
    "hours_run_pump": _hours_schema(),
    "hours_run_3way": _hours_schema(),
    "hours_run_ch": _hours_schema(),
    "hours_run_dhw": _hours_schema(),
    "power_supply_aval_hours": _hours_schema(),
    "pump_starts": _count_schema(),
    "number_of_3way_valve_cycles": _count_schema(),
    "burner_start_dhw": _count_schema(),
    # counter data 2
    "total_burner_start": _count_schema(),
    "failed_burner_start": _count_schema(),
    "number_flame_loss": _count_schema(),
    # Stored parameters, read from EEPROM blocks 0x14..0x1B rather than from the
    # sample block - pcu05_p3 only. Configuring any of these makes the component
    # add an eight-request parameter sweep, once at boot and hourly after that.
    # The four curve parameters describe a straight line through two points:
    #   flow = p26 + (p1 - p26) x (p25 - T_outside) / (p25 - p27)
    "param_ch_max_flow": _param_temp_schema(),  # p1
    "param_dhw_setpoint": _param_temp_schema(),  # p2
    "param_pump_post_run": _param_minutes_schema(),  # p5, 99 = continuous
    "param_max_flow_system": _param_temp_schema(),  # p23
    "param_curve_foot_outside": _param_temp_schema(),  # p25
    "param_curve_foot_flow": _param_temp_schema(),  # p26
    "param_curve_cold_outside": _param_temp_schema(),  # p27, negative
    "param_pump_ch_min": _param_percent_schema(),  # p28
    "param_pump_ch_max": _param_percent_schema(),  # p29
    "param_dhw_hysteresis": _param_temp_schema(),  # p33
}

# Status bits. valve_bit0/bit6 and pump_bit0/bit1/bit2 carry a device_class
# because their polarity was verified against two logged CH burns; the rest are
# left plain rather than guess at open-vs-closed semantics.
BINARY_SENSOR_SCHEMAS = {
    # byte 36 - heat demand sources
    "demand_source_bit0": _flag_schema(),  # Mod.Controller Connected
    "demand_source_bit1": _flag_schema(),  # Heat demand from Mod.Controller
    "demand_source_bit2": _flag_schema(),  # Heat demand from on/off controller
    "demand_source_bit3": _flag_schema(),  # Frost Protection
    "demand_source_bit4": _flag_schema(),  # DHW Eco
    "demand_source_bit5": _flag_schema(),  # DHW Blocking
    "demand_source_bit6": _flag_schema(),  # Anti Legionella
    "demand_source_bit7": _flag_schema(),  # DHW Heat Demand
    # byte 37 - inputs
    "input_bit0": _flag_schema(),  # Shutdown Input
    "input_bit1": _flag_schema(),  # Release Input
    "input_bit2": _flag_schema(),  # Ionisation
    "input_bit3": _flag_schema(),  # Flow Switch detecting DHW
    "input_bit5": _flag_schema(),  # Min Gas Pressure
    "input_bit6": _flag_schema(),  # CH Enable
    "input_bit7": _flag_schema(),  # DHW Enable
    # byte 38 - valves
    "valve_bit0": _flag_schema(DEVICE_CLASS_OPENING),  # Gas Valve
    "valve_bit2": _flag_schema(),  # Ignition
    "valve_bit3": _flag_schema(),  # 3-Way valve position: off = CH, on = DHW
    "valve_bit4": _flag_schema(),  # Ext. 3-Way Valve
    "valve_bit6": _flag_schema(DEVICE_CLASS_OPENING),  # Ext. Gas Valve
    # byte 39 - pumps
    "pump_bit0": _flag_schema(DEVICE_CLASS_RUNNING),  # Pump
    "pump_bit1": _flag_schema(DEVICE_CLASS_RUNNING),  # Calorifier Pump
    "pump_bit2": _flag_schema(DEVICE_CLASS_RUNNING),  # Ext. CH Pump
    "pump_bit4": _flag_schema(),  # Status Report
    "pump_bit7": _flag_schema(),  # Opentherm SmartPower
    # byte 50 - misc flags
    "hru": _flag_schema(),
    "ch_timer_enable": _flag_schema(),
    "dhw_timer_enable": _flag_schema(),
}

# decoded text for the status/locking/blocking code registers
TEXT_SENSOR_SCHEMAS = {
    "state_text": text_sensor.text_sensor_schema(),
    "sub_state_text": text_sensor.text_sensor_schema(),
    "lockout_text": text_sensor.text_sensor_schema(),
    "blocking_text": text_sensor.text_sensor_schema(),
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Dietrich),
            cv.Optional(CONF_VARIANT, default="mcr3"): cv.enum(VARIANTS, lower=True),
            **{cv.Optional(key): schema for key, schema in SENSOR_SCHEMAS.items()},
            **{cv.Optional(key): schema for key, schema in BINARY_SENSOR_SCHEMAS.items()},
            **{cv.Optional(key): schema for key, schema in TEXT_SENSOR_SCHEMAS.items()},
        }
    )
    .extend(cv.polling_component_schema("15s"))
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_variant(config[CONF_VARIANT]))

    for key in SENSOR_SCHEMAS:
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(sens))

    for key in BINARY_SENSOR_SCHEMAS:
        if key in config:
            bs = await binary_sensor.new_binary_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_binary_sensor")(bs))

    for key in TEXT_SENSOR_SCHEMAS:
        if key in config:
            txt = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, f"set_{key}_sensor")(txt))
