# eddaemon

EEDD: An Event-Driven Daemon with CLI

EEDD is an empty event-driven daemon. It is empty in the
sense that the daemon itself provides just a command line
interface, leaving the real functionality of the daemon
to a set of loadable shared object libraries or plug-ins.
The "Empty Daemon" has several features that you might
might find useful for your next project.
  - Command line tools to view and set plug-in parameters
  - Simple publish/subscribe mechanism for sensor data
  - All commands and data are printable ASCII
  - Modular plug-ins for easy development and debug
  - No dependencies (excluding libc)
  - Ready-to-run, event-driven daemon


