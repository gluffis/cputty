# Only the null/loop test backends remain here: the Telnet, Rlogin,
# SUPDUP and Raw protocol backends have been removed.
add_sources_from_current_dir(otherbackends
  testback.c)
