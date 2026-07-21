AZEROTH AUTOFARM 1.1.1
======================

A standalone World of Warcraft 3.3.5a control panel for the AzerothCore
mod-autofarm server module. No third-party addon libraries are required.

INSTALLATION
------------

1. Exit World of Warcraft completely.
2. Copy the AzerothAutofarm folder into:

   World of Warcraft\Interface\AddOns\

3. The final path must be:

   World of Warcraft\Interface\AddOns\AzerothAutofarm\AzerothAutofarm.toc

4. Start the client. On the character screen, open AddOns and make sure
   Azeroth Autofarm is enabled.
5. Log into the non-progression realm running mod-autofarm.
6. Type /autofarm or click the new minimap button.

USAGE
-----

- Target an online playerbot and click Use Target, or type its exact name.
- Select a material preset, or enter an item name, ID, or shift-clicked link.
- Set a quantity goal. Zero means unlimited.
- Click Start Farming.
- Use Status, Stop, or Stop All to manage sessions.
- Right-click material rows to add them to Favorites.
- Click Activity to see commands and server replies.
- Keep Activity open to monitor the configured altbot. The dashboard refreshes
  one lightweight server snapshot every 15 seconds and stops polling when the
  window is closed.
- The Activity dashboard shows the bot's state, health, bags, location,
  material progress, current source, route position, distance, elapsed time,
  gathering rate, and movement-recovery information.

SLASH COMMANDS
--------------

/autofarm        Toggle the main window
/afarm           Toggle the main window
/afarm log       Toggle the activity log
/afarm help      Open the quick guide
/afarm status    Request status for the configured/selected bot
/afarm minimap   Show or hide the minimap button

NOTES
-----

- The addon is a user interface. The server module performs all routing,
  movement, combat, gathering, looting, and session management.
- Activity monitoring does not scan units, maps, combat logs, or inventory on
  the client. Automatic refresh can be disabled from the Activity window.
- The main, Activity, and Help windows automatically scale to fit the current
  display resolution and UI scale while preserving their internal layout.
- The playerbot must already be online through mod-playerbots.
- Gathering professions, skill, and required tools are still required.
- Fishing presets only work when an outdoor fishing-school source exists.
- Custom crafted-only, vendor-only, container-only, or open-water-only items
  may be rejected by the server because they have no supported farm source.
