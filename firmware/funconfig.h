#ifndef _FUNCONFIG_H
#define _FUNCONFIG_H

#define FUNCONF_USE_DEBUGPRINTF 1

/* Display orientation config
 * 0 = normal (default)
 * 1 = rotate 180 degrees
 * You may override via compiler flag: -DDISPLAY_ORIENTATION_180=1
 */
#ifndef DISPLAY_ORIENTATION_180
#define DISPLAY_ORIENTATION_180 0
#endif

/* No extra biases are needed with software rotation. */

/* INA226 I2C address selector
 * 0 = ADR strapped low  -> 0x40 (default)
 * 1 = ADR strapped high -> 0x44
 * You may override via compiler flag: -DINA226_ADDR_SELECT_0X44=1 or -DINA226_I2C_ADDR=0x44
 */
#ifndef INA226_ADDR_SELECT_0X44
#define INA226_ADDR_SELECT_0X44 0
#endif

#ifndef INA226_I2C_ADDR
#if INA226_ADDR_SELECT_0X44
#define INA226_I2C_ADDR 0x44u
#else
#define INA226_I2C_ADDR 0x40u
#endif
#endif

#endif
