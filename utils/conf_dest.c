/*
 * Return the destination field of a Conf. (There used to be a choice
 * between host name and serial line; now only the host name remains.)
 */

#include "putty.h"

char const *conf_dest(Conf *conf)
{
    return conf_get_str(conf, CONF_host);
}

