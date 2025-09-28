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

#endif
