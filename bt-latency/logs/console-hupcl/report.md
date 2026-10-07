# Console helper close behavior
Two clean-boot test setups lost console responsiveness after successful login,
before any usable radio test result. Failed evidence is retained under
logs/hci-ring-depth/runs/buffers10-rx32-tx4-1 and
logs/worker-affinity/runs/worker0-1.
Current host stty inspection found HUPCL enabled despite raw input/output.
All four raw UART helpers now clear HUPCL and retain CLOCAL/CREAD, raw115200,
and CR command endings. This prevents requesting a modem-line hangup at close.
No explicit DTR/RTS changes are added to the capture helpers.

Recovered through esptool read-mac (no flash writes), waited30s, logged in.
Five separately opened/closed board_command probes succeeded; uptime rose
34.41 to37.93s without reset. Final stty confirms-hupcl.
This removes a host-side modem-control hazard; it does not prove HUPCL caused
every earlier stall. Continued clean-boot tests will check recurrence.
