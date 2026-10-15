static const char *NQC_KEYWORDS[] = {
    "task", "sub", "inline", "if", "else", "while", "do", "for", "repeat",
    "until", "break", "continue", "return", "switch", "case", "default",
    "goto", "start", "stop", "monitor", "acquire", "catch", "asm", "abs",
    "sign", "int", "void", "const", "bool", "true", "false", NULL
};

static const char *NQC_BUILTINS[] = {
    "OnFwd", "OnRev", "OnFor", "On", "Off", "Float", "Toggle", "Fwd", "Rev",
    "SetOutput", "SetDirection", "SetPower", "Wait", "SetSensor",
    "SetSensorType", "SetSensorMode", "ClearSensor", "SensorValue",
    "SensorType", "SensorMode", "SensorValueBool", "SensorValueRaw",
    "PlaySound", "PlayTone", "MuteSound", "UnmuteSound", "ClearSound",
    "SelectDisplay", "SetWatch", "Watch", "ClearTimer", "Timer",
    "FastTimer", "SetTimer", "Random", "SetRandomSeed", "SendMessage",
    "ClearMessage", "Message", "SetTxPower", "SetSleepTime", "SleepNow",
    "StopAllTasks", "CreateDatalog", "AddToDatalog", "UploadDatalog",
    "SetGlobalOutput", "SetGlobalDirection", "SetMaxPower", "Program",
    "BatteryLevel", "Counter", "ClearCounter", "IncCounter", "DecCounter",
    "OUT_A", "OUT_B", "OUT_C", "OUT_FULL", "OUT_HALF", "OUT_LOW", "OUT_ON",
    "OUT_OFF", "OUT_FLOAT", "OUT_FWD", "OUT_REV", "OUT_TOGGLE", "SENSOR_1",
    "SENSOR_2", "SENSOR_3", "SENSOR_TYPE_NONE", "SENSOR_TYPE_TOUCH",
    "SENSOR_TYPE_TEMPERATURE", "SENSOR_TYPE_LIGHT", "SENSOR_TYPE_ROTATION",
    "SENSOR_MODE_RAW", "SENSOR_MODE_BOOL", "SENSOR_MODE_EDGE",
    "SENSOR_MODE_PULSE", "SENSOR_MODE_PERCENT", "SENSOR_MODE_CELSIUS",
    "SENSOR_MODE_FAHRENHEIT", "SENSOR_MODE_ROTATION", "SENSOR_TOUCH",
    "SENSOR_LIGHT", "SENSOR_ROTATION", "SENSOR_CELSIUS",
    "SENSOR_FAHRENHEIT", "SENSOR_PULSE", "SENSOR_EDGE", "SOUND_CLICK",
    "SOUND_DOUBLE_BEEP", "SOUND_DOWN", "SOUND_UP", "SOUND_LOW_BEEP",
    "SOUND_FAST_UP", "DISPLAY_WATCH", "DISPLAY_SENSOR_1",
    "DISPLAY_SENSOR_2", "DISPLAY_SENSOR_3", "DISPLAY_OUT_A",
    "DISPLAY_OUT_B", "DISPLAY_OUT_C", "TX_POWER_LO", "TX_POWER_HI", NULL
};
