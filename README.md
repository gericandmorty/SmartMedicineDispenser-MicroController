# Scheduled buzzer and compartment lights

Upload the updated sketches to all four controllers: `arduino`, `esp1`, `esp2`,
and `esp32-mini-code`. Arduino-only flashing will not update the drawer lights.

- A scheduled Arduino alarm beeps 1 second on / 1 second off for up to **5 minutes**.
  Opening the alerting drawer silences it early. The pickup window and LCD countdown also end after 5 minutes. The startup beep remains 10 seconds,
  and the manual buzzer test remains 2 seconds.
- Opening a drawer to retrieve a due dose, or confirming a dose while a drawer
  remains open, starts the cabinet-close warning. It repeats indefinitely until
  **both** reed switches confirm stable closure. New alarms and manual buzzer
  tests cannot end it. Arduino `/status` reports both door states and
  `cabinet_close_required` for the app's button-free popup.
- The existing pin 7 relay keeps a 250 ms on/off warning. Happy Birthday plays
  repeatedly on a **separate passive piezo output, pin 6**, while the warning is
  active. Use a suitable piezo/driver and common ground; never feed tone pulses
  into the mechanical relay input. The existing fixed-pitch relay buzzer alone
  cannot play this melody. Pin 6 stops immediately on confirmed closure.
- A missed dose also starts the faster 250 ms on / 250 ms off warning for up
  to 30 seconds, including when a newer scheduled dose takes over. The relay
  changes beep rhythm; the buzzer's actual pitch is fixed.
- Saving a schedule does not illuminate its compartment. With the drawer open,
  only the current due compartment blinks. Closed drawers keep their lights off.
- The latest due occurrence replaces the previous indication, including across
  drawers. For example, at 11:45 only the 11:45 compartment is indicated; at 11:46
  only the 11:46 compartment is indicated; at 11:47 only the 11:47 compartment is
  indicated. An uncollected older dose is recorded as missed when the next dose starts.
- Pickup is confirmed by opening and then closing the drawer, or by an explicit
  confirmation request. Its light clears and stays off if the drawer is reopened.
  The light also expires after the 5-minute alert window.
- `date=YYYY-MM-DD` schedules fire once on that local date. An omitted/empty `date`
  repeats daily, once per day at its time. Time matching uses GMT+8.
- Re-saving an unchanged schedule with the same ID preserves its fired state.
  The ESP32 master persists that state in flash. Arduino direct schedules and
  their fired state remain in RAM, as before, and must be restored after reboot.

Use a single compartment per alarm request. If multiple schedules share the same
minute, the last matching schedule becomes the current indication.

## Verification

Run `python3 microcontroller/tests/run_behavior_tests.py` from the project root.
The tests compile firmware logic using `g++` with simulated time, GPIO, and
network/storage boundaries. They cover consecutive alarm times, cross-drawer
selection, pickup, retries, daily recurrence, date restrictions, master flash
restoration, and buzzer/LED expiry. They do not replace board compilation or a
physical check of the reed switches, relay, buzzer, and LEDs.

For a bench check, schedule three different compartments one minute apart. Open
and close the correct drawer at each due time, then reopen it to verify that the
collected occurrence stays dark. Also leave an alarm uncollected to check that
the next due time records it as missed and starts the new alarm. With no later
alarm, the active dose expires after 5 minutes.

- When all due doses in a drawer are collected, its buzzer plays three short
  100ms chirps. This is a distinct rhythm from the normal 1s-on/1s-off alarm.
  The relay cannot change the buzzer's actual pitch.

## Missed medicine and completion

A missed dose sets `unresolved_missed_dose` on Arduino `/status`. It silences the
missed-dose warning and subsequent due-dose buzzer/vibration until the missed
medicine is reviewed in the app, which calls Arduino `/ack-missed`. Later doses
remain due on the display and their compartment lights. Taking every current
dose with no outstanding missed medicine plays the completion chime and writes
`ALL MEDS TAKEN!` on the LCD. A buzzer cannot speak those words; audible speech
would require a speaker or audio module. The cabinet-close safety warning still
takes priority until both reed switches confirm closure.
