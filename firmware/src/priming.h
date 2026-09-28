#pragma once

// Manual override that force-opens every valve regardless of what the print
// engine is scanning, for flushing/priming the system before a print run.
// Set by the web server (on WEB_CORE) and read every cycle by the print
// engine (print_engine_run(), on VALVE_CORE), so it's a plain atomic rather
// than mutex-protected like the rest of the shared state in this project.

void priming_start();
void priming_stop();
bool priming_is_active();
