/*
 * Determine whether or not a Conf represents a session which can
 * sensibly be launched right now.
 */

#include "putty.h"

bool conf_launchable(Conf *conf)
{
    return conf_get_str(conf, CONF_host)[0] != 0;
}
